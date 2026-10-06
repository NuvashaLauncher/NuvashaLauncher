/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlSettings: the small file of choices the whole application shares,
 * <config dir>/AML/config.json.
 *
 * One object, four options so far: whether the version lists also show
 * snapshots and the versions Mojang no longer serves, which Java an
 * instance runs with when it picks none, how many lines the log of a
 * running game keeps and whether the log folder is opened when a game
 * crashes. The file is read once when the object is created and written
 * on every change; a file that is missing, unreadable or nonsense leaves
 * the defaults in place instead of failing. */

#ifndef AML_SETTINGS_H
#define AML_SETTINGS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define AML_TYPE_SETTINGS (aml_settings_get_type ())

G_DECLARE_FINAL_TYPE (AmlSettings, aml_settings, AML, SETTINGS, GObject)

#define AML_SETTINGS_ERROR (aml_settings_error_quark ())

typedef enum {
  AML_SETTINGS_ERROR_IO,      /* the settings file could not be written */
  AML_SETTINGS_ERROR_INVALID  /* the value asked for is not a usable one */
} AmlSettingsError;

GQuark aml_settings_error_quark (void);

/* How many lines the log of a running game keeps before the oldest ones
 * are dropped. */
#define AML_SETTINGS_DEFAULT_LOG_LINE_LIMIT 5000

/* Reads <config dir>/AML/config.json, falling back to the defaults
 * when there is nothing there to read. Never returns NULL. */
AmlSettings *aml_settings_new (void);

/* FALSE unless the file says otherwise. */
gboolean aml_settings_get_show_snapshots (AmlSettings *self);

/* Stores @value and writes the file. On failure the old value stays
 * in place, so the object never disagrees with what is on disk, and
 * FALSE comes back with the reason. */
gboolean aml_settings_set_show_snapshots (AmlSettings *self,
                                          gboolean     value,
                                          GError     **error);

/* The global default Java executable, or NULL for "the one that fits
 * each instance best". Borrowed from the object, NULL when unset. */
const char *aml_settings_get_default_java_path (AmlSettings *self);

/* Same write-through contract as the snapshots setter: the object only
 * ever agrees with the file. */
gboolean aml_settings_set_default_java_path (AmlSettings *self,
                                             const char *path,
                                             GError    **error);

/* How many lines the log of a running game keeps, oldest dropped first.
 * Never below one: AML_SETTINGS_DEFAULT_LOG_LINE_LIMIT when the file
 * says nothing usable. */
guint aml_settings_get_log_line_limit (AmlSettings *self);

/* @limit has to be at least one or the call fails with
 * AML_SETTINGS_ERROR_INVALID and the stored value stays in place. */
gboolean aml_settings_set_log_line_limit (AmlSettings *self,
                                          guint        limit,
                                          GError     **error);

/* FALSE unless the file says otherwise: a crash does not open a folder
 * by itself, the user asks for the log folder when they want it. */
gboolean aml_settings_get_open_logs_after_crash (AmlSettings *self);

gboolean aml_settings_set_open_logs_after_crash (AmlSettings *self,
                                                 gboolean     value,
                                                 GError     **error);

G_END_DECLS

#endif /* AML_SETTINGS_H */
