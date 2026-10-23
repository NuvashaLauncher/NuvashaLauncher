/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaSettings: the desktop preferences shared by the application.
 *
 * One object, eleven options so far: whether the version lists also show
 * snapshots and the versions Mojang no longer serves, whether the loader
 * lists also show the versions a loader does not call stable, how many
 * lines the log of a running game keeps, whether the log folder is opened
 * when a game crashes, and whether the game sandbox is enabled. Which Java
 * an instance runs with is not
 * one of them: it is the automatic choice, unless the instance itself
 * names another runtime. The file is read once when the object is
 * created and written on every change through GSettings. */

#ifndef nuvasha_SETTINGS_H
#define nuvasha_SETTINGS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define nuvasha_TYPE_SETTINGS (nuvasha_settings_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaSettings, nuvasha_settings, nuvasha, SETTINGS, GObject)

#define nuvasha_SETTINGS_ERROR (nuvasha_settings_error_quark ())

typedef enum {
  nuvasha_SETTINGS_ERROR_IO,      /* the settings file could not be written */
  nuvasha_SETTINGS_ERROR_INVALID  /* the value asked for is not a usable one */
} NuvashaSettingsError;

typedef enum {
  nuvasha_SETTINGS_CACHE_EXPIRATION_1_HOUR = 60 * 60,
  nuvasha_SETTINGS_CACHE_EXPIRATION_6_HOURS = 6 * 60 * 60,
  nuvasha_SETTINGS_CACHE_EXPIRATION_1_DAY = 24 * 60 * 60,
  nuvasha_SETTINGS_CACHE_EXPIRATION_7_DAYS = 7 * 24 * 60 * 60
} NuvashaSettingsCacheExpiration;

GQuark nuvasha_settings_error_quark (void);

/* How many lines the log of a running game keeps before the oldest ones
 * are dropped. */
#define nuvasha_SETTINGS_DEFAULT_LOG_LINE_LIMIT 5000
#define nuvasha_SETTINGS_DEFAULT_CACHE_EXPIRATION \
  nuvasha_SETTINGS_CACHE_EXPIRATION_1_HOUR

/* Reads the installed GSettings schema and defaults. Never returns NULL. */
NuvashaSettings *nuvasha_settings_new (void);

/* Whether snapshot versions are shown. */
gboolean nuvasha_settings_get_show_snapshots (NuvashaSettings *self);

/* Stores @value and writes the file. On failure the old value stays
 * in place, so the object never disagrees with what is on disk, and
 * FALSE comes back with the reason. */
gboolean nuvasha_settings_set_show_snapshots (NuvashaSettings *self,
                                          gboolean     value,
                                          GError     **error);

/* How many lines the log of a running game keeps, oldest dropped first.
 * Never below one: nuvasha_SETTINGS_DEFAULT_LOG_LINE_LIMIT when the file
 * says nothing usable. */
guint nuvasha_settings_get_log_line_limit (NuvashaSettings *self);

/* @limit has to be at least one or the call fails with
 * nuvasha_SETTINGS_ERROR_INVALID and the stored value stays in place. */
gboolean nuvasha_settings_set_log_line_limit (NuvashaSettings *self,
                                          guint        limit,
                                          GError     **error);

/* Whether the log folder opens after a crash. */
gboolean nuvasha_settings_get_open_logs_after_crash (NuvashaSettings *self);

gboolean nuvasha_settings_set_open_logs_after_crash (NuvashaSettings *self,
                                                 gboolean     value,
                                                 GError     **error);

/* Whether unstable loader versions are shown. */
gboolean nuvasha_settings_get_show_unstable_loaders (NuvashaSettings *self);

gboolean nuvasha_settings_set_show_unstable_loaders (NuvashaSettings *self,
                                                 gboolean     value,
                                                 GError     **error);

const char *nuvasha_settings_get_language (NuvashaSettings *self);
gboolean nuvasha_settings_set_language (NuvashaSettings *self,
                                    const char *language,
                                    GError    **error);

gboolean nuvasha_settings_get_cache_modrinth_api_responses (NuvashaSettings *self);
gboolean nuvasha_settings_set_cache_modrinth_api_responses (NuvashaSettings *self,
                                                        gboolean     value,
                                                        GError     **error);

NuvashaSettingsCacheExpiration
nuvasha_settings_get_cache_expiration (NuvashaSettings *self);
gboolean nuvasha_settings_set_cache_expiration (NuvashaSettings *self,
                                            NuvashaSettingsCacheExpiration value,
                                            GError **error);

/* TRUE asks for pack metadata before an export; FALSE performs a direct
 * export using the instance name and no optional metadata. */
gboolean nuvasha_settings_get_export_metadata (NuvashaSettings *self);
gboolean nuvasha_settings_set_export_metadata (NuvashaSettings *self,
                                           gboolean value,
                                           GError **error);

/* FALSE by default: deleting a dependency does not delete its dependents. */
gboolean nuvasha_settings_get_delete_dependents (NuvashaSettings *self);
gboolean nuvasha_settings_set_delete_dependents (NuvashaSettings *self,
                                              gboolean value,
                                              GError **error);

/* FALSE by default: the game is started directly unless the user opts into
 * the bubblewrap sandbox. */
gboolean nuvasha_settings_get_sandbox_enabled (NuvashaSettings *self);
gboolean nuvasha_settings_set_sandbox_enabled (NuvashaSettings *self,
                                           gboolean value,
                                           GError **error);

/* TRUE by default: the sandbox keeps the game's network access unless the
 * user explicitly disables it. */
gboolean nuvasha_settings_get_sandbox_network_enabled (NuvashaSettings *self);
gboolean nuvasha_settings_set_sandbox_network_enabled (NuvashaSettings *self,
                                                   gboolean value,
                                                   GError **error);

G_END_DECLS

#endif /* nuvasha_SETTINGS_H */
