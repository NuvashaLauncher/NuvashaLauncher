/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Mojang version metadata types, rule evaluation and JSON parsing. The
 * asynchronous cache/fetch pipeline lives in aml-minecraft-versions.c. */

#include "aml-minecraft-versions.h"

#include <string.h>
#include <sys/utsname.h>

static void free_download_info (gpointer data);
static void free_rule (gpointer data);
static void free_library (gpointer data);
static void free_argument (gpointer data);
static void free_logging (gpointer data);
static void free_java_version (gpointer data);
static void free_entry (gpointer data);

/* ---------------------------------------------------------- *
 * Types and errors
 * ---------------------------------------------------------- */

GQuark
aml_mc_error_quark (void)
{
  return g_quark_from_static_string ("aml-minecraft-versions-error");
}

/* The order has to match AmlMcVersionType. */
static const char * const version_type_names[] = {
  "release",
  "snapshot",
  "old_beta",
  "old_alpha",
};

gboolean
aml_mc_version_type_from_string (const char *string, AmlMcVersionType *type)
{
  g_return_val_if_fail (type != NULL, FALSE);

  if (string == NULL)
    return FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (version_type_names); i++)
    {
      if (g_str_equal (string, version_type_names[i]))
        {
          *type = (AmlMcVersionType) i;
          return TRUE;
        }
    }

  return FALSE;
}

const char *
aml_mc_version_type_to_string (AmlMcVersionType type)
{
  if ((guint) type >= G_N_ELEMENTS (version_type_names))
    return NULL;

  return version_type_names[type];
}

/* ---------------------------------------------------------- *
 * Reading fields out of a JsonObject
 *
 * Every helper writes NULL into the target before it looks at the
 * document, so a caller that fails half way never sees a stale
 * pointer.
 * ---------------------------------------------------------- */

static gboolean
missing_field (const char *member, GError **error)
{
  g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING,
               "required field \"%s\" is missing", member);
  return FALSE;
}

static gboolean
wrong_field (const char *member, const char *expected, GError **error)
{
  g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
               "field \"%s\" must be %s", member, expected);
  return FALSE;
}

/* *value receives an owned copy, or NULL when the member is absent.
 * @required decides whether "absent" is an error at all. */
static gboolean
get_string (JsonObject  *object,
            const char  *member,
            gboolean     required,
            char       **value,
            GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || missing_field (member, error);

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return wrong_field (member, "a string", error);

  *value = g_strdup (json_node_get_string (node));

  if (required && (*value)[0] == '\0')
    {
      g_clear_pointer (value, g_free);
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING,
                   "required field \"%s\" is empty", member);
      return FALSE;
    }

  return TRUE;
}

/* The object itself is borrowed from the parser, so the caller only
 * uses it while the document is alive. */
static gboolean
get_object (JsonObject   *object,
            const char   *member,
            gboolean      required,
            JsonObject  **value,
            GError      **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || missing_field (member, error);

  if (!JSON_NODE_HOLDS_OBJECT (node))
    return wrong_field (member, "an object", error);

  *value = json_node_get_object (node);
  return TRUE;
}

static gboolean
get_array (JsonObject  *object,
           const char  *member,
           gboolean     required,
           JsonArray  **value,
           GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || missing_field (member, error);

  if (!JSON_NODE_HOLDS_ARRAY (node))
    return wrong_field (member, "an array", error);

  *value = json_node_get_array (node);
  return TRUE;
}

static gboolean
get_int64 (JsonObject *object,
           const char *member,
           gboolean    required,
           gint64     *value,
           GError    **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = 0;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return !required || missing_field (member, error);

  if (JSON_NODE_HOLDS_VALUE (node))
    {
      GType type = json_node_get_value_type (node);

      if (type == G_TYPE_INT64)
        {
          *value = json_node_get_int (node);
          return TRUE;
        }

      if (type == G_TYPE_DOUBLE)
        {
          *value = (gint64) json_node_get_double (node);
          return TRUE;
        }
    }

  return wrong_field (member, "a number", error);
}

/* Parses @json and returns the root object, handing back the parser
 * it came from so that borrowed nodes stay valid. The caller unrefs
 * it. */
static JsonObject *
root_object (const char *json, gssize length, JsonParser **parser, GError **error)
{
  JsonParser *document = json_parser_new ();
  g_autoptr (GError) json_error = NULL;
  JsonNode *root;

  if (!json_parser_load_from_data (document, json, length, &json_error))
    {
      /* The domain changes so that a caller only has to know
       * AML_MC_ERROR for a document that will not read. */
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "the document is not JSON: %s", json_error->message);
      g_object_unref (document);
      return NULL;
    }

  root = json_parser_get_root (document);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "the document is not a JSON object");
      g_object_unref (document);
      return NULL;
    }

  *parser = document;
  return json_node_get_object (root);
}

/* ---------------------------------------------------------- *
 * Freeing
 * ---------------------------------------------------------- */

static void
free_download_info (gpointer data)
{
  AmlDownloadInfo *info = data;

  if (info == NULL)
    return;

  g_free (info->id);
  g_free (info->url);
  g_free (info->sha1);
  g_free (info->path);
  g_free (info);
}

static void
free_rule (gpointer data)
{
  AmlRule *rule = data;

  if (rule == NULL)
    return;

  g_free (rule->os_name);
  g_free (rule->os_arch);
  g_free (rule->os_version);
  g_clear_pointer (&rule->features, g_hash_table_unref);
  g_free (rule);
}

static void
free_library (gpointer data)
{
  AmlLibrary *library = data;

  if (library == NULL)
    return;

  g_free (library->name);
  g_clear_pointer (&library->artifact, free_download_info);
  g_clear_pointer (&library->classifiers, g_hash_table_unref);
  g_clear_pointer (&library->natives, g_hash_table_unref);
  g_clear_pointer (&library->rules, g_ptr_array_unref);
  g_clear_pointer (&library->exclude, g_ptr_array_unref);
  g_free (library);
}

static void
free_argument (gpointer data)
{
  AmlArgument *argument = data;

  if (argument == NULL)
    return;

  g_clear_pointer (&argument->rules, g_ptr_array_unref);
  g_clear_pointer (&argument->values, g_ptr_array_unref);
  g_free (argument);
}

static void
free_logging (gpointer data)
{
  AmlLogging *logging = data;

  if (logging == NULL)
    return;

  g_free (logging->argument);
  g_free (logging->type);
  g_clear_pointer (&logging->file, free_download_info);
  g_free (logging);
}

static void
free_java_version (gpointer data)
{
  AmlJavaVersion *version = data;

  if (version == NULL)
    return;

  g_free (version->component);
  g_free (version);
}

static void
free_entry (gpointer data)
{
  AmlVersionEntry *entry = data;

  if (entry == NULL)
    return;

  g_free (entry->id);
  g_free (entry->url);
  g_free (entry->sha1);
  g_free (entry->release_time);
  g_free (entry);
}

/* ---------------------------------------------------------- *
 * Rules
 * ---------------------------------------------------------- */

static AmlRule *
parse_rule (JsonObject *object, GError **error)
{
  AmlRule *rule = g_new0 (AmlRule, 1);
  JsonObject *os = NULL;
  JsonObject *features = NULL;
  g_autofree char *action = NULL;

  if (!get_string (object, "action", TRUE, &action, error))
    goto fail;

  if (g_str_equal (action, "allow"))
    rule->allow = TRUE;
  else if (g_str_equal (action, "disallow"))
    rule->allow = FALSE;
  else
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "unknown rule action \"%s\"", action);
      goto fail;
    }

  if (!get_object (object, "os", FALSE, &os, error))
    goto fail;

  if (os != NULL)
    {
      /* "versionRange" is a window of Windows versions: it cannot
       * apply to AML, which only ever runs on Linux, and is left out
       * on purpose. "version" is a regular expression and is
       * evaluated below. */
      if (!get_string (os, "name", FALSE, &rule->os_name, error) ||
          !get_string (os, "arch", FALSE, &rule->os_arch, error) ||
          !get_string (os, "version", FALSE, &rule->os_version, error))
        goto fail;
    }

  if (!get_object (object, "features", FALSE, &features, error))
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
              g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
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
  free_rule (rule);
  return NULL;
}

/* NULL only on error, with @error set. */
static GPtrArray *
parse_rules (JsonNode *node, GError **error)
{
  GPtrArray *rules;
  JsonArray *array;

  if (!JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "\"rules\" must be an array");
      return NULL;
    }

  array = json_node_get_array (node);
  rules = g_ptr_array_new_with_free_func (free_rule);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *item = json_array_get_element (array, i);
      AmlRule *rule;

      if (!JSON_NODE_HOLDS_OBJECT (item))
        {
          g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                       "rule %u is not an object", i);
          g_ptr_array_unref (rules);
          return NULL;
        }

      rule = parse_rule (json_node_get_object (item), error);

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
static gboolean
get_rules_member (JsonObject  *object,
                  const char  *member,
                  GPtrArray  **rules,
                  GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *rules = NULL;

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  *rules = parse_rules (node, error);
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
  g_autoptr (GError) error = NULL;
  g_autoptr (GRegex) regex = NULL;
  g_autoptr (GMatchInfo) match = NULL;
  gint start = 0;
  gint end = 0;

  if (version == NULL)
    return FALSE;

  regex = g_regex_new (pattern, G_REGEX_OPTIMIZE, 0, &error);

  if (regex == NULL)
    {
      g_warning ("the version pattern \"%s\" is not a regular expression: %s",
                 pattern, error->message);
      return FALSE;
    }

  if (!g_regex_match (regex, version, 0, &match))
    return FALSE;

  g_match_info_fetch_pos (match, 0, &start, &end);

  return start == 0 && end == (gint) strlen (version);
}

gboolean
aml_rule_matches (const AmlRule *rule, const AmlRuleContext *context)
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
aml_rules_allow (GPtrArray *rules, const AmlRuleContext *context)
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
      AmlRule *rule = g_ptr_array_index (rules, i);

      if (aml_rule_matches (rule, context))
        allowed = rule->allow;
    }

  return allowed;
}

gboolean
aml_argument_allowed (const AmlArgument *argument, const AmlRuleContext *context)
{
  g_return_val_if_fail (argument != NULL, FALSE);
  g_return_val_if_fail (context != NULL, FALSE);

  return aml_rules_allow (argument->rules, context);
}

gboolean
aml_library_allowed (const AmlLibrary *library, const AmlRuleContext *context)
{
  g_return_val_if_fail (library != NULL, FALSE);
  g_return_val_if_fail (context != NULL, FALSE);

  return aml_rules_allow (library->rules, context);
}

AmlRuleContext *
aml_rule_context_new_for (const char *os_name,
                          const char *os_arch,
                          const char *os_version)
{
  AmlRuleContext *context = g_new0 (AmlRuleContext, 1);

  context->os_name = g_strdup (os_name);
  context->os_arch = g_strdup (os_arch);
  context->os_version = g_strdup (os_version);
  context->features = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, NULL);

  return context;
}

AmlRuleContext *
aml_rule_context_new (void)
{
  struct utsname machine;
  g_autofree char *name = NULL;

  memset (&machine, 0, sizeof (machine));

  if (uname (&machine) != 0)
    {
      /* AML runs on Linux and nowhere else, so the name Mojang's
       * rules look for is known even when the call fails. */
      return aml_rule_context_new_for ("linux", NULL, NULL);
    }

  /* Rules write the name in lower case, uname does not. */
  name = g_ascii_strdown (machine.sysname, -1);

  return aml_rule_context_new_for (name, machine.machine, machine.release);
}

void
aml_rule_context_free (AmlRuleContext *context)
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
aml_rule_context_set_feature (AmlRuleContext *context,
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
aml_rule_context_get_feature (AmlRuleContext *context, const char *name)
{
  g_return_val_if_fail (context != NULL, FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  return g_hash_table_contains (context->features, name);
}

/* ---------------------------------------------------------- *
 * Libraries, arguments and the rest of a version document
 * ---------------------------------------------------------- */

/* @url_required is FALSE for the places where Mojang gives a path but
 * no address, which the launcher fills in later. */
static AmlDownloadInfo *
parse_download_info (JsonObject *object, gboolean url_required, GError **error)
{
  AmlDownloadInfo *info = g_new0 (AmlDownloadInfo, 1);

  if (!get_string (object, "url", url_required, &info->url, error) ||
      !get_string (object, "sha1", FALSE, &info->sha1, error) ||
      !get_string (object, "id", FALSE, &info->id, error) ||
      !get_string (object, "path", FALSE, &info->path, error) ||
      !get_int64 (object, "size", FALSE, &info->size, error) ||
      !get_int64 (object, "totalSize", FALSE, &info->total_size, error))
    {
      free_download_info (info);
      return NULL;
    }

  return info;
}

/* *value is NULL when the member is absent. */
static gboolean
get_download_member (JsonObject     *object,
                     const char     *member,
                     gboolean        url_required,
                     AmlDownloadInfo **value,
                     GError        **error)
{
  JsonObject *child = NULL;

  *value = NULL;

  if (!get_object (object, member, FALSE, &child, error))
    return FALSE;

  if (child == NULL)
    return TRUE;

  *value = parse_download_info (child, url_required, error);
  return *value != NULL;
}

static AmlLibrary *
parse_library (JsonObject *object, GError **error)
{
  AmlLibrary *library = g_new0 (AmlLibrary, 1);
  JsonObject *downloads = NULL;
  JsonObject *classifiers = NULL;
  JsonObject *natives = NULL;
  JsonObject *extract = NULL;

  if (!get_string (object, "name", TRUE, &library->name, error))
    goto fail;

  if (!get_object (object, "downloads", FALSE, &downloads, error))
    goto fail;

  if (downloads != NULL)
    {
      if (!get_download_member (downloads, "artifact", FALSE,
                                &library->artifact, error))
        goto fail;

      if (!get_object (downloads, "classifiers", FALSE, &classifiers, error))
        goto fail;

      if (classifiers != NULL)
        {
          JsonObjectIter iter;
          const char *name = NULL;
          JsonNode *node = NULL;

          library->classifiers =
            g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   free_download_info);
          json_object_iter_init (&iter, classifiers);

          while (json_object_iter_next (&iter, &name, &node))
            {
              AmlDownloadInfo *info;

              if (!JSON_NODE_HOLDS_OBJECT (node))
                {
                  g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                               "classifier \"%s\" must be an object", name);
                  goto fail;
                }

              info = parse_download_info (json_node_get_object (node), FALSE,
                                          error);

              if (info == NULL)
                goto fail;

              g_hash_table_replace (library->classifiers, g_strdup (name),
                                    info);
            }
        }
    }

  if (!get_object (object, "natives", FALSE, &natives, error))
    goto fail;

  if (natives != NULL)
    {
      JsonObjectIter iter;
      const char *name = NULL;
      JsonNode *node = NULL;

      library->natives = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
      json_object_iter_init (&iter, natives);

      while (json_object_iter_next (&iter, &name, &node))
        {
          if (!JSON_NODE_HOLDS_VALUE (node) ||
              json_node_get_value_type (node) != G_TYPE_STRING)
            {
              g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                           "native \"%s\" must be a string", name);
              goto fail;
            }

          g_hash_table_replace (library->natives, g_strdup (name),
                                g_strdup (json_node_get_string (node)));
        }
    }

  if (!get_rules_member (object, "rules", &library->rules, error))
    goto fail;

  if (!get_object (object, "extract", FALSE, &extract, error))
    goto fail;

  if (extract != NULL)
    {
      JsonArray *array = NULL;

      if (!get_array (extract, "exclude", FALSE, &array, error))
        goto fail;

      if (array != NULL)
        {
          library->exclude = g_ptr_array_new_with_free_func (g_free);

          for (guint i = 0; i < json_array_get_length (array); i++)
            {
              JsonNode *item = json_array_get_element (array, i);

              if (!JSON_NODE_HOLDS_VALUE (item) ||
                  json_node_get_value_type (item) != G_TYPE_STRING)
                {
                  g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                               "exclude %u must be a string", i);
                  goto fail;
                }

              g_ptr_array_add (library->exclude,
                               g_strdup (json_node_get_string (item)));
            }
        }
    }

  return library;

fail:
  free_library (library);
  return NULL;
}

/* One entry of arguments.jvm or arguments.game: either a plain string
 * or an object carrying rules and one or several values. */
static AmlArgument *
parse_argument (JsonNode *node, GError **error)
{
  AmlArgument *argument = g_new0 (AmlArgument, 1);

  argument->values = g_ptr_array_new_with_free_func (g_free);

  if (JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    {
      g_ptr_array_add (argument->values,
                       g_strdup (json_node_get_string (node)));
      return argument;
    }

  if (!JSON_NODE_HOLDS_OBJECT (node))
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "an argument must be a string or an object");
      goto fail;
    }

  {
    JsonObject *object = json_node_get_object (node);
    JsonNode *value = json_object_get_member (object, "value");

    if (!get_rules_member (object, "rules", &argument->rules, error))
      goto fail;

    if (value == NULL || JSON_NODE_HOLDS_NULL (value))
      {
        g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_MISSING,
                     "required field \"value\" is missing");
        goto fail;
      }

    if (JSON_NODE_HOLDS_VALUE (value) &&
        json_node_get_value_type (value) == G_TYPE_STRING)
      {
        g_ptr_array_add (argument->values,
                         g_strdup (json_node_get_string (value)));
        return argument;
      }

    if (JSON_NODE_HOLDS_ARRAY (value))
      {
        JsonArray *array = json_node_get_array (value);

        for (guint i = 0; i < json_array_get_length (array); i++)
          {
            JsonNode *item = json_array_get_element (array, i);

            if (!JSON_NODE_HOLDS_VALUE (item) ||
                json_node_get_value_type (item) != G_TYPE_STRING)
              {
                g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                             "argument value %u must be a string", i);
                goto fail;
              }

            g_ptr_array_add (argument->values,
                             g_strdup (json_node_get_string (item)));
          }

        return argument;
      }

    g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                 "field \"value\" must be a string or an array");
  }

fail:
  free_argument (argument);
  return NULL;
}

/* arguments.jvm and arguments.game. Mojang writes other groups beside
 * them, such as the default heap sizes, and they are not part of what
 * launches the game, so they are skipped. */
static gboolean
parse_argument_group (JsonObject   *object,
                      const char   *member,
                      GPtrArray   **value,
                      GError      **error)
{
  JsonNode *node = json_object_get_member (object, member);

  *value = g_ptr_array_new_with_free_func (free_argument);

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  if (!JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "field \"%s\" must be an array", member);
      return FALSE;
    }

  {
    JsonArray *array = json_node_get_array (node);

    for (guint i = 0; i < json_array_get_length (array); i++)
      {
        AmlArgument *argument =
          parse_argument (json_array_get_element (array, i), error);

        if (argument == NULL)
          return FALSE;

        g_ptr_array_add (*value, argument);
      }
  }

  return TRUE;
}

static gboolean
parse_arguments (JsonObject *root,
                 AmlVersionDescription *self,
                 GError **error)
{
  JsonObject *arguments = NULL;

  if (!get_object (root, "arguments", FALSE, &arguments, error))
    return FALSE;

  if (arguments == NULL)
    {
      /* A legacy document: only minecraftArguments, and no modern
       * groups at all. */
      self->jvm_arguments = g_ptr_array_new_with_free_func (free_argument);
      self->game_arguments = g_ptr_array_new_with_free_func (free_argument);
      return TRUE;
    }

  self->modern_arguments = TRUE;

  if (!parse_argument_group (arguments, "jvm", &self->jvm_arguments, error))
    return FALSE;

  return parse_argument_group (arguments, "game", &self->game_arguments, error);
}

static gboolean
parse_libraries (JsonObject *root, GPtrArray **value, GError **error)
{
  JsonNode *node = json_object_get_member (root, "libraries");

  *value = g_ptr_array_new_with_free_func (free_library);

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  if (!JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "field \"libraries\" must be an array");
      return FALSE;
    }

  {
    JsonArray *array = json_node_get_array (node);

    for (guint i = 0; i < json_array_get_length (array); i++)
      {
        JsonNode *item = json_array_get_element (array, i);
        AmlLibrary *library;

        if (!JSON_NODE_HOLDS_OBJECT (item))
          {
            g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                         "library %u is not an object", i);
            return FALSE;
          }

        library = parse_library (json_node_get_object (item), error);

        if (library == NULL)
          return FALSE;

        g_ptr_array_add (*value, library);
      }
  }

  return TRUE;
}

/* *logging is NULL when the document has nothing to say about the
 * log4j configuration. */
static gboolean
parse_logging (JsonObject *root, AmlLogging **logging, GError **error)
{
  JsonObject *entry = NULL;
  JsonObject *client = NULL;

  *logging = NULL;

  if (!get_object (root, "logging", FALSE, &entry, error))
    return FALSE;

  if (entry == NULL)
    return TRUE;

  if (!get_object (entry, "client", FALSE, &client, error))
    return FALSE;

  if (client == NULL)
    return TRUE;

  {
    AmlLogging *logging_client = g_new0 (AmlLogging, 1);

    if (!get_string (client, "argument", TRUE, &logging_client->argument, error) ||
        !get_string (client, "type", TRUE, &logging_client->type, error) ||
        !get_download_member (client, "file", TRUE, &logging_client->file, error))
      {
        free_logging (logging_client);
        return FALSE;
      }

    *logging = logging_client;
  }

  return TRUE;
}

static gboolean
parse_java_version (JsonObject *root, AmlJavaVersion **version, GError **error)
{
  JsonObject *object = NULL;
  AmlJavaVersion *java_version;
  gint64 major = 0;

  *version = NULL;

  if (!get_object (root, "javaVersion", FALSE, &object, error))
    return FALSE;

  if (object == NULL)
    return TRUE;

  java_version = g_new0 (AmlJavaVersion, 1);

  if (!get_string (object, "component", TRUE, &java_version->component, error) ||
      !get_int64 (object, "majorVersion", TRUE, &major, error))
    {
      free_java_version (java_version);
      return FALSE;
    }

  java_version->major_version = (int) major;
  *version = java_version;

  return TRUE;
}

/* ---------------------------------------------------------- *
 * AmlVersionDescription
 * ---------------------------------------------------------- */

G_DEFINE_FINAL_TYPE (AmlVersionDescription, aml_version_description,
                     G_TYPE_OBJECT)

static gboolean
fill_version (JsonObject *root, AmlVersionDescription *self, GError **error)
{
  JsonObject *downloads = NULL;
  g_autofree char *type_name = NULL;

  if (!get_string (root, "id", TRUE, &self->id, error))
    return FALSE;

  if (!get_string (root, "type", TRUE, &type_name, error))
    return FALSE;

  if (!aml_mc_version_type_from_string (type_name, &self->type))
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "unknown version type \"%s\"", type_name);
      return FALSE;
    }

  /* Everything below is optional: a loader profile that inherits from
   * a real version legitimately carries only some of it. */
  if (!get_string (root, "mainClass", FALSE, &self->main_class, error) ||
      !get_string (root, "inheritsFrom", FALSE, &self->inherits_from, error) ||
      !get_string (root, "assets", FALSE, &self->assets, error) ||
      !get_string (root, "minecraftArguments", FALSE,
                   &self->minecraft_arguments, error))
    return FALSE;

  if (!get_download_member (root, "assetIndex", TRUE, &self->asset_index, error))
    return FALSE;

  if (!get_object (root, "downloads", FALSE, &downloads, error))
    return FALSE;

  if (downloads != NULL &&
      !get_download_member (downloads, "client", TRUE, &self->client, error))
    return FALSE;

  if (!parse_libraries (root, &self->libraries, error))
    return FALSE;

  if (!parse_arguments (root, self, error))
    return FALSE;

  if (!parse_logging (root, &self->logging, error))
    return FALSE;

  return parse_java_version (root, &self->java_version, error);
}

AmlVersionDescription *
aml_version_description_new_from_json (const char *json,
                                       gssize      length,
                                       GError    **error)
{
  g_autoptr (JsonParser) parser = NULL;
  AmlVersionDescription *self;
  JsonObject *root;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (json != NULL, NULL);

  root = root_object (json, length, &parser, error);

  if (root == NULL)
    return NULL;

  self = g_object_new (AML_TYPE_VERSION_DESCRIPTION, NULL);

  if (!fill_version (root, self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

static void
aml_version_description_finalize (GObject *object)
{
  AmlVersionDescription *self = AML_VERSION_DESCRIPTION (object);

  g_free (self->id);
  g_free (self->main_class);
  g_free (self->inherits_from);
  g_free (self->assets);
  g_free (self->minecraft_arguments);
  g_clear_pointer (&self->asset_index, free_download_info);
  g_clear_pointer (&self->client, free_download_info);
  g_clear_pointer (&self->logging, free_logging);
  g_clear_pointer (&self->java_version, free_java_version);
  g_clear_pointer (&self->libraries, g_ptr_array_unref);
  g_clear_pointer (&self->jvm_arguments, g_ptr_array_unref);
  g_clear_pointer (&self->game_arguments, g_ptr_array_unref);

  G_OBJECT_CLASS (aml_version_description_parent_class)->finalize (object);
}

static void
aml_version_description_class_init (AmlVersionDescriptionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = aml_version_description_finalize;
}

static void
aml_version_description_init (AmlVersionDescription *self)
{
  (void) self;
}

/* ---------------------------------------------------------- *
 * AmlVersionManifest
 * ---------------------------------------------------------- */

G_DEFINE_FINAL_TYPE (AmlVersionManifest, aml_version_manifest, G_TYPE_OBJECT)

static AmlVersionEntry *
parse_entry (JsonObject *object, GError **error)
{
  AmlVersionEntry *entry = g_new0 (AmlVersionEntry, 1);
  g_autofree char *type_name = NULL;

  if (!get_string (object, "id", TRUE, &entry->id, error) ||
      !get_string (object, "type", TRUE, &type_name, error) ||
      !get_string (object, "url", TRUE, &entry->url, error) ||
      !get_string (object, "sha1", TRUE, &entry->sha1, error) ||
      !get_string (object, "releaseTime", TRUE, &entry->release_time, error))
    {
      free_entry (entry);
      return NULL;
    }

  if (!aml_mc_version_type_from_string (type_name, &entry->type))
    {
      g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                   "unknown version type \"%s\"", type_name);
      free_entry (entry);
      return NULL;
    }

  return entry;
}

static gboolean
fill_manifest (JsonObject *root, AmlVersionManifest *self, GError **error)
{
  JsonObject *latest = NULL;
  JsonArray *versions = NULL;

  if (!get_object (root, "latest", TRUE, &latest, error))
    return FALSE;

  if (latest == NULL)
    return missing_field ("latest", error);

  if (!get_string (latest, "release", TRUE, &self->latest_release, error) ||
      !get_string (latest, "snapshot", TRUE, &self->latest_snapshot, error))
    return FALSE;

  if (!get_array (root, "versions", TRUE, &versions, error))
    return FALSE;

  if (versions == NULL)
    return missing_field ("versions", error);

  self->entries = g_ptr_array_new_with_free_func (free_entry);

  for (guint i = 0; i < json_array_get_length (versions); i++)
    {
      JsonNode *item = json_array_get_element (versions, i);
      AmlVersionEntry *entry;

      if (!JSON_NODE_HOLDS_OBJECT (item))
        {
          g_set_error (error, AML_MC_ERROR, AML_MC_ERROR_INVALID,
                       "version %u is not an object", i);
          return FALSE;
        }

      entry = parse_entry (json_node_get_object (item), error);

      if (entry == NULL)
        return FALSE;

      g_ptr_array_add (self->entries, entry);
    }

  return TRUE;
}

AmlVersionManifest *
aml_version_manifest_new_from_json (const char *json,
                                    gssize      length,
                                    GError    **error)
{
  g_autoptr (JsonParser) parser = NULL;
  AmlVersionManifest *self;
  JsonObject *root;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (json != NULL, NULL);

  root = root_object (json, length, &parser, error);

  if (root == NULL)
    return NULL;

  self = g_object_new (AML_TYPE_VERSION_MANIFEST, NULL);

  if (!fill_manifest (root, self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

const AmlVersionEntry *
aml_version_manifest_find (AmlVersionManifest *self, const char *id)
{
  g_return_val_if_fail (AML_IS_VERSION_MANIFEST (self), NULL);
  g_return_val_if_fail (id != NULL, NULL);

  if (self->entries == NULL)
    return NULL;

  for (guint i = 0; i < self->entries->len; i++)
    {
      AmlVersionEntry *entry = g_ptr_array_index (self->entries, i);

      if (g_str_equal (entry->id, id))
        return entry;
    }

  return NULL;
}

static void
aml_version_manifest_finalize (GObject *object)
{
  AmlVersionManifest *self = AML_VERSION_MANIFEST (object);

  g_free (self->latest_release);
  g_free (self->latest_snapshot);
  g_clear_pointer (&self->entries, g_ptr_array_unref);

  G_OBJECT_CLASS (aml_version_manifest_parent_class)->finalize (object);
}

static void
aml_version_manifest_class_init (AmlVersionManifestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = aml_version_manifest_finalize;
}

static void
aml_version_manifest_init (AmlVersionManifest *self)
{
  (void) self;
}

/* ---------------------------------------------------------- *
 * Merging a version with the one it inherits from
 * ---------------------------------------------------------- */

static char *
either (const char *child_value, const char *parent_value)
{
  return g_strdup (child_value != NULL ? child_value : parent_value);
}

static AmlDownloadInfo *
download_copy (const AmlDownloadInfo *source)
{
  AmlDownloadInfo *copy;

  if (source == NULL)
    return NULL;

  copy = g_new0 (AmlDownloadInfo, 1);
  copy->id = g_strdup (source->id);
  copy->url = g_strdup (source->url);
  copy->sha1 = g_strdup (source->sha1);
  copy->path = g_strdup (source->path);
  copy->size = source->size;
  copy->total_size = source->total_size;

  return copy;
}

static AmlRule *
rule_copy (const AmlRule *source)
{
  AmlRule *copy = g_new0 (AmlRule, 1);

  copy->allow = source->allow;
  copy->os_name = g_strdup (source->os_name);
  copy->os_arch = g_strdup (source->os_arch);
  copy->os_version = g_strdup (source->os_version);

  if (source->features != NULL)
    {
      GHashTableIter iter;
      gpointer name = NULL;
      gpointer required = NULL;

      copy->features = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);
      g_hash_table_iter_init (&iter, source->features);

      while (g_hash_table_iter_next (&iter, &name, &required))
        g_hash_table_replace (copy->features, g_strdup (name), required);
    }

  return copy;
}

static GPtrArray *
rules_copy (GPtrArray *source)
{
  GPtrArray *copy;

  if (source == NULL)
    return NULL;

  copy = g_ptr_array_new_with_free_func (free_rule);

  for (guint i = 0; i < source->len; i++)
    g_ptr_array_add (copy, rule_copy (g_ptr_array_index (source, i)));

  return copy;
}

static GPtrArray *
strings_copy (GPtrArray *source)
{
  GPtrArray *copy;

  if (source == NULL)
    return NULL;

  copy = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < source->len; i++)
    g_ptr_array_add (copy, g_strdup (g_ptr_array_index (source, i)));

  return copy;
}

static GHashTable *
classifiers_copy (GHashTable *source)
{
  GHashTable *copy;
  GHashTableIter iter;
  gpointer name = NULL;
  gpointer info = NULL;

  if (source == NULL)
    return NULL;

  copy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                free_download_info);
  g_hash_table_iter_init (&iter, source);

  while (g_hash_table_iter_next (&iter, &name, &info))
    g_hash_table_replace (copy, g_strdup (name), download_copy (info));

  return copy;
}

static GHashTable *
natives_copy (GHashTable *source)
{
  GHashTable *copy;
  GHashTableIter iter;
  gpointer name = NULL;
  gpointer classifier = NULL;

  if (source == NULL)
    return NULL;

  copy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_iter_init (&iter, source);

  while (g_hash_table_iter_next (&iter, &name, &classifier))
    g_hash_table_replace (copy, g_strdup (name), g_strdup (classifier));

  return copy;
}

static AmlLibrary *
library_copy (const AmlLibrary *source)
{
  AmlLibrary *copy = g_new0 (AmlLibrary, 1);

  copy->name = g_strdup (source->name);
  copy->artifact = download_copy (source->artifact);
  copy->classifiers = classifiers_copy (source->classifiers);
  copy->natives = natives_copy (source->natives);
  copy->rules = rules_copy (source->rules);
  copy->exclude = strings_copy (source->exclude);

  return copy;
}

static AmlArgument *
argument_copy (const AmlArgument *source)
{
  AmlArgument *copy = g_new0 (AmlArgument, 1);

  copy->rules = rules_copy (source->rules);
  copy->values = strings_copy (source->values);

  return copy;
}

static AmlLogging *
logging_copy (const AmlLogging *source)
{
  AmlLogging *copy;

  if (source == NULL)
    return NULL;

  copy = g_new0 (AmlLogging, 1);
  copy->argument = g_strdup (source->argument);
  copy->type = g_strdup (source->type);
  copy->file = download_copy (source->file);

  return copy;
}

static AmlJavaVersion *
java_version_copy (const AmlJavaVersion *source)
{
  AmlJavaVersion *copy;

  if (source == NULL)
    return NULL;

  copy = g_new0 (AmlJavaVersion, 1);
  copy->component = g_strdup (source->component);
  copy->major_version = source->major_version;

  return copy;
}

static GPtrArray *
concat_libraries (GPtrArray *parent, GPtrArray *child)
{
  GPtrArray *all = g_ptr_array_new_with_free_func (free_library);

  for (guint i = 0; parent != NULL && i < parent->len; i++)
    g_ptr_array_add (all, library_copy (g_ptr_array_index (parent, i)));

  for (guint i = 0; child != NULL && i < child->len; i++)
    g_ptr_array_add (all, library_copy (g_ptr_array_index (child, i)));

  return all;
}

static GPtrArray *
concat_arguments (GPtrArray *parent, GPtrArray *child)
{
  GPtrArray *all = g_ptr_array_new_with_free_func (free_argument);

  for (guint i = 0; parent != NULL && i < parent->len; i++)
    g_ptr_array_add (all, argument_copy (g_ptr_array_index (parent, i)));

  for (guint i = 0; child != NULL && i < child->len; i++)
    g_ptr_array_add (all, argument_copy (g_ptr_array_index (child, i)));

  return all;
}

AmlVersionDescription *
aml_version_description_merge (AmlVersionDescription *parent,
                               AmlVersionDescription *child)
{
  AmlVersionDescription *self;

  g_return_val_if_fail (AML_IS_VERSION_DESCRIPTION (parent), NULL);
  g_return_val_if_fail (AML_IS_VERSION_DESCRIPTION (child), NULL);

  self = g_object_new (AML_TYPE_VERSION_DESCRIPTION, NULL);

  /* The identity of the merged version is the child's: that is the
   * name the user picked. */
  self->id = g_strdup (child->id);
  self->type = child->type;
  self->main_class = either (child->main_class, parent->main_class);
  self->assets = either (child->assets, parent->assets);
  self->minecraft_arguments = either (child->minecraft_arguments,
                                      parent->minecraft_arguments);
  self->modern_arguments = child->modern_arguments || parent->modern_arguments;

  /* The merged version keeps climbing the parent's chain, so a caller
   * can resolve a whole line of profiles one step at a time. */
  self->inherits_from = g_strdup (parent->inherits_from);

  self->asset_index = download_copy (child->asset_index != NULL
                                     ? child->asset_index
                                     : parent->asset_index);
  self->client = download_copy (child->client != NULL
                                ? child->client
                                : parent->client);
  self->logging = logging_copy (child->logging != NULL
                                ? child->logging
                                : parent->logging);
  self->java_version = java_version_copy (child->java_version != NULL
                                          ? child->java_version
                                          : parent->java_version);

  /* Libraries and arguments are appended, parent first: whatever the
   * child adds comes after what it inherits. */
  self->libraries = concat_libraries (parent->libraries, child->libraries);
  self->jvm_arguments = concat_arguments (parent->jvm_arguments,
                                          child->jvm_arguments);
  self->game_arguments = concat_arguments (parent->game_arguments,
                                           child->game_arguments);

  return self;
}

GPtrArray *
aml_version_description_legacy_argv (AmlVersionDescription *self)
{
  GPtrArray *words;
  gchar **argv = NULL;
  gint argc = 0;

  g_return_val_if_fail (AML_IS_VERSION_DESCRIPTION (self), NULL);

  words = g_ptr_array_new_with_free_func (g_free);

  if (self->minecraft_arguments == NULL)
    return words;

  if (g_shell_parse_argv (self->minecraft_arguments, &argc, &argv, NULL))
    {
      for (gint i = 0; i < argc; i++)
        g_ptr_array_add (words, g_strdup (argv[i]));

      g_strfreev (argv);
      return words;
    }

  /* Not a command line after all. The version that wrote it simply
   * split the words on spaces, and so does this. */
  {
    gchar **parts = g_strsplit (self->minecraft_arguments, " ", -1);

    for (guint i = 0; parts[i] != NULL; i++)
      {
        if (parts[i][0] != '\0')
          g_ptr_array_add (words, g_strdup (parts[i]));
      }

    g_strfreev (parts);
  }

  return words;
}

