/* Minecraft rule parsing and evaluation. */

#include "nuvasha-minecraft-versions-json-private.h"
#include "nuvasha-json.h"

#include <regex.h>
#include <sys/utsname.h>

#define MAX_VERSION_REGEX_LENGTH 1024
#define MAX_VERSION_VALUE_LENGTH 1024
#include <string.h>

NuvashaRule *
nuvasha_mc_parse_rule (JsonObject *object, GError **error)
{
  NuvashaRule *rule = g_new0 (NuvashaRule, 1);
  JsonObject *os = NULL;
  JsonObject *features = NULL;
  g_autofree char *action = NULL;

  if (!nuvasha_mc_get_string (object, "action", TRUE, &action, error))
    goto fail;

  if (g_str_equal (action, "allow"))
    rule->allow = TRUE;
  else if (g_str_equal (action, "disallow"))
    rule->allow = FALSE;
  else
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "unknown rule action \"%s\"", action);
      goto fail;
    }

  if (!nuvasha_mc_get_object (object, "os", FALSE, &os, error))
    goto fail;

  if (os != NULL)
    {
      /* "versionRange" is a window of Windows versions: it cannot
       * apply to nuvasha, which only ever runs on Linux, and is left out
       * on purpose. "version" is a regular expression and is
       * evaluated below. */
      if (!nuvasha_mc_get_string (os, "name", FALSE, &rule->os_name, error) ||
          !nuvasha_mc_get_string (os, "arch", FALSE, &rule->os_arch, error) ||
          !nuvasha_mc_get_string (os, "version", FALSE, &rule->os_version, error))
        goto fail;
    }

  if (!nuvasha_mc_get_object (object, "features", FALSE, &features, error))
    goto fail;

  if (features != NULL)
    {
      JsonObjectIter iter;
      const char *feature_name = NULL;
      JsonNode *feature_value = NULL;

      rule->features = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);
      json_object_iter_init (&iter, features);

      while (json_object_iter_next (&iter, &feature_name, &feature_value))
        {
          if (!JSON_NODE_HOLDS_VALUE (feature_value) ||
              json_node_get_value_type (feature_value) != G_TYPE_BOOLEAN)
            {
              g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                           "feature \"%s\" must be true or false",
                           feature_name);
              goto fail;
            }

          /* The value the rule asks for, both ways round: a rule that
           * wants a feature off matches as long as nobody turned it
           * on. */
          g_hash_table_replace (rule->features, g_strdup (feature_name),
                                GINT_TO_POINTER (json_node_get_boolean (
                                                   feature_value) ? 1 : 0));
        }
    }

  return rule;

fail:
  nuvasha_mc_free_rule (rule);
  return NULL;
}

/* NULL only on error, with @error set. */
GPtrArray *
nuvasha_mc_parse_rules (JsonNode *node, GError **error)
{
  GPtrArray *rules;
  JsonArray *array;

  if (!JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                   "\"rules\" must be an array");
      return NULL;
    }

  array = json_node_get_array (node);
  rules = g_ptr_array_new_with_free_func (nuvasha_mc_free_rule);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *item = json_array_get_element (array, i);
      NuvashaRule *rule;

      if (!JSON_NODE_HOLDS_OBJECT (item))
        {
          g_set_error (error, nuvasha_MC_ERROR, nuvasha_MC_ERROR_INVALID,
                       "rule %u is not an object", i);
          g_ptr_array_unref (rules);
          return NULL;
        }

      rule = nuvasha_mc_parse_rule (json_node_get_object (item), error);

      if (rule == NULL)
        {
          g_ptr_array_unref (rules);
          return NULL;
        }

      g_ptr_array_add (rules, rule);
    }

  return rules;
}

/* *rules is NULL when the member is not there, which is what "this
 * thing has no rules" looks like in the format. */
gboolean
nuvasha_mc_get_rules_member (JsonObject  *object,
                  const char  *member,
                  GPtrArray  **rules,
                  GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *rules = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  *rules = nuvasha_mc_parse_rules (node, error);
  return *rules != NULL;
}

/* x86_64 and amd64 are the same machine, and so are aarch64 and
 * arm64; the kernel and Mojang do not agree on which name to use. */
static gboolean
arch_equals (const char *one, const char *two)
{
  if (g_ascii_strcasecmp (one, two) == 0)
    return TRUE;

  return (g_ascii_strcasecmp (one, "amd64") == 0 &&
          g_ascii_strcasecmp (two, "x86_64") == 0) ||
         (g_ascii_strcasecmp (one, "x86_64") == 0 &&
          g_ascii_strcasecmp (two, "amd64") == 0) ||
         (g_ascii_strcasecmp (one, "aarch64") == 0 &&
          g_ascii_strcasecmp (two, "arm64") == 0) ||
         (g_ascii_strcasecmp (one, "arm64") == 0 &&
          g_ascii_strcasecmp (two, "aarch64") == 0);
}

/* The rule matches only when the regular expression covers the whole
 * value, which is how the launcher that wrote these rules reads it. */
static gboolean
version_matches (const char *pattern, const char *version)
{
  static char *cached_pattern = NULL;
  static GRegex *cached_regex = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GMatchInfo) match = NULL;
  gint start = 0;
  gint end = 0;

  if (pattern == NULL || version == NULL ||
      strlen (pattern) > MAX_VERSION_REGEX_LENGTH ||
      strlen (version) > MAX_VERSION_VALUE_LENGTH)
    return FALSE;

  if (g_strcmp0 (cached_pattern, pattern) != 0)
    {
      g_clear_pointer (&cached_pattern, g_free);
      g_clear_pointer (&cached_regex, g_regex_unref);
      cached_pattern = g_strdup (pattern);
      cached_regex = g_regex_new (pattern, G_REGEX_OPTIMIZE, 0, &error);
    }

  if (cached_regex == NULL)
    {
      g_warning ("the version pattern \"%s\" is not a regular expression: %s",
                 pattern, error->message);
      return FALSE;
    }

  if (!g_regex_match (cached_regex, version, 0, &match))
    return FALSE;

  g_match_info_fetch_pos (match, 0, &start, &end);

  return start == 0 && end == (gint) strlen (version);
}

gboolean
nuvasha_rule_matches (const NuvashaRule *rule, const NuvashaRuleContext *context)
{
  g_return_val_if_fail (rule != NULL, FALSE);
  g_return_val_if_fail (context != NULL, FALSE);

  if (rule->os_name != NULL)
    {
      if (context->os_name == NULL ||
          g_ascii_strcasecmp (rule->os_name, context->os_name) != 0)
        return FALSE;
    }

  if (rule->os_arch != NULL)
    {
      if (context->os_arch == NULL || !arch_equals (rule->os_arch, context->os_arch))
        return FALSE;
    }

  if (rule->os_version != NULL && !version_matches (rule->os_version, context->os_version))
    return FALSE;

  if (rule->features != NULL)
    {
      GHashTableIter iter;
      gpointer name = NULL;
      gpointer value = NULL;

      g_hash_table_iter_init (&iter, rule->features);

      while (g_hash_table_iter_next (&iter, &name, &value))
        {
          /* A feature nobody set is off, so the rule asks for the
           * value it wrote down and the context answers with what is
           * set. */
          gboolean wanted = GPOINTER_TO_INT (value) != 0;
          gboolean actual =
            g_hash_table_contains (context->features, name);

          if (wanted != actual)
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
nuvasha_rules_allow (GPtrArray *rules, const NuvashaRuleContext *context)
{
  g_return_val_if_fail (context != NULL, FALSE);

  /* Nothing wrote a rule down, so nothing takes the version away. */
  if (rules == NULL || rules->len == 0)
    return TRUE;

  /* Rules exist and the format says the answer is "disallow" until a
   * matching rule says otherwise. */
  gboolean allowed = FALSE;

  for (guint i = 0; i < rules->len; i++)
    {
      NuvashaRule *rule = g_ptr_array_index (rules, i);

      if (nuvasha_rule_matches (rule, context))
        allowed = rule->allow;
    }

  return allowed;
}

gboolean
nuvasha_argument_allowed (const NuvashaArgument *argument, const NuvashaRuleContext *context)
{
  g_return_val_if_fail (argument != NULL, FALSE);
  g_return_val_if_fail (context != NULL, FALSE);

  return nuvasha_rules_allow (argument->rules, context);
}

gboolean
nuvasha_library_allowed (const NuvashaLibrary *library, const NuvashaRuleContext *context)
{
  g_return_val_if_fail (library != NULL, FALSE);
  g_return_val_if_fail (context != NULL, FALSE);

  return nuvasha_rules_allow (library->rules, context);
}

NuvashaRuleContext *
nuvasha_rule_context_new_for (const char *os_name,
                          const char *os_arch,
                          const char *os_version)
{
  NuvashaRuleContext *context = g_new0 (NuvashaRuleContext, 1);

  context->os_name = g_strdup (os_name);
  context->os_arch = g_strdup (os_arch);
  context->os_version = g_strdup (os_version);
  context->features = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, NULL);

  return context;
}

NuvashaRuleContext *
nuvasha_rule_context_new (void)
{
  struct utsname machine;
  g_autofree char *name = NULL;

  memset (&machine, 0, sizeof (machine));

  if (uname (&machine) != 0)
    {
      /* nuvasha runs on Linux and nowhere else, so the name Mojang's
       * rules look for is known even when the call fails. */
      return nuvasha_rule_context_new_for ("linux", NULL, NULL);
    }

  /* Rules write the name in lower case, uname does not. */
  name = g_ascii_strdown (machine.sysname, -1);

  return nuvasha_rule_context_new_for (name, machine.machine, machine.release);
}

void
nuvasha_rule_context_free (NuvashaRuleContext *context)
{
  if (context == NULL)
    return;

  g_free (context->os_name);
  g_free (context->os_arch);
  g_free (context->os_version);
  g_clear_pointer (&context->features, g_hash_table_unref);
  g_free (context);
}

void
nuvasha_rule_context_set_feature (NuvashaRuleContext *context,
                              const char     *name,
                              gboolean        value)
{
  g_return_if_fail (context != NULL);
  g_return_if_fail (name != NULL);

  if (value)
    g_hash_table_replace (context->features, g_strdup (name),
                          GINT_TO_POINTER (1));
  else
    g_hash_table_remove (context->features, name);
}

gboolean
nuvasha_rule_context_get_feature (NuvashaRuleContext *context, const char *name)
{
  g_return_val_if_fail (context != NULL, FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  return g_hash_table_contains (context->features, name);
}
