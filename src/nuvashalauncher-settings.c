/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherSettings: desktop preferences backed by GSettings. */

#include "nuvashalauncher-settings.h"

/* The keys stored by the desktop settings schema. */
#define SHOW_UNSTABLE_VERSIONS_KEY "show-unstable-versions"
#define LOG_LINE_LIMIT_KEY "log-line-limit"
#define SHOW_UNSTABLE_LOADERS_KEY "show-unstable-loaders"
#define LANGUAGE_KEY "language"
#define CACHE_MODRINTH_API_RESPONSES_KEY "cache-modrinth-api-responses"
#define CACHE_EXPIRATION_KEY "cache-expiration"
#define EXPORT_METADATA_KEY "export-metadata"
#define DELETE_DEPENDENTS_KEY "delete-dependents"
#define DEFAULT_MEMORY_MIB_KEY "default-memory-mib"
#define DEFAULT_JVM_ARGS_KEY "default-jvm-args"
#define DEFAULT_GAME_WIDTH_KEY "default-game-width"
#define DEFAULT_GAME_HEIGHT_KEY "default-game-height"
#define DEFAULT_GAME_FULLSCREEN_KEY "default-game-fullscreen"

/* A stored limit below this is not a number a log can be trimmed to. */
#define LOG_LINE_LIMIT_FLOOR 1

struct _NuvashaLauncherSettings
{
  GObject parent_instance;

  gboolean show_unstable_versions;
  guint log_line_limit;
  gboolean show_unstable_loaders;
  char *language;
  gboolean cache_modrinth_api_responses;
  NuvashaLauncherSettingsCacheExpiration cache_expiration;
  gboolean export_metadata;
  gboolean delete_dependents;
  guint default_memory_mib;
  char *default_jvm_args;
  guint default_game_width;
  guint default_game_height;
  gboolean default_game_fullscreen;
  GSettings *desktop_settings;
};

G_DEFINE_FINAL_TYPE (NuvashaLauncherSettings, nuvashalauncher_settings, G_TYPE_OBJECT)

GQuark
nuvashalauncher_settings_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-settings-error");
}

/* Settings are owned by the desktop settings backend. */
static void
load (NuvashaLauncherSettings *self)
{
  if (self->desktop_settings == NULL)
    return;

  self->show_unstable_versions = g_settings_get_boolean (
    self->desktop_settings, SHOW_UNSTABLE_VERSIONS_KEY);
  self->log_line_limit = MAX (LOG_LINE_LIMIT_FLOOR,
                               g_settings_get_uint (self->desktop_settings,
                                                    LOG_LINE_LIMIT_KEY));
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
  {
    guint mem = g_settings_get_uint (self->desktop_settings, DEFAULT_MEMORY_MIB_KEY);
    self->default_memory_mib = mem >= 512 ? mem : NUVASHALAUNCHER_SETTINGS_DEFAULT_MEMORY_MIB;
  }
  g_free (self->default_jvm_args);
  self->default_jvm_args = g_settings_get_string (self->desktop_settings, DEFAULT_JVM_ARGS_KEY);
  {
    guint w = g_settings_get_uint (self->desktop_settings, DEFAULT_GAME_WIDTH_KEY);
    guint h = g_settings_get_uint (self->desktop_settings, DEFAULT_GAME_HEIGHT_KEY);
    self->default_game_width = w >= 320 ? w : NUVASHALAUNCHER_SETTINGS_DEFAULT_GAME_WIDTH;
    self->default_game_height = h >= 240 ? h : NUVASHALAUNCHER_SETTINGS_DEFAULT_GAME_HEIGHT;
  }
  self->default_game_fullscreen = g_settings_get_boolean (
    self->desktop_settings, DEFAULT_GAME_FULLSCREEN_KEY);
}

static gboolean
save_boolean (NuvashaLauncherSettings *self, const char *key, gboolean value)
{
  if (self->desktop_settings != NULL)
    g_settings_set_boolean (self->desktop_settings, key, value);
  return TRUE;
}

static gboolean
save_uint (NuvashaLauncherSettings *self, const char *key, guint value)
{
  if (self->desktop_settings != NULL)
    g_settings_set_uint (self->desktop_settings, key, value);
  return TRUE;
}

static gboolean
save_string (NuvashaLauncherSettings *self, const char *key, const char *value)
{
  if (self->desktop_settings != NULL)
    g_settings_set_string (self->desktop_settings, key,
                           value != NULL ? value : "");
  return TRUE;
}

NuvashaLauncherSettings *
nuvashalauncher_settings_new (void)
{
  return g_object_new (NUVASHALAUNCHER_TYPE_SETTINGS, NULL);
}

gboolean
nuvashalauncher_settings_get_export_metadata (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), TRUE);

  return self->export_metadata;
}

gboolean
nuvashalauncher_settings_set_export_metadata (NuvashaLauncherSettings *self, gboolean value,
                                  GError **error)
{
  gboolean previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->export_metadata;
  self->export_metadata = value;
  if (save_boolean (self, EXPORT_METADATA_KEY, value))
    return TRUE;

  self->export_metadata = previous;
  return FALSE;
}

gboolean
nuvashalauncher_settings_get_delete_dependents (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  return self->delete_dependents;
}

gboolean
nuvashalauncher_settings_set_delete_dependents (NuvashaLauncherSettings *self, gboolean value,
                                    GError **error)
{
  gboolean previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->delete_dependents;
  self->delete_dependents = value;
  if (save_boolean (self, DELETE_DEPENDENTS_KEY, value))
    return TRUE;

  self->delete_dependents = previous;
  return FALSE;
}

gboolean
nuvashalauncher_settings_get_show_unstable_versions (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);

  return self->show_unstable_versions;
}

gboolean
nuvashalauncher_settings_set_show_unstable_versions (NuvashaLauncherSettings *self,
                                           gboolean     value,
                                           GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->show_unstable_versions;
  self->show_unstable_versions = value;

  if (save_boolean (self, SHOW_UNSTABLE_VERSIONS_KEY, value))
    return TRUE;

  /* The file did not change, so neither does the answer. */
  self->show_unstable_versions = previous;
  return FALSE;
}

guint
nuvashalauncher_settings_get_log_line_limit (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), 0);

  return self->log_line_limit;
}

gboolean
nuvashalauncher_settings_set_log_line_limit (NuvashaLauncherSettings *self,
                                 guint        limit,
                                 GError     **error)
{
  guint previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (limit < LOG_LINE_LIMIT_FLOOR)
    {
      g_set_error (error, NUVASHALAUNCHER_SETTINGS_ERROR, NUVASHALAUNCHER_SETTINGS_ERROR_INVALID,
                   "the log of a running game keeps at least %d line",
                   LOG_LINE_LIMIT_FLOOR);
      return FALSE;
    }

  previous = self->log_line_limit;
  self->log_line_limit = limit;

  if (save_uint (self, LOG_LINE_LIMIT_KEY, limit))
    return TRUE;

  /* The file did not change, so neither does the answer. */
  self->log_line_limit = previous;
  return FALSE;
}

gboolean
nuvashalauncher_settings_get_show_unstable_loaders (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);

  return self->show_unstable_loaders;
}

gboolean
nuvashalauncher_settings_set_show_unstable_loaders (NuvashaLauncherSettings *self,
                                        gboolean     value,
                                        GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->show_unstable_loaders;
  self->show_unstable_loaders = value;

  if (save_boolean (self, SHOW_UNSTABLE_LOADERS_KEY, value))
    return TRUE;

  /* The file did not change, so neither does the answer. */
  self->show_unstable_loaders = previous;
  return FALSE;
}

const char *
nuvashalauncher_settings_get_language (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), "system");

  return self->language != NULL ? self->language : "system";
}

gboolean
nuvashalauncher_settings_set_language (NuvashaLauncherSettings *self,
                           const char *language,
                           GError    **error)
{
  char *previous = NULL;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (language != NULL && *language != '\0', FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->language;
  self->language = g_strdup (language);

  if (save_string (self, LANGUAGE_KEY, self->language))
    {
      g_free (previous);
      return TRUE;
    }

  g_free (self->language);
  self->language = previous;
  return FALSE;
}

gboolean
nuvashalauncher_settings_get_cache_modrinth_api_responses (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);

  return self->cache_modrinth_api_responses;
}

gboolean
nuvashalauncher_settings_set_cache_modrinth_api_responses (NuvashaLauncherSettings *self,
                                               gboolean     value,
                                               GError     **error)
{
  gboolean previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->cache_modrinth_api_responses;
  self->cache_modrinth_api_responses = value;
  if (save_boolean (self, CACHE_MODRINTH_API_RESPONSES_KEY, value))
    return TRUE;

  self->cache_modrinth_api_responses = previous;
  return FALSE;
}

NuvashaLauncherSettingsCacheExpiration
nuvashalauncher_settings_get_cache_expiration (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self),
                        NUVASHALAUNCHER_SETTINGS_DEFAULT_CACHE_EXPIRATION);

  return self->cache_expiration;
}

gboolean
nuvashalauncher_settings_set_cache_expiration (NuvashaLauncherSettings *self,
                                   NuvashaLauncherSettingsCacheExpiration value,
                                   GError **error)
{
  NuvashaLauncherSettingsCacheExpiration previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (value != NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_1_HOUR &&
      value != NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_6_HOURS &&
      value != NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_1_DAY &&
      value != NUVASHALAUNCHER_SETTINGS_CACHE_EXPIRATION_7_DAYS)
    {
      g_set_error (error, NUVASHALAUNCHER_SETTINGS_ERROR, NUVASHALAUNCHER_SETTINGS_ERROR_INVALID,
                   "the cache expiration interval is not supported");
      return FALSE;
    }

  previous = self->cache_expiration;
  self->cache_expiration = value;
  if (save_uint (self, CACHE_EXPIRATION_KEY, value))
    return TRUE;

  self->cache_expiration = previous;
  return FALSE;
}

guint
nuvashalauncher_settings_get_default_memory_mib (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self),
                        NUVASHALAUNCHER_SETTINGS_DEFAULT_MEMORY_MIB);
  return self->default_memory_mib;
}

gboolean
nuvashalauncher_settings_set_default_memory_mib (NuvashaLauncherSettings *self,
                                                 guint value,
                                                 GError **error)
{
  guint previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (value < 512 || value > 1048576)
    {
      g_set_error (error, NUVASHALAUNCHER_SETTINGS_ERROR, NUVASHALAUNCHER_SETTINGS_ERROR_INVALID,
                   "the default RAM value is out of range");
      return FALSE;
    }

  previous = self->default_memory_mib;
  self->default_memory_mib = value;
  if (save_uint (self, DEFAULT_MEMORY_MIB_KEY, value))
    return TRUE;

  self->default_memory_mib = previous;
  return FALSE;
}

const char *
nuvashalauncher_settings_get_default_jvm_args (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), "");
  return self->default_jvm_args != NULL ? self->default_jvm_args : "";
}

gboolean
nuvashalauncher_settings_set_default_jvm_args (NuvashaLauncherSettings *self,
                                               const char *value,
                                               GError **error)
{
  char *previous;
  const char *next = value != NULL ? value : "";

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->default_jvm_args;
  self->default_jvm_args = g_strdup (next);
  if (save_string (self, DEFAULT_JVM_ARGS_KEY, next))
    {
      g_free (previous);
      return TRUE;
    }

  g_free (self->default_jvm_args);
  self->default_jvm_args = previous;
  return FALSE;
}

guint
nuvashalauncher_settings_get_default_game_width (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self),
                        NUVASHALAUNCHER_SETTINGS_DEFAULT_GAME_WIDTH);
  return self->default_game_width;
}

gboolean
nuvashalauncher_settings_set_default_game_width (NuvashaLauncherSettings *self,
                                                 guint value,
                                                 GError **error)
{
  guint previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (value < 320 || value > 7680)
    {
      g_set_error (error, NUVASHALAUNCHER_SETTINGS_ERROR, NUVASHALAUNCHER_SETTINGS_ERROR_INVALID,
                   "the default window width is out of range");
      return FALSE;
    }

  previous = self->default_game_width;
  self->default_game_width = value;
  if (save_uint (self, DEFAULT_GAME_WIDTH_KEY, value))
    return TRUE;

  self->default_game_width = previous;
  return FALSE;
}

guint
nuvashalauncher_settings_get_default_game_height (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self),
                        NUVASHALAUNCHER_SETTINGS_DEFAULT_GAME_HEIGHT);
  return self->default_game_height;
}

gboolean
nuvashalauncher_settings_set_default_game_height (NuvashaLauncherSettings *self,
                                                  guint value,
                                                  GError **error)
{
  guint previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (value < 240 || value > 4320)
    {
      g_set_error (error, NUVASHALAUNCHER_SETTINGS_ERROR, NUVASHALAUNCHER_SETTINGS_ERROR_INVALID,
                   "the default window height is out of range");
      return FALSE;
    }

  previous = self->default_game_height;
  self->default_game_height = value;
  if (save_uint (self, DEFAULT_GAME_HEIGHT_KEY, value))
    return TRUE;

  self->default_game_height = previous;
  return FALSE;
}

gboolean
nuvashalauncher_settings_get_default_game_fullscreen (NuvashaLauncherSettings *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  return self->default_game_fullscreen;
}

gboolean
nuvashalauncher_settings_set_default_game_fullscreen (NuvashaLauncherSettings *self,
                                                      gboolean value,
                                                      GError **error)
{
  gboolean previous;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_SETTINGS (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  previous = self->default_game_fullscreen;
  self->default_game_fullscreen = value;
  if (save_boolean (self, DEFAULT_GAME_FULLSCREEN_KEY, value))
    return TRUE;

  self->default_game_fullscreen = previous;
  return FALSE;
}

static void
nuvashalauncher_settings_dispose (GObject *object)
{
  NuvashaLauncherSettings *self = NUVASHALAUNCHER_SETTINGS (object);

  g_clear_object (&self->desktop_settings);

  G_OBJECT_CLASS (nuvashalauncher_settings_parent_class)->dispose (object);
}

static void
nuvashalauncher_settings_finalize (GObject *object)
{
  NuvashaLauncherSettings *self = NUVASHALAUNCHER_SETTINGS (object);

  g_clear_pointer (&self->language, g_free);
  g_clear_pointer (&self->default_jvm_args, g_free);

  G_OBJECT_CLASS (nuvashalauncher_settings_parent_class)->finalize (object);
}

static void
nuvashalauncher_settings_class_init (NuvashaLauncherSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_settings_dispose;
  object_class->finalize = nuvashalauncher_settings_finalize;
}

static GSettingsSchema *
lookup_nuvashalauncher_schema (GSettingsSchemaSource *source)
{
  if (source == NULL)
    return NULL;
  return g_settings_schema_source_lookup (source, "com.github.NuvashaLauncher.NuvashaLauncher", TRUE);
}

static GSettingsSchema *cached_settings_schema = NULL;
static char *cached_schema_env_dir = NULL;
static gboolean cached_schema_initialized = FALSE;

__attribute__ ((destructor)) static void
clear_cached_settings_schema (void)
{
  g_clear_pointer (&cached_settings_schema, g_settings_schema_unref);
  g_clear_pointer (&cached_schema_env_dir, g_free);
  cached_schema_initialized = FALSE;
}

static GSettingsSchema *
cache_and_return_schema (GSettingsSchema *schema, const char *env_dir)
{
  g_clear_pointer (&cached_settings_schema, g_settings_schema_unref);
  g_free (cached_schema_env_dir);
  cached_schema_env_dir = g_strdup (env_dir);
  cached_settings_schema = g_settings_schema_ref (schema);
  cached_schema_initialized = TRUE;
  return schema;
}

static GSettingsSchema *
find_settings_schema (void)
{
  const char *env_dir = g_getenv ("GSETTINGS_SCHEMA_DIR");

  if (cached_schema_initialized && cached_settings_schema != NULL &&
      g_strcmp0 (env_dir, cached_schema_env_dir) == 0)
    return g_settings_schema_ref (cached_settings_schema);

  GSettingsSchemaSource *default_source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema = lookup_nuvashalauncher_schema (default_source);
  if (schema != NULL)
    return cache_and_return_schema (schema, env_dir);

  if (env_dir != NULL && g_file_test (env_dir, G_FILE_TEST_IS_DIR))
    {
      GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory (env_dir, default_source, FALSE, NULL);
      if (src != NULL)
        {
          schema = lookup_nuvashalauncher_schema (src);
          g_settings_schema_source_unref (src);
          if (schema != NULL)
            return cache_and_return_schema (schema, env_dir);
        }
    }

  const char *candidate_dirs[] = {
    NUVASHALAUNCHER_BUILD_SCHEMA_DIR,
    "build/schemas",
    "../build/schemas",
  };
  for (gsize i = 0; i < G_N_ELEMENTS (candidate_dirs); i++)
    {
      if (candidate_dirs[i] != NULL && g_file_test (candidate_dirs[i], G_FILE_TEST_IS_DIR))
        {
          GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory (candidate_dirs[i], default_source, FALSE, NULL);
          if (src != NULL)
            {
              schema = lookup_nuvashalauncher_schema (src);
              g_settings_schema_source_unref (src);
              if (schema != NULL)
                return cache_and_return_schema (schema, env_dir);
            }
        }
    }

  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  if (exe != NULL)
    {
      g_autofree char *bin_dir = g_path_get_dirname (exe);
      g_autofree char *prefix_dir = g_path_get_dirname (bin_dir);
      g_autofree char *share_dir = g_build_filename (prefix_dir, "share", "glib-2.0", "schemas", NULL);
      if (g_file_test (share_dir, G_FILE_TEST_IS_DIR))
        {
          GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory (share_dir, default_source, FALSE, NULL);
          if (src != NULL)
            {
              schema = lookup_nuvashalauncher_schema (src);
              g_settings_schema_source_unref (src);
              if (schema != NULL)
                return cache_and_return_schema (schema, env_dir);
            }
        }
    }

  const char *xml_dirs[] = {
    NUVASHALAUNCHER_SOURCE_DATA_DIR,
    "data",
    "../data",
  };
  for (gsize i = 0; i < G_N_ELEMENTS (xml_dirs); i++)
    {
      if (xml_dirs[i] == NULL)
        continue;
      g_autofree char *xml_file = g_build_filename (xml_dirs[i], "com.github.NuvashaLauncher.NuvashaLauncher.gschema.xml", NULL);
      if (g_file_test (xml_file, G_FILE_TEST_EXISTS))
        {
          g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "nuvashalauncher", "schemas", NULL);
          g_mkdir_with_parents (cache_dir, 0700);
          g_autofree char *cached_xml = g_build_filename (cache_dir, "com.github.NuvashaLauncher.NuvashaLauncher.gschema.xml", NULL);
          g_autofree char *contents = NULL;
          gsize len = 0;
          if (g_file_get_contents (xml_file, &contents, &len, NULL) &&
              g_file_set_contents (cached_xml, contents, len, NULL))
            {
              const char *argv[] = { "glib-compile-schemas", cache_dir, NULL };
              int wait_status = 0;
              if (g_spawn_sync (NULL, (char **) argv, NULL,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, NULL, NULL, &wait_status, NULL) &&
                  g_spawn_check_wait_status (wait_status, NULL))
                {
                  GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory (cache_dir, default_source, FALSE, NULL);
                  if (src != NULL)
                    {
                      schema = lookup_nuvashalauncher_schema (src);
                      g_settings_schema_source_unref (src);
                      if (schema != NULL)
                        return cache_and_return_schema (schema, env_dir);
                    }
                }
            }
        }
    }

  return NULL;
}

GSettings *
nuvashalauncher_settings_open_gsettings (void)
{
  GSettingsSchema *schema = find_settings_schema ();
  GSettings *settings = NULL;

  if (schema == NULL)
    return NULL;

  settings = g_settings_new_full (schema, NULL, NULL);
  g_settings_schema_unref (schema);
  return settings;
}

static void
nuvashalauncher_settings_init (NuvashaLauncherSettings *self)
{
  self->desktop_settings = nuvashalauncher_settings_open_gsettings ();

  self->show_unstable_versions = FALSE;
  self->log_line_limit = NUVASHALAUNCHER_SETTINGS_DEFAULT_LOG_LINE_LIMIT;
  self->show_unstable_loaders = FALSE;
  self->language = g_strdup ("");
  self->cache_modrinth_api_responses = TRUE;
  self->cache_expiration = NUVASHALAUNCHER_SETTINGS_DEFAULT_CACHE_EXPIRATION;
  self->export_metadata = FALSE;
  self->delete_dependents = TRUE;
  self->default_memory_mib = NUVASHALAUNCHER_SETTINGS_DEFAULT_MEMORY_MIB;
  self->default_jvm_args = g_strdup ("");
  self->default_game_width = NUVASHALAUNCHER_SETTINGS_DEFAULT_GAME_WIDTH;
  self->default_game_height = NUVASHALAUNCHER_SETTINGS_DEFAULT_GAME_HEIGHT;
  self->default_game_fullscreen = FALSE;

  if (self->desktop_settings != NULL)
    load (self);
}
