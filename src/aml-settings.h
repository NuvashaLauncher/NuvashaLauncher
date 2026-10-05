/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlSettings: the small file of choices the whole application shares,
 * <config dir>/AML/config.json.
 *
 * One object, one option so far: whether the version lists also show
 * snapshots and the versions Mojang no longer serves. The file is read
 * once when the object is created and written on every change; a file
 * that is missing, unreadable or nonsense leaves the defaults in
 * place instead of failing. */

#ifndef AML_SETTINGS_H
#define AML_SETTINGS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define AML_TYPE_SETTINGS (aml_settings_get_type ())

G_DECLARE_FINAL_TYPE (AmlSettings, aml_settings, AML, SETTINGS, GObject)

#define AML_SETTINGS_ERROR (aml_settings_error_quark ())

typedef enum {
  AML_SETTINGS_ERROR_IO /* the settings file could not be written */
} AmlSettingsError;

GQuark aml_settings_error_quark (void);

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

G_END_DECLS

#endif /* AML_SETTINGS_H */
