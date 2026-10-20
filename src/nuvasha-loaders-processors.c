/* Loader processor variables, classpaths, and execution. */
#include "nuvasha-loaders-private.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-fs.h"
#include "nuvasha-minecraft-resolve.h"
#include "nuvasha-task-private.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

/* Return the final lines of processor output for error reporting. */
static char *
what_a_processor_said (const char *output, guint count)
{
  g_auto (GStrv) lines = NULL;
  g_autoptr (GString) kept = NULL;
  guint saying = 0;

  if (output == NULL || *output == '\0')
    return NULL;

  lines = g_strsplit (output, "\n", -1);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      if (*g_strstrip (lines[i]) != '\0')
        saying++;
    }

  if (saying == 0)
    return NULL;

  /* The last @count lines that say something: an error of a tool is in
     the last of them, not in the first. */
  kept = g_string_new (NULL);
  saying -= MIN (saying, count);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      const char *line = g_strstrip (lines[i]);

      if (*line == '\0')
        continue;

      if (saying > 0)
        {
          saying--;
          continue;
        }

      if (kept->len > 0)
        g_string_append_c (kept, '\n');

      g_string_append (kept, line);
    }

  return g_strdup (kept->str);
}

static void
processor_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  InstallRequest *request = user_data;
  GSubprocess *process = G_SUBPROCESS (source);
  g_autoptr (GError) error = NULL;
  g_autofree char *output = NULL;
  g_autofree char *said = NULL;

  if (!g_subprocess_communicate_utf8_finish (process, result, &output, NULL,
                                             &error))
    {
      install_failed (request, g_steal_pointer (&error));
      return;
    }

  if (g_subprocess_get_successful (process))
    {
      report_progress (request);
      install_step_done (request);
      return;
    }

  /* Processor output provides details not present in the exit status. */
  said = what_a_processor_said (output, 12);

  g_set_error (&error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_INSTALLER,
               "the processor that patches the game for %s %s failed with "
               "status %d%s%s", nuvasha_loader_display_name (request->loader),
               request->loader_version, g_subprocess_get_exit_status (process),
               said != NULL ? ": " : "", said != NULL ? said : "");

  install_failed (request, g_steal_pointer (&error));
}
/* ---------------------------------------------------------- *
 * What the names in the arguments of a processor stand for
 * ---------------------------------------------------------- */

/* The profile of the install, as the parser that read it keeps it. */
JsonObject *
install_profile (InstallRequest *request)
{
  JsonNode *root = NULL;

  if (request->profile == NULL)
    return NULL;

  root = json_parser_get_root (request->profile);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;

  return json_node_get_object (root);
}

/* What the profile says about one name on the client side: either the
 * value is the same on every side, or each side names its own. */
static const char *
profile_data (InstallRequest *request, const char *name)
{
  JsonNode *data = NULL;
  JsonNode *entry = NULL;
  JsonObject *profile = install_profile (request);

  if (profile == NULL)
    return NULL;

  data = json_object_get_member (profile, "data");

  if (data == NULL || !JSON_NODE_HOLDS_OBJECT (data))
    return NULL;

  entry = json_object_get_member (json_node_get_object (data), name);

  if (entry == NULL)
    return NULL;

  if (JSON_NODE_HOLDS_VALUE (entry) &&
      json_node_get_value_type (entry) == G_TYPE_STRING)
    return json_node_get_string (entry);

  if (!JSON_NODE_HOLDS_OBJECT (entry))
    return NULL;

  entry = json_object_get_member (json_node_get_object (entry), "client");

  if (entry != NULL && JSON_NODE_HOLDS_VALUE (entry) &&
      json_node_get_value_type (entry) == G_TYPE_STRING)
    return json_node_get_string (entry);

  return NULL;
}

/* The names the installer gives to the files of an install itself: where
 * it is installed, the jar it was run from, the client jar its processors
 * patch, the side they run for and the version they are for. */
static const char *
installer_data (InstallRequest *request, const char *name)
{
  /* The root of an install is where its artifacts are, which is the
     directory the games keep their libraries in: that is where the
     documents of the loaders say to find them. */
  if (g_str_equal (name, "ROOT"))
    return request->data_dir;

  if (g_str_equal (name, "INSTALLER"))
    return request->installer;

  if (g_str_equal (name, "MINECRAFT_JAR"))
    return request->client_jar;

  if (g_str_equal (name, "SIDE"))
    return "client";

  if (g_str_equal (name, "VERSION_NAME"))
    return request->game_version;

  if (g_str_equal (name, "CACHE_DIR"))
    return request->root;

  return NULL;
}

static gboolean
resolve_variable (InstallRequest *request, const char *name, GError **error)
{
  const char *value = NULL;
  gsize length = 0;

  if (g_hash_table_contains (request->variables, name))
    return TRUE;

  value = profile_data (request, name);

  if (value == NULL)
    {
      /* The names the installer itself gives are the paths it works on,
         not values of the profile: they are taken as they are. */
      value = installer_data (request, name);

      if (value == NULL)
        {
          g_set_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_INSTALLER,
                       "the install profile of %s %s says nothing about "
                       "{%s}, which its processors ask for",
                       nuvasha_loader_display_name (request->loader),
                       request->loader_version, name);
          return FALSE;
        }

      g_hash_table_insert (request->variables, g_strdup (name),
                           g_strdup (value));
      return TRUE;
    }

  length = strlen (value);

  /* An artifact of the maven of the loader, written as "[coordinate]". */
  if (length > 2 && value[0] == '[' && value[length - 1] == ']')
    {
      g_autofree char *coordinate = g_strndup (value + 1, length - 2);
      g_autofree char *path = artifact_path (request, coordinate);

      if (path == NULL)
        {
          g_set_error (error, nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_MAVEN,
                       "\"%s\" is no path an artifact of %s can live at",
                       coordinate, nuvasha_loader_display_name (request->loader));
          return FALSE;
        }

      queue_artifact (request, coordinate, TRUE);

      if (g_str_equal (name, "PATCHED"))
        {
          g_free (request->patched_jar);
          request->patched_jar = g_strdup (path);
        }

      g_hash_table_insert (request->variables, g_strdup (name),
                           g_steal_pointer (&path));
      return TRUE;
    }

  /* A file inside the installer jar, written as a path in it. */
  if (value[0] == '/')
    {
      const char *entry = value;
      g_autofree char *path = NULL;

      while (*entry == '/')
        entry++;

      if (*entry == '\0' || strstr (entry, "..") != NULL ||
          !nuvasha_resolve_entry_name_is_safe (entry))
        {
          g_set_error (error, nuvasha_LOADERS_ERROR,
                       nuvasha_LOADERS_ERROR_INSTALLER,
                       "the installer names an unsafe unpacked path \"%s\"",
                       value);
          return FALSE;
        }

      queue_unpacked (request, entry);
      path = g_build_filename (request->root, entry, NULL);

      g_hash_table_insert (request->variables, g_strdup (name),
                           g_steal_pointer (&path));
      return TRUE;
    }

  /* Anything else is a value the profile gives as it is: a mapping name, a
     version, a date. */
  g_hash_table_insert (request->variables, g_strdup (name), g_strdup (value));

  return TRUE;
}

/* One argument of a processor, with every "{NAME}" in it replaced by what
 * the name stands for. */
static char *
substitute_argument (InstallRequest *request, const char *argument,
                     GError **error)
{
  g_autoptr (GString) result = g_string_new (NULL);

  for (const char *p = argument; *p != '\0'; p++)
    {
      const char *end = NULL;
      g_autofree char *name = NULL;
      const char *value = NULL;

      if (*p != '{')
        {
          g_string_append_c (result, *p);
          continue;
        }

      end = strchr (p, '}');

      /* A brace the profile did not open is a brace. */
      if (end == NULL)
        {
          g_string_append_c (result, *p);
          continue;
        }

      name = g_strndup (p + 1, end - p - 1);

      if (!resolve_variable (request, name, error))
        return NULL;

      value = g_hash_table_lookup (request->variables, name);
      g_string_append (result, value != NULL ? value : name);

      p = end;
    }

  return g_strdup (result->str);
}
  /* Resolve processor variables and required JARs before execution. */
void
stage_processor (InstallRequest *request)
{
  JsonNode *entry = NULL;
  JsonObject *processor = NULL;
  JsonNode *node = NULL;
  g_autoptr (GError) error = NULL;

  request->stage = STAGE_VARIABLES;

  if (request->index >= request->processors->len)
    {
      install_failed (request,
                      g_error_new (nuvasha_LOADERS_ERROR,
                                   nuvasha_LOADERS_ERROR_INSTALLER,
                                   "the install of %s %s has no processor to "
                                   "run",
                                   nuvasha_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  entry = g_ptr_array_index (request->processors, request->index);
  processor = json_node_get_object (entry);

  /* Each processor gets names of its own: two of them may well use the
     same name for different things. */
  g_clear_pointer (&request->variables, g_hash_table_unref);
  request->variables = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              g_free);
  g_clear_pointer (&request->main_class, g_free);

  /* Resolve the files referenced by processor arguments. */
  node = json_object_get_member (processor, "args");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          g_autofree char *argument = NULL;

          if (!JSON_NODE_HOLDS_VALUE (element) ||
              json_node_get_value_type (element) != G_TYPE_STRING)
            continue;

          argument = substitute_argument (
            request, json_node_get_string (element), &error);

          if (argument == NULL)
            {
              install_failed (request, g_steal_pointer (&error));
              return;
            }
        }
    }

  /* Then the jars it runs with: the tools of the processor, and the jar of
     the processor itself, which is where the class to run is read from. */
  node = json_object_get_member (processor, "classpath");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);

          if (JSON_NODE_HOLDS_VALUE (element) &&
              json_node_get_value_type (element) == G_TYPE_STRING)
            queue_artifact (request, json_node_get_string (element), FALSE);
        }
    }

  node = json_object_get_member (processor, "jar");

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    queue_artifact (request, json_node_get_string (node), FALSE);

  drain_pending (request);
}

/* Run one processor using the configured Java and processor main class. */
void
stage_run (InstallRequest *request)
{
  JsonObject *processor =
    json_node_get_object (g_ptr_array_index (request->processors,
                                             request->index));
  g_autoptr (GPtrArray) argv = NULL;
  g_autoptr (GString) classpath = NULL;
  g_autoptr (GError) error = NULL;
  JsonNode *node = NULL;
  const char *tool = NULL;

  request->stage = STAGE_RUN;

  node = json_object_get_member (processor, "jar");

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      install_failed (request,
                      g_error_new (nuvasha_LOADERS_ERROR,
                                   nuvasha_LOADERS_ERROR_INSTALLER,
                                   "a processor of %s %s names no jar to run",
                                   nuvasha_loader_display_name (request->loader),
                                   request->loader_version));
      return;
    }

  tool = json_node_get_string (node);

  {
    g_autofree char *jar = artifact_path (request, tool);

    if (jar == NULL)
      {
        install_failed (
          request,
          g_error_new (nuvasha_LOADERS_ERROR, nuvasha_LOADERS_ERROR_NO_MAVEN,
                       "\"%s\" is no path a processor of %s can live at", tool,
                       nuvasha_loader_display_name (request->loader)));
        return;
      }

    request->main_class = nuvasha_loader_jar_main_class (jar, &error);

    if (request->main_class == NULL)
      {
        install_failed (request, g_steal_pointer (&error));
        return;
      }
  }

  /* The class path of a processor is the jar of the processor and the jars
     it runs with: NeoForge names its tool among them and Forge does not,
     and a tool that is not on its own class path has no class to run. */
  classpath = g_string_new (NULL);

  {
    g_autofree char *own = artifact_path (request, tool);

    if (own != NULL)
      g_string_append (classpath, own);
  }

  node = json_object_get_member (processor, "classpath");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          g_autofree char *path = NULL;

          if (!JSON_NODE_HOLDS_VALUE (element) ||
              json_node_get_value_type (element) != G_TYPE_STRING)
            continue;

          path = artifact_path (request, json_node_get_string (element));

          if (path == NULL)
            continue;

          if (classpath->len > 0)
            g_string_append_c (classpath, ':');

          g_string_append (classpath, path);
        }
    }

  argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup (request->java));
  g_ptr_array_add (argv, g_strdup ("-cp"));
  g_ptr_array_add (argv, g_strdup (classpath->str));
  g_ptr_array_add (argv, g_strdup (request->main_class));

  node = json_object_get_member (processor, "args");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *array = json_node_get_array (node);

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          g_autofree char *argument = NULL;

          if (!JSON_NODE_HOLDS_VALUE (element) ||
              json_node_get_value_type (element) != G_TYPE_STRING)
            continue;

          argument = substitute_argument (
            request, json_node_get_string (element), &error);

          if (argument == NULL)
            {
              install_failed (request, g_steal_pointer (&error));
              return;
            }

          g_ptr_array_add (argv, g_steal_pointer (&argument));
        }
    }

  g_ptr_array_add (argv, NULL);

  {
    g_autoptr (GSubprocess) process = NULL;

    process = g_subprocess_newv (
      (const char *const *) argv->pdata,
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
      &error);

    if (process == NULL)
      {
        install_failed (request, g_steal_pointer (&error));
        return;
      }

    g_subprocess_communicate_utf8_async (
      process, NULL, g_task_get_cancellable (request->task),
      processor_finished, request);
  }
}
