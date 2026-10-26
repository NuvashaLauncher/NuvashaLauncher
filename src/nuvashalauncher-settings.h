/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherSettings: the desktop preferences shared by the application.
 *
 * One object, ten options so far: whether the version lists also show
 * unstable Minecraft versions, whether the loader lists also show the
 * versions a loader does not call stable, how many lines the log of a running
 * game keeps. Which Java
 * an instance runs with is not
 * one of them: it is the automatic choice, unless the instance itself
 * names another runtime. The file is read once when the object is
 * created and written on every change through GSettings. */

#ifndef nuvashalauncher_SETTINGS_H
#define nuvashalauncher_SETTINGS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_SETTINGS (nuvashalauncher_settings_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherSettings, nuvashalauncher_settings, nuvashalauncher, SETTINGS, GObject)

#define nuvashalauncher_SETTINGS_ERROR (nuvashalauncher_settings_error_quark ())

typedef enum {
  nuvashalauncher_SETTINGS_ERROR_IO,      /* the settings file could not be written */
  nuvashalauncher_SETTINGS_ERROR_INVALID  /* the value asked for is not a usable one */
} nuvashalauncherSettingsError;

typedef enum {
  nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_HOUR = 60 * 60,
  nuvashalauncher_SETTINGS_CACHE_EXPIRATION_6_HOURS = 6 * 60 * 60,
  nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_DAY = 24 * 60 * 60,
  nuvashalauncher_SETTINGS_CACHE_EXPIRATION_7_DAYS = 7 * 24 * 60 * 60
} nuvashalauncherSettingsCacheExpiration;

GQuark nuvashalauncher_settings_error_quark (void);

/* How many lines the log of a running game keeps before the oldest ones
 * are dropped. */
#define nuvashalauncher_SETTINGS_DEFAULT_LOG_LINE_LIMIT 5000
#define nuvashalauncher_SETTINGS_DEFAULT_CACHE_EXPIRATION \
  nuvashalauncher_SETTINGS_CACHE_EXPIRATION_1_HOUR

/* Reads the installed GSettings schema and defaults. Never returns NULL. */
nuvashalauncherSettings *nuvashalauncher_settings_new (void);

/* Whether unstable Minecraft versions are shown. */
gboolean nuvashalauncher_settings_get_show_unstable_versions (nuvashalauncherSettings *self);

/* Stores @value and writes the file. On failure the old value stays
 * in place, so the object never disagrees with what is on disk, and
 * FALSE comes back with the reason. */
gboolean nuvashalauncher_settings_set_show_unstable_versions (nuvashalauncherSettings *self,
                                                   gboolean     value,
                                                   GError     **error);

/* How many lines the log of a running game keeps, oldest dropped first.
 * Never below one: nuvashalauncher_SETTINGS_DEFAULT_LOG_LINE_LIMIT when the file
 * says nothing usable. */
guint nuvashalauncher_settings_get_log_line_limit (nuvashalauncherSettings *self);

/* @limit has to be at least one or the call fails with
 * nuvashalauncher_SETTINGS_ERROR_INVALID and the stored value stays in place. */
gboolean nuvashalauncher_settings_set_log_line_limit (nuvashalauncherSettings *self,
                                          guint        limit,
                                          GError     **error);

/* Whether unstable loader versions are shown. */
gboolean nuvashalauncher_settings_get_show_unstable_loaders (nuvashalauncherSettings *self);

gboolean nuvashalauncher_settings_set_show_unstable_loaders (nuvashalauncherSettings *self,
                                                 gboolean     value,
                                                 GError     **error);

const char *nuvashalauncher_settings_get_language (nuvashalauncherSettings *self);
gboolean nuvashalauncher_settings_set_language (nuvashalauncherSettings *self,
                                    const char *language,
                                    GError    **error);

gboolean nuvashalauncher_settings_get_cache_modrinth_api_responses (nuvashalauncherSettings *self);
gboolean nuvashalauncher_settings_set_cache_modrinth_api_responses (nuvashalauncherSettings *self,
                                                        gboolean     value,
                                                        GError     **error);

nuvashalauncherSettingsCacheExpiration
nuvashalauncher_settings_get_cache_expiration (nuvashalauncherSettings *self);
gboolean nuvashalauncher_settings_set_cache_expiration (nuvashalauncherSettings *self,
                                            nuvashalauncherSettingsCacheExpiration value,
                                            GError **error);

/* TRUE asks for pack metadata before an export; FALSE performs a direct
 * export using the instance name and no optional metadata. */
gboolean nuvashalauncher_settings_get_export_metadata (nuvashalauncherSettings *self);
gboolean nuvashalauncher_settings_set_export_metadata (nuvashalauncherSettings *self,
                                           gboolean value,
                                           GError **error);

/* TRUE by default: deleting a dependency also deletes its dependents. */
gboolean nuvashalauncher_settings_get_delete_dependents (nuvashalauncherSettings *self);
gboolean nuvashalauncher_settings_set_delete_dependents (nuvashalauncherSettings *self,
                                              gboolean value,
                                              GError **error);

G_END_DECLS

#endif /* nuvashalauncher_SETTINGS_H */
