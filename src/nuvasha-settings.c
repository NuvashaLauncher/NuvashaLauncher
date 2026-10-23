/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaSettings: desktop preferences backed by GSettings. */

#include "nuvasha-settings.h"

/* The keys stored by the desktop settings schema. */
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
#define SANDBOX_NETWORK_ENABLED_KEY "sandbox-network-enabled"

/* A stored limit below this is not a number a log can be trimmed to. */
#define LOG_LINE_LIMIT_FLOOR 1

struct _NuvashaSettings
{
  GObject parent_instance;

  gboolean show_snapshots;
  guint log_line_limit;
  gboolean open_logs_after_crash;
  gboolean show_unstable_loaders;
  char *language;
  gboolean cache_modrinth_api_responses;
  NuvashaSettingsCacheExpiration cache_expiration;
  gboolean export_metadata;
  gboolean delete_dependents;
  gboolean sandbox_enabled;
  gboolean sandbox_network_enabled;
  GSettings *desktop_settings;
};

G_DEFINE_FINAL_TYPE (NuvashaSettings, nuvasha_settings, G_TYPE_OBJECT)

GQuark
nuvasha_settings_error_quark (void)
{
  return g_quark_from_static_string ("nuvasha-settings-error");
}

/* Settings are owned by the desktop settings backend. */
static void
load (NuvashaSettings *self)
{
  self->show_snapshots = g_settings_get_boolean (self->desktop_settings,
                                                  SHOW_SNAPSHOTS_KEY);
  self->log_line_limit = MAX (LOG_LINE_LIMIT_FLOOR,
                               g_settings_get_uint (self->desktop_settings,
                                                    LOG_LINE_LIMIT_KEY));
  self->open_logs_after_crash = g_settings_get_boolean (
    self->desktop_settings, OPEN_LOGS_AFTER_CRASH_KEY);
  self->show_unstable_loaders = g_settings_get_boolean (
    self->desktop_settings, SHOW_UNSTABLE_LOADERS_KEY);
  g_free (self->language);
  self->language = g_settings_get_string (self->desktop_settings, LANGUAGE_KEY);
  self->cache_modrinth_api_responses = g_settings_get_boolean (
    self->desktop_settings, CACHE_MODRINTH_API_RESPONSES_KEY);
  self->cache_expiration = g_settings_get_uint (
    self->desktop_settings, CACHE_EXPIRATION_KEY);
  self->export_metadata = g_settings_get_boolean (
    self->desktop_settings, EXPORT_METADATA_KEY);
  self->delete_dependents = g_settings_get_boolean (
    self->desktop_settings, DELETE_DEPENDENTS_KEY);
  self->sandbox_enabled = g_settings_get_boolean (
    self->desktop_settings, SANDBOX_ENABLED_KEY);
  self->sandbox_network_enabled = g_settings_get_boolean (
    self->desktop_settings, SANDBOX_NETWORK_ENABLED_KEY);
}

static gboolean
save (NuvashaSettings *self, GError **error)
{
  g_return_val_if_fail (self->desktop_settings != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_settings_set_boolean (self->desktop_settings, SHOW_SNAPSHOTS_KEY,
                          self->show_snapshots);
  g_settings_set_uint (self->desktop_settings, LOG_LINE_LIMIT_KEY,
                       self->log_line_limit);
  g_settings_set_boolean (self->desktop_settings, OPEN_LOGS_AFTER_CRASH_KEY,
                          self->open_logs_after_crash);
  g_settings_set_boolean (self->desktop_settings, SHOW_UNSTABLE_LOADERS_KEY,
                          self->show_unstable_loaders);
  g_settings_set_string (self->desktop_settings, LANGUAGE_KEY,
                         self->language != NULL ? self->language : "");
  g_settings_set_boolean (self->desktop_settings,
                          CACHE_MODRINTH_API_RESPONSES_KEY,
                          self->cache_modrinth_api_responses);
  g_settings_set_uint (self->desktop_settings, CACHE_EXPIRATION_KEY,
                       self->cache_expiration);
  g_settings_set_boolean (self->desktop_settings, EXPORT_METADATA_KEY,
                          self->export_metadata);
  g_settings_set_boolean (self->desktop_settings, DELETE_DEPENDENTS_KEY,
                          self->delete_dependents);
  g_settings_set_boolean (self->desktop_settings, SANDBOX_ENABLED_KEY,
                          self->sandbox_enabled);
  g_settings_set_boolean (self->desktop_settings,
                          SANDBOX_NETWORK_ENABLED_KEY,
                          self->sandbox_network_enabled);
  return TRUE;
}

NuvashaSettings *
nuvasha_settings_new (void)
{
  NuvashaSettings *self = g_object_new (nuvasha_TYPE_SETTINGS, NULL);

  load (self);

  return self;
}

gboolean
nuvasha_settings_get_export_metadata (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), TRUE);

  return self->export_metadata;
}

gboolean
nuvasha_settings_set_export_metadata (NuvashaSettings *self, gboolean value,
                                  GError **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->export_metadata;
  self->export_metadata = value;
  if (save (self, error))
    return TRUE;

  self->export_metadata = previous;
  return FALSE;
}

gboolean
nuvasha_settings_get_delete_dependents (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  return self->delete_dependents;
}

gboolean
nuvasha_settings_set_delete_dependents (NuvashaSettings *self, gboolean value,
                                    GError **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->delete_dependents;
  self->delete_dependents = value;
  if (save (self, error))
    return TRUE;

  self->delete_dependents = previous;
  return FALSE;
}

gboolean
nuvasha_settings_get_sandbox_enabled (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);

  return self->sandbox_enabled;
}

gboolean
nuvasha_settings_set_sandbox_enabled (NuvashaSettings *self, gboolean value,
                                  GError **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->sandbox_enabled;
  self->sandbox_enabled = value;
  if (save (self, error))
    return TRUE;

  self->sandbox_enabled = previous;
  return FALSE;
}

gboolean
nuvasha_settings_get_sandbox_network_enabled (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), TRUE);

  return self->sandbox_network_enabled;
}

gboolean
nuvasha_settings_set_sandbox_network_enabled (NuvashaSettings *self, gboolean value,
                                           GError **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->sandbox_network_enabled;
  self->sandbox_network_enabled = value;
  if (save (self, error))
    return TRUE;

  self->sandbox_network_enabled = previous;
  return FALSE;
}

gboolean
nuvasha_settings_get_show_snapshots (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);

  return self->show_snapshots;
}

gboolean
nuvasha_settings_set_show_snapshots (NuvashaSettings *self,
                                 gboolean     value,
                                 GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
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
nuvasha_settings_get_log_line_limit (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), 0);

  return self->log_line_limit;
}

gboolean
nuvasha_settings_set_log_line_limit (NuvashaSettings *self,
                                 guint        limit,
                                 GError     **error)
{
  guint previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (limit < LOG_LINE_LIMIT_FLOOR)
    {
      g_set_error (error, nuvasha_SETTINGS_ERROR, nuvasha_SETTINGS_ERROR_INVALID,
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
nuvasha_settings_get_open_logs_after_crash (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);

  return self->open_logs_after_crash;
}

gboolean
nuvasha_settings_set_open_logs_after_crash (NuvashaSettings *self,
                                        gboolean     value,
                                        GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
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
nuvasha_settings_get_show_unstable_loaders (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);

  return self->show_unstable_loaders;
}

gboolean
nuvasha_settings_set_show_unstable_loaders (NuvashaSettings *self,
                                        gboolean     value,
                                        GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
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
nuvasha_settings_get_language (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), "system");

  return self->language != NULL ? self->language : "system";
}

gboolean
nuvasha_settings_set_language (NuvashaSettings *self,
                           const char *language,
                           GError    **error)
{
  char *previous = NULL;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
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
nuvasha_settings_get_cache_modrinth_api_responses (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);

  return self->cache_modrinth_api_responses;
}

gboolean
nuvasha_settings_set_cache_modrinth_api_responses (NuvashaSettings *self,
                                               gboolean     value,
                                               GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->cache_modrinth_api_responses;
  self->cache_modrinth_api_responses = value;
  if (save (self, error))
    return TRUE;

  self->cache_modrinth_api_responses = previous;
  return FALSE;
}

NuvashaSettingsCacheExpiration
nuvasha_settings_get_cache_expiration (NuvashaSettings *self)
{
  g_return_val_if_fail (nuvasha_IS_SETTINGS (self),
                        nuvasha_SETTINGS_DEFAULT_CACHE_EXPIRATION);

  return self->cache_expiration;
}

gboolean
nuvasha_settings_set_cache_expiration (NuvashaSettings *self,
                                   NuvashaSettingsCacheExpiration value,
                                   GError **error)
{
  NuvashaSettingsCacheExpiration previous;

  g_return_val_if_fail (nuvasha_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (value != nuvasha_SETTINGS_CACHE_EXPIRATION_1_HOUR &&
      value != nuvasha_SETTINGS_CACHE_EXPIRATION_6_HOURS &&
      value != nuvasha_SETTINGS_CACHE_EXPIRATION_1_DAY &&
      value != nuvasha_SETTINGS_CACHE_EXPIRATION_7_DAYS)
    {
      g_set_error (error, nuvasha_SETTINGS_ERROR, nuvasha_SETTINGS_ERROR_INVALID,
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
nuvasha_settings_finalize (GObject *object)
{
  NuvashaSettings *self = nuvasha_SETTINGS (object);

  g_free (self->language);
  g_clear_object (&self->desktop_settings);

  G_OBJECT_CLASS (nuvasha_settings_parent_class)->finalize (object);
}

static void
nuvasha_settings_class_init (NuvashaSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nuvasha_settings_finalize;
}

static void
nuvasha_settings_init (NuvashaSettings *self)
{
  {
    GSettingsSchemaSource *source = NULL;
    GSettingsSchemaSource *default_source;
    g_autoptr (GError) schema_error = NULL;

    if (g_file_test (nuvasha_BUILD_SCHEMA_DIR, G_FILE_TEST_IS_DIR))
      source = g_settings_schema_source_new_from_directory (
        nuvasha_BUILD_SCHEMA_DIR, NULL, FALSE, &schema_error);
    default_source = g_settings_schema_source_get_default ();
    if (source == NULL)
      source = default_source;
    GSettingsSchema *schema = source != NULL
      ? g_settings_schema_source_lookup (source,
                                         "com.github.nuvashalauncher.Nuvasha",
                                         TRUE)
      : NULL;

    g_return_if_fail (schema != NULL);
    self->desktop_settings = g_settings_new_full (schema, NULL, NULL);
    g_settings_schema_unref (schema);
    if (source != default_source && source != NULL)
      g_settings_schema_source_unref (source);
  }
  self->log_line_limit = nuvasha_SETTINGS_DEFAULT_LOG_LINE_LIMIT;
  self->cache_modrinth_api_responses = TRUE;
  self->cache_expiration = nuvasha_SETTINGS_DEFAULT_CACHE_EXPIRATION;
  self->export_metadata = TRUE;
  self->sandbox_network_enabled = TRUE;
}
