/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherInstance: one launcher instance, a Minecraft installation with its
 * own directory, game version, mod loader and Java settings.
 *
 * Plain data object with JSON serialization: no widgets and no file or
 * network I/O live here. */

#ifndef NUVASHALAUNCHER_INSTANCE_H
#define NUVASHALAUNCHER_INSTANCE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Mod loaders nuvashalauncher knows about. In JSON the loader is the lowercase
 * string, e.g. "fabric". */
typedef enum {
  NUVASHALAUNCHER_LOADER_VANILLA,
  NUVASHALAUNCHER_LOADER_FABRIC,
  NUVASHALAUNCHER_LOADER_FORGE,
  NUVASHALAUNCHER_LOADER_NEOFORGE,
  NUVASHALAUNCHER_LOADER_QUILT,
} NuvashaLauncherLoader;

#define NUVASHALAUNCHER_TYPE_INSTANCE (nuvashalauncher_instance_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLauncherInstance, nuvashalauncher_instance, NUVASHALAUNCHER, INSTANCE, GObject)

#define NUVASHALAUNCHER_INSTANCE_ERROR (nuvashalauncher_instance_error_quark ())

typedef enum {
  NUVASHALAUNCHER_INSTANCE_ERROR_INVALID,      /* a field holds a bad value */
  NUVASHALAUNCHER_INSTANCE_ERROR_MISSING_FIELD /* a required field is absent */
} NuvashaLauncherInstanceError;

GQuark nuvashalauncher_instance_error_quark (void);

GType nuvashalauncher_loader_get_type (void);

/* String form of @loader as written to JSON, or NULL if @loader is not
 * a known value. */
const char *nuvashalauncher_loader_to_string (NuvashaLauncherLoader loader);

/* Parse the JSON form of a loader. Returns FALSE for unknown values. */
gboolean nuvashalauncher_loader_from_string (const char *string, NuvashaLauncherLoader *loader);

/* A name that is safe to use as a directory component: no slashes, no
 * "..", no leading or trailing dots or spaces, never empty. Never
 * returns NULL or an empty string. */
char *nuvashalauncher_instance_sanitize_name (const char *name);

/* Return the packaged default instance icon path, or NULL when it is not
 * available. The returned path is owned by the caller. */
char *nuvashalauncher_instance_default_icon_path (void);

/* (transfer full) NFC-normalized display name with Unicode surrounding
 * whitespace removed. Control characters remain in the result so validation
 * can reject them instead of changing the user's input. */
char *nuvashalauncher_instance_normalize_name (const char *name);

/* Creates an instance and validates every field. @id may be NULL to
 * generate a random UUID. @directory must be an absolute path and is
 * canonicalized; it must exist or be creatable by the caller. */
NuvashaLauncherInstance *nuvashalauncher_instance_new (const char *id,
                               const char *name,
                               const char *directory,
                               const char *minecraft_version,
                               NuvashaLauncherLoader   loader,
                               GError    **error);

/* Reads one instance from a JSON object. Malformed or incomplete input
 * fails with a GError instead of producing a half built instance. */
NuvashaLauncherInstance *nuvashalauncher_instance_new_from_json (JsonNode *node, GError **error);

/* (transfer full). The instance must be valid, see
 * nuvashalauncher_instance_validate(). */
JsonNode *nuvashalauncher_instance_to_json (NuvashaLauncherInstance *self);

/* TRUE when every field of @self holds a usable value. */
gboolean nuvashalauncher_instance_validate (NuvashaLauncherInstance *self, GError **error);

/* Model-level mutation helpers. Callers should use these instead of writing
 * instance properties directly; validation is performed before the value is
 * accepted. */
gboolean nuvashalauncher_instance_set_string (NuvashaLauncherInstance *self, const char *property,
                                  const char *value, GError **error);
gboolean nuvashalauncher_instance_set_loader (NuvashaLauncherInstance *self, NuvashaLauncherLoader loader,
                                  const char *version, GError **error);
gboolean nuvashalauncher_instance_set_name (NuvashaLauncherInstance *self, const char *name,
                                GError **error);
gboolean nuvashalauncher_instance_set_directory (NuvashaLauncherInstance *self, const char *directory,
                                     GError **error);

/* The unique id, never NULL. */
const char *nuvashalauncher_instance_get_id (NuvashaLauncherInstance *self);

/* Borrowed component map keyed by stable component id. Property access and
 * legacy JSON remain compatibility views over this map. */
GHashTable *nuvashalauncher_instance_get_components (NuvashaLauncherInstance *self);

G_END_DECLS

#endif /* NUVASHALAUNCHER_INSTANCE_H */
