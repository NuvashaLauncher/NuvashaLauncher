/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlSettings: reading and writing <config dir>/AML/config.json. */

#include "aml-settings.h"
#include "aml-fs.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define SETTINGS_FORMAT_VERSION 1
#define CORRUPT_SUFFIX ".corrupt"

/* Everything below g_get_user_config_dir(). */
#define SETTINGS_SUBDIR "AML"
#define SETTINGS_FILE "config.json"

/* The keys the file holds for now. */
#define SHOW_SNAPSHOTS_KEY "show-snapshots"
#define LOG_LINE_LIMIT_KEY "log-line-limit"
#define OPEN_LOGS_AFTER_CRASH_KEY "open-logs-after-crash"
#define SHOW_UNSTABLE_LOADERS_KEY "show-unstable-loaders"
#define LANGUAGE_KEY "language"
#define CACHE_MODRINTH_API_RESPONSES_KEY "cache-modrinth-api-responses"
#define CACHE_EXPIRATION_KEY "cache-expiration"
#define EXPORT_METADATA_KEY "export-metadata"
#define DELETE_DEPENDENTS_KEY "delete-dependents"
#define SANDBOX_ENABLED_KEY "sandbox-enabled"
#define SANDBOX_NETWORK_KEY "sandbox-network"

/* A stored limit below this is not a number a log can be trimmed to. */
#define LOG_LINE_LIMIT_FLOOR 1

struct _AmlSettings
{
  GObject parent_instance;

  gboolean show_snapshots;
  guint log_line_limit;
  gboolean open_logs_after_crash;
  gboolean show_unstable_loaders;
  char *language;
  gboolean cache_modrinth_api_responses;
  AmlSettingsCacheExpiration cache_expiration;
  gboolean export_metadata;
  gboolean delete_dependents;
  gboolean sandbox_enabled;
  gboolean sandbox_network;
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

static void
quarantine_corrupt_file (const char *path)
{
  g_autofree char *base = g_strconcat (path, CORRUPT_SUFFIX, NULL);
  g_autofree char *candidate = NULL;

  for (guint i = 0; i < 1000; i++)
    {
      candidate = i == 0 ? g_strdup (base)
                         : g_strdup_printf ("%s.%u", base, i);
      if (!g_file_test (candidate, G_FILE_TEST_EXISTS))
        break;
      g_clear_pointer (&candidate, g_free);
    }

  if (candidate == NULL || !aml_fs_rename (path, candidate, NULL))
    g_warning ("could not quarantine the settings file \"%s\": %s",
               path, g_strerror (errno));
  else
    g_warning ("the settings file \"%s\" is corrupt; it was moved to \"%s\"",
               path, candidate);
}

/* A missing file means defaults; malformed files are quarantined so they can
 * be recovered instead of silently disappearing. */
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

  if (!aml_fs_get_contents_within (self->filename,
                                   AML_FS_MAX_SMALL_METADATA_SIZE,
                                   &contents, &length, &error))
    {
      g_debug ("the settings were not read: %s", error->message);
      return;
    }

  parser = json_parser_new ();

  /* The size check above already refused anything larger, so the length
     always fits the signed one the parser takes. */
  if (!json_parser_load_from_data (parser, contents, (gssize) length, NULL))
    {
      quarantine_corrupt_file (self->filename);
      return;
    }

  root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      quarantine_corrupt_file (self->filename);
      return;
    }

  object = json_node_get_object (root);
  node = json_object_get_member (object, "version");

  /* Version 0 is the original format, which had no version member. */
  if (node != NULL &&
      (!JSON_NODE_HOLDS_VALUE (node) ||
       json_node_get_value_type (node) != G_TYPE_INT64 ||
       json_node_get_int (node) != SETTINGS_FORMAT_VERSION))
    {
      quarantine_corrupt_file (self->filename);
      return;
    }

  node = json_object_get_member (object, SHOW_SNAPSHOTS_KEY);

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->show_snapshots = json_node_get_boolean (node);

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

  node = json_object_get_member (object, LANGUAGE_KEY);
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING)
    {
      const char *language = json_node_get_string (node);

      if (language != NULL && *language != '\0')
        self->language = g_strdup (language);
    }

  node = json_object_get_member (object, CACHE_MODRINTH_API_RESPONSES_KEY);
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->cache_modrinth_api_responses = json_node_get_boolean (node);

  node = json_object_get_member (object, CACHE_EXPIRATION_KEY);
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_INT64)
    {
      gint64 stored = json_node_get_int (node);

      if (stored == AML_SETTINGS_CACHE_EXPIRATION_1_HOUR ||
          stored == AML_SETTINGS_CACHE_EXPIRATION_6_HOURS ||
          stored == AML_SETTINGS_CACHE_EXPIRATION_1_DAY ||
          stored == AML_SETTINGS_CACHE_EXPIRATION_7_DAYS)
        self->cache_expiration = (AmlSettingsCacheExpiration) stored;
    }

  node = json_object_get_member (object, EXPORT_METADATA_KEY);
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->export_metadata = json_node_get_boolean (node);

  node = json_object_get_member (object, DELETE_DEPENDENTS_KEY);
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->delete_dependents = json_node_get_boolean (node);

  node = json_object_get_member (object, SANDBOX_ENABLED_KEY);
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->sandbox_enabled = json_node_get_boolean (node);

  node = json_object_get_member (object, SANDBOX_NETWORK_KEY);
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_BOOLEAN)
    self->sandbox_network = json_node_get_boolean (node);
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

  if (!aml_fs_make_private_directory (directory, &io_error))
    {
      g_set_error (error, AML_SETTINGS_ERROR, AML_SETTINGS_ERROR_IO,
                   "%s", io_error->message);
      return FALSE;
    }

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "version");
  json_builder_add_int_value (builder, SETTINGS_FORMAT_VERSION);
  json_builder_set_member_name (builder, SHOW_SNAPSHOTS_KEY);
  json_builder_add_boolean_value (builder, self->show_snapshots);

  json_builder_set_member_name (builder, LOG_LINE_LIMIT_KEY);
  json_builder_add_int_value (builder, (gint64) self->log_line_limit);

  json_builder_set_member_name (builder, OPEN_LOGS_AFTER_CRASH_KEY);
  json_builder_add_boolean_value (builder, self->open_logs_after_crash);

  json_builder_set_member_name (builder, SHOW_UNSTABLE_LOADERS_KEY);
  json_builder_add_boolean_value (builder, self->show_unstable_loaders);

  json_builder_set_member_name (builder, LANGUAGE_KEY);
  json_builder_add_string_value (builder, self->language != NULL
                                            ? self->language
                                            : "system");

  json_builder_set_member_name (builder, CACHE_MODRINTH_API_RESPONSES_KEY);
  json_builder_add_boolean_value (builder, self->cache_modrinth_api_responses);

  json_builder_set_member_name (builder, CACHE_EXPIRATION_KEY);
  json_builder_add_int_value (builder, (gint64) self->cache_expiration);

  json_builder_set_member_name (builder, EXPORT_METADATA_KEY);
  json_builder_add_boolean_value (builder, self->export_metadata);
  json_builder_set_member_name (builder, DELETE_DEPENDENTS_KEY);
  json_builder_add_boolean_value (builder, self->delete_dependents);
  json_builder_set_member_name (builder, SANDBOX_ENABLED_KEY);
  json_builder_add_boolean_value (builder, self->sandbox_enabled);
  json_builder_set_member_name (builder, SANDBOX_NETWORK_KEY);
  json_builder_add_boolean_value (builder, self->sandbox_network);

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

  /* What we just generated always fits the signed one the writer takes. */
  if (!aml_fs_replace_contents (self->filename, data, (gssize) length, 0600,
                                &io_error))
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
aml_settings_get_export_metadata (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), TRUE);

  return self->export_metadata;
}

gboolean
aml_settings_set_export_metadata (AmlSettings *self, gboolean value,
                                  GError **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->export_metadata;
  self->export_metadata = value;
  if (save (self, error))
    return TRUE;

  self->export_metadata = previous;
  return FALSE;
}

gboolean
aml_settings_get_delete_dependents (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  return self->delete_dependents;
}

gboolean
aml_settings_set_delete_dependents (AmlSettings *self, gboolean value,
                                    GError **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->delete_dependents;
  self->delete_dependents = value;
  if (save (self, error))
    return TRUE;

  self->delete_dependents = previous;
  return FALSE;
}

gboolean
aml_settings_get_sandbox_enabled (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), TRUE);
  return self->sandbox_enabled;
}

gboolean
aml_settings_set_sandbox_enabled (AmlSettings *self, gboolean value,
                                   GError **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->sandbox_enabled;
  self->sandbox_enabled = value;
  if (save (self, error))
    return TRUE;
  self->sandbox_enabled = previous;
  return FALSE;
}

gboolean
aml_settings_get_sandbox_network (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), TRUE);
  return self->sandbox_network;
}

gboolean
aml_settings_set_sandbox_network (AmlSettings *self, gboolean value,
                                   GError **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->sandbox_network;
  self->sandbox_network = value;
  if (save (self, error))
    return TRUE;
  self->sandbox_network = previous;
  return FALSE;
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

const char *
aml_settings_get_language (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), "system");

  return self->language != NULL ? self->language : "system";
}

gboolean
aml_settings_set_language (AmlSettings *self,
                           const char *language,
                           GError    **error)
{
  char *previous = NULL;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (language != NULL && *language != '\0', FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->language;
  self->language = g_strdup (language);

  if (save (self, error))
    {
      g_free (previous);
      return TRUE;
    }

  g_free (self->language);
  self->language = previous;
  return FALSE;
}

gboolean
aml_settings_get_cache_modrinth_api_responses (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);

  return self->cache_modrinth_api_responses;
}

gboolean
aml_settings_set_cache_modrinth_api_responses (AmlSettings *self,
                                               gboolean     value,
                                               GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->cache_modrinth_api_responses;
  self->cache_modrinth_api_responses = value;
  if (save (self, error))
    return TRUE;

  self->cache_modrinth_api_responses = previous;
  return FALSE;
}

AmlSettingsCacheExpiration
aml_settings_get_cache_expiration (AmlSettings *self)
{
  g_return_val_if_fail (AML_IS_SETTINGS (self),
                        AML_SETTINGS_DEFAULT_CACHE_EXPIRATION);

  return self->cache_expiration;
}

gboolean
aml_settings_set_cache_expiration (AmlSettings *self,
                                   AmlSettingsCacheExpiration value,
                                   GError **error)
{
  AmlSettingsCacheExpiration previous;

  g_return_val_if_fail (AML_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (value != AML_SETTINGS_CACHE_EXPIRATION_1_HOUR &&
      value != AML_SETTINGS_CACHE_EXPIRATION_6_HOURS &&
      value != AML_SETTINGS_CACHE_EXPIRATION_1_DAY &&
      value != AML_SETTINGS_CACHE_EXPIRATION_7_DAYS)
    {
      g_set_error (error, AML_SETTINGS_ERROR, AML_SETTINGS_ERROR_INVALID,
                   "the cache expiration interval is not supported");
      return FALSE;
    }

  previous = self->cache_expiration;
  self->cache_expiration = value;
  if (save (self, error))
    return TRUE;

  self->cache_expiration = previous;
  return FALSE;
}

static void
aml_settings_finalize (GObject *object)
{
  AmlSettings *self = AML_SETTINGS (object);

  g_free (self->language);
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
  self->cache_modrinth_api_responses = TRUE;
  self->cache_expiration = AML_SETTINGS_DEFAULT_CACHE_EXPIRATION;
  self->export_metadata = TRUE;
  self->sandbox_enabled = TRUE;
  self->sandbox_network = TRUE;
}
