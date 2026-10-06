/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlSettings: reading and writing <config dir>/AML/config.json. */

#include "aml-settings.h"

#include <errno.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Everything below g_get_user_config_dir(). */
#define SETTINGS_SUBDIR "AML"
#define SETTINGS_FILE "config.json"

/* The keys the file holds for now. */
#define SHOW_SNAPSHOTS_KEY "show-snapshots"
#define DEFAULT_JAVA_PATH_KEY "default-java-path"
#define LOG_LINE_LIMIT_KEY "log-line-limit"
#define OPEN_LOGS_AFTER_CRASH_KEY "open-logs-after-crash"
#define SHOW_UNSTABLE_LOADERS_KEY "show-unstable-loaders"

/* A stored limit below this is not a number a log can be trimmed to. */
#define LOG_LINE_LIMIT_FLOOR 1

struct _AmlSettings
{
  GObject parent_instance;

  gboolean show_snapshots;
  char *default_java_path;
  guint log_line_limit;
  gboolean open_logs_after_crash;
  gboolean show_unstable_loaders;
  char *filename;
};

G_DEFINE_FINAL_TYPE (AmlSettings, aml_settings, G_TYPE_OBJECT)

GQuark
aml_settings_error_quark (void)
{
  return g_quark_from_static_string ("aml-settings-error");
}

static char *
settings_filename (void)
{
  g_autofree char *directory =
    g_build_filename (g_get_user_config_dir (), SETTINGS_SUBDIR, NULL);

  return g_build_filename (directory, SETTINGS_FILE, NULL);
}

/* Nothing in here is an error the user has to hear about: a settings
 * file that is missing or broken simply means the defaults. */
static void
load (AmlSettings *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autofree char *contents = NULL;
  JsonNode *root;
  JsonObject *object;
  JsonNode *node;
  gsize length = 0;

  if (!g_file_get_contents (self->filename, &contents, &length, &error))
    {
      g_debug ("the settings were not read: %s", error->message);
      return;
    }

  parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, contents, length, NULL))
    {
      g_debug ("\"%s\" does not hold JSON", self->filename);
      return;
    }

  root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_debug ("\"%s\" does not hold an object", self->filename);
      return;
    }

  object = json_node_get_object (root);
  node = json_object_get_member (object, SHOW_SNAPSHOTS_KEY);

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->show_snapshots = json_node_get_boolean (node);

  node = json_object_get_member (object, DEFAULT_JAVA_PATH_KEY);

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    {
      const char *path = json_node_get_string (node);

      g_free (self->default_java_path);
      self->default_java_path =
        (path != NULL && *path != '\0') ? g_strdup (path) : NULL;
    }

  node = json_object_get_member (object, LOG_LINE_LIMIT_KEY);

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_INT64)
    {
      gint64 stored = json_node_get_int (node);

      /* A limit that is not a usable number is left at the default
       * rather than carried into the running log. */
      if (stored >= LOG_LINE_LIMIT_FLOOR && stored <= G_MAXUINT)
        self->log_line_limit = (guint) stored;
    }

  node = json_object_get_member (object, OPEN_LOGS_AFTER_CRASH_KEY);

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->open_logs_after_crash = json_node_get_boolean (node);

  node = json_object_get_member (object, SHOW_UNSTABLE_LOADERS_KEY);

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->show_unstable_loaders = json_node_get_boolean (node);
}

static gboolean
save (AmlSettings *self, GError **error)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (GError) io_error = NULL;
  g_autofree char *directory = NULL;
  g_autofree char *data = NULL;
  gsize length = 0;

  directory = g_path_get_dirname (self->filename);

  if (g_mkdir_with_parents (directory, 0755) != 0)
    {
      g_set_error (error, AML_SETTINGS_ERROR, AML_SETTINGS_ERROR_IO,
                   "cannot create \"%s\": %s", directory,
                   g_strerror (errno));
      return FALSE;
    }

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, SHOW_SNAPSHOTS_KEY);
  json_builder_add_boolean_value (builder, self->show_snapshots);

  /* NULL means auto-select the closest compatible one per instance. */
  json_builder_set_member_name (builder, DEFAULT_JAVA_PATH_KEY);
  if (self->default_java_path != NULL)
    json_builder_add_string_value (builder, self->default_java_path);
  else
    json_builder_add_null_value (builder);

  json_builder_set_member_name (builder, LOG_LINE_LIMIT_KEY);
  json_builder_add_int_value (builder, (gint64) self->log_line_limit);

  json_builder_set_member_name (builder, OPEN_LOGS_AFTER_CRASH_KEY);
  json_builder_add_boolean_value (builder, self->open_logs_after_crash);

  json_builder_set_member_name (builder, SHOW_UNSTABLE_LOADERS_KEY);
  json_builder_add_boolean_value (builder, self->show_unstable_loaders);

  json_builder_end_object (builder);

  /* The builder hands out a copy of what it built and the generator makes
     a copy of what it is given: the first one is this function's to let
     go of when it is done with it. */
  {
    g_autoptr (JsonNode) root = json_builder_get_root (builder);

    json_generator_set_root (generator, root);
  }

  json_generator_set_pretty (generator, TRUE);
  data = json_generator_to_data (generator, &length);

  if (!g_file_set_contents_full (self->filename, data, length,
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_DURABLE,
                                 0600, &io_error))
    {
      g_set_error (error, AML_SETTINGS_ERROR, AML_SETTINGS_ERROR_IO,
                   "cannot write \"%s\": %s", self->filename,
                   io_error->message);
      return FALSE;
    }

  return TRUE;
}

AmlSettings *
aml_settings_new (void)
{
  AmlSettings *self = g_object_new (AML_TYPE_SETTINGS, NULL);

  load (self);

  return self;
}

gboolean
aml_settings_get_show_snapshots (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);

  return self->show_snapshots;
}

gboolean
aml_settings_set_show_snapshots (AmlSettings *self,
                                 gboolean     value,
                                 GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->show_snapshots;
  self->show_snapshots = value;

  if (save (self, error))
    return TRUE;

  /* The file did not change, so neither does the answer. */
  self->show_snapshots = previous;
  return FALSE;
}

const char *
aml_settings_get_default_java_path (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), NULL);

  return self->default_java_path;
}

gboolean
aml_settings_set_default_java_path (AmlSettings *self,
                                    const char *path,
                                    GError    **error)
{
  char *previous = NULL;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->default_java_path;
  self->default_java_path = g_strdup (path);

  if (save (self, error))
    {
      g_free (previous);
      return TRUE;
    }

  /* The file did not change, so neither does the answer. */
  g_free (self->default_java_path);
  self->default_java_path = previous;
  return FALSE;
}

guint
aml_settings_get_log_line_limit (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), 0);

  return self->log_line_limit;
}

gboolean
aml_settings_set_log_line_limit (AmlSettings *self,
                                 guint        limit,
                                 GError     **error)
{
  guint previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (limit < LOG_LINE_LIMIT_FLOOR)
    {
      g_set_error (error, AML_SETTINGS_ERROR, AML_SETTINGS_ERROR_INVALID,
                   "the log of a running game keeps at least %d line",
                   LOG_LINE_LIMIT_FLOOR);
      return FALSE;
    }

  previous = self->log_line_limit;
  self->log_line_limit = limit;

  if (save (self, error))
    return TRUE;

  /* The file did not change, so neither does the answer. */
  self->log_line_limit = previous;
  return FALSE;
}

gboolean
aml_settings_get_open_logs_after_crash (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);

  return self->open_logs_after_crash;
}

gboolean
aml_settings_set_open_logs_after_crash (AmlSettings *self,
                                        gboolean     value,
                                        GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->open_logs_after_crash;
  self->open_logs_after_crash = value;

  if (save (self, error))
    return TRUE;

  /* The file did not change, so neither does the answer. */
  self->open_logs_after_crash = previous;
  return FALSE;
}

gboolean
aml_settings_get_show_unstable_loaders (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);

  return self->show_unstable_loaders;
}

gboolean
aml_settings_set_show_unstable_loaders (AmlSettings *self,
                                        gboolean     value,
                                        GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->show_unstable_loaders;
  self->show_unstable_loaders = value;

  if (save (self, error))
    return TRUE;

  /* The file did not change, so neither does the answer. */
  self->show_unstable_loaders = previous;
  return FALSE;
}

static void
aml_settings_finalize (GObject *object)
{
  AmlSettings *self = AML_SETTINGS (object);

  g_free (self->default_java_path);
  g_free (self->filename);

  G_OBJECT_CLASS (aml_settings_parent_class)->finalize (object);
}

static void
aml_settings_class_init (AmlSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = aml_settings_finalize;
}

static void
aml_settings_init (AmlSettings *self)
{
  self->filename = settings_filename ();
  self->log_line_limit = AML_SETTINGS_DEFAULT_LOG_LINE_LIMIT;
}
