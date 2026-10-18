/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaSettings: the small file of choices the whole application shares,
 * <config dir>/leppoisa/config.json.
 *
 * One object, eleven options so far: whether the version lists also show
 * snapshots and the versions Mojang no longer serves, whether the loader
 * lists also show the versions a loader does not call stable, how many
 * lines the log of a running game keeps, whether the log folder is opened
 * when a game crashes, and whether the game sandbox is enabled. Which Java
 * an instance runs with is not
 * one of them: it is the automatic choice, unless the instance itself
 * names another runtime. The file is read once when the object is
 * created and written on every change; a file that is missing, unreadable
 * or nonsense leaves the defaults in place instead of failing. */

#ifndef leppoisa_SETTINGS_H
#define leppoisa_SETTINGS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define leppoisa_TYPE_SETTINGS (leppoisa_settings_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaSettings, leppoisa_settings, leppoisa, SETTINGS, GObject)

#define leppoisa_SETTINGS_ERROR (leppoisa_settings_error_quark ())

typedef enum {
  leppoisa_SETTINGS_ERROR_IO,      /* the settings file could not be written */
  leppoisa_SETTINGS_ERROR_INVALID  /* the value asked for is not a usable one */
} LeppoisaSettingsError;

typedef enum {
  leppoisa_SETTINGS_CACHE_EXPIRATION_1_HOUR = 60 * 60,
  leppoisa_SETTINGS_CACHE_EXPIRATION_6_HOURS = 6 * 60 * 60,
  leppoisa_SETTINGS_CACHE_EXPIRATION_1_DAY = 24 * 60 * 60,
  leppoisa_SETTINGS_CACHE_EXPIRATION_7_DAYS = 7 * 24 * 60 * 60
} LeppoisaSettingsCacheExpiration;

GQuark leppoisa_settings_error_quark (void);

/* How many lines the log of a running game keeps before the oldest ones
 * are dropped. */
#define leppoisa_SETTINGS_DEFAULT_LOG_LINE_LIMIT 5000
#define leppoisa_SETTINGS_DEFAULT_CACHE_EXPIRATION \
  leppoisa_SETTINGS_CACHE_EXPIRATION_1_HOUR

/* Reads <config dir>/leppoisa/config.json, falling back to the defaults
 * when there is nothing there to read. Never returns NULL. */
LeppoisaSettings *leppoisa_settings_new (void);

/* FALSE unless the file says otherwise. */
gboolean leppoisa_settings_get_show_snapshots (LeppoisaSettings *self);

/* Stores @value and writes the file. On failure the old value stays
 * in place, so the object never disagrees with what is on disk, and
 * FALSE comes back with the reason. */
gboolean leppoisa_settings_set_show_snapshots (LeppoisaSettings *self,
                                          gboolean     value,
                                          GError     **error);

/* How many lines the log of a running game keeps, oldest dropped first.
 * Never below one: leppoisa_SETTINGS_DEFAULT_LOG_LINE_LIMIT when the file
 * says nothing usable. */
guint leppoisa_settings_get_log_line_limit (LeppoisaSettings *self);

/* @limit has to be at least one or the call fails with
 * leppoisa_SETTINGS_ERROR_INVALID and the stored value stays in place. */
gboolean leppoisa_settings_set_log_line_limit (LeppoisaSettings *self,
                                          guint        limit,
                                          GError     **error);

/* FALSE unless the file says otherwise: a crash does not open a folder
 * by itself, the user asks for the log folder when they want it. */
gboolean leppoisa_settings_get_open_logs_after_crash (LeppoisaSettings *self);

gboolean leppoisa_settings_set_open_logs_after_crash (LeppoisaSettings *self,
                                                 gboolean     value,
                                                 GError     **error);

/* FALSE unless the file says otherwise, so a loader list holds the
 * versions that loader calls stable and nothing else. */
gboolean leppoisa_settings_get_show_unstable_loaders (LeppoisaSettings *self);

gboolean leppoisa_settings_set_show_unstable_loaders (LeppoisaSettings *self,
                                                 gboolean     value,
                                                 GError     **error);

const char *leppoisa_settings_get_language (LeppoisaSettings *self);
gboolean leppoisa_settings_set_language (LeppoisaSettings *self,
                                    const char *language,
                                    GError    **error);

gboolean leppoisa_settings_get_cache_modrinth_api_responses (LeppoisaSettings *self);
gboolean leppoisa_settings_set_cache_modrinth_api_responses (LeppoisaSettings *self,
                                                        gboolean     value,
                                                        GError     **error);

LeppoisaSettingsCacheExpiration
leppoisa_settings_get_cache_expiration (LeppoisaSettings *self);
gboolean leppoisa_settings_set_cache_expiration (LeppoisaSettings *self,
                                            LeppoisaSettingsCacheExpiration value,
                                            GError **error);

/* TRUE asks for pack metadata before an export; FALSE performs a direct
 * export using the instance name and no optional metadata. */
gboolean leppoisa_settings_get_export_metadata (LeppoisaSettings *self);
gboolean leppoisa_settings_set_export_metadata (LeppoisaSettings *self,
                                           gboolean value,
                                           GError **error);

/* FALSE by default: deleting a dependency does not delete its dependents. */
gboolean leppoisa_settings_get_delete_dependents (LeppoisaSettings *self);
gboolean leppoisa_settings_set_delete_dependents (LeppoisaSettings *self,
                                              gboolean value,
                                              GError **error);

/* FALSE by default: the game is started directly unless the user opts into
 * the bubblewrap sandbox. */
gboolean leppoisa_settings_get_sandbox_enabled (LeppoisaSettings *self);
gboolean leppoisa_settings_set_sandbox_enabled (LeppoisaSettings *self,
                                           gboolean value,
                                           GError **error);

/* TRUE by default: the sandbox keeps the game's network access unless the
 * user explicitly disables it. */
gboolean leppoisa_settings_get_sandbox_network_enabled (LeppoisaSettings *self);
gboolean leppoisa_settings_set_sandbox_network_enabled (LeppoisaSettings *self,
                                                   gboolean value,
                                                   GError **error);

G_END_DECLS

#endif /* leppoisa_SETTINGS_H */
