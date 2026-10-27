/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherInstance: one launcher instance, a Minecraft installation with its
 * own directory, game version, mod loader and Java settings.
 *
 * Plain data object with JSON serialization: no widgets and no file or
 * network I/O live here. */

#ifndef nuvashalauncher_INSTANCE_H
#define nuvashalauncher_INSTANCE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Mod loaders nuvashalauncher knows about. In JSON the loader is the lowercase
 * string, e.g. "fabric". */
typedef enum {
  nuvashalauncher_LOADER_VANILLA,
  nuvashalauncher_LOADER_FABRIC,
  nuvashalauncher_LOADER_FORGE,
  nuvashalauncher_LOADER_NEOFORGE,
  nuvashalauncher_LOADER_QUILT,
} nuvashalauncherLoader;

#define nuvashalauncher_TYPE_INSTANCE (nuvashalauncher_instance_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherInstance, nuvashalauncher_instance, nuvashalauncher, INSTANCE, GObject)

#define nuvashalauncher_INSTANCE_ERROR (nuvashalauncher_instance_error_quark ())

typedef enum {
  nuvashalauncher_INSTANCE_ERROR_INVALID,      /* a field holds a bad value */
  nuvashalauncher_INSTANCE_ERROR_MISSING_FIELD /* a required field is absent */
} nuvashalauncherInstanceError;

GQuark nuvashalauncher_instance_error_quark (void);

GType nuvashalauncher_loader_get_type (void);

/* String form of @loader as written to JSON, or NULL if @loader is not
 * a known value. */
const char *nuvashalauncher_loader_to_string (nuvashalauncherLoader loader);

/* Parse the JSON form of a loader. Returns FALSE for unknown values. */
gboolean nuvashalauncher_loader_from_string (const char *string, nuvashalauncherLoader *loader);

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
nuvashalauncherInstance *nuvashalauncher_instance_new (const char *id,
                               const char *name,
                               const char *directory,
                               const char *minecraft_version,
                               nuvashalauncherLoader   loader,
                               GError    **error);

/* Reads one instance from a JSON object. Malformed or incomplete input
 * fails with a GError instead of producing a half built instance. */
nuvashalauncherInstance *nuvashalauncher_instance_new_from_json (JsonNode *node, GError **error);

/* (transfer full). The instance must be valid, see
 * nuvashalauncher_instance_validate(). */
JsonNode *nuvashalauncher_instance_to_json (nuvashalauncherInstance *self);

/* TRUE when every field of @self holds a usable value. */
gboolean nuvashalauncher_instance_validate (nuvashalauncherInstance *self, GError **error);

/* Model-level mutation helpers. Callers should use these instead of writing
 * instance properties directly; validation is performed before the value is
 * accepted. */
gboolean nuvashalauncher_instance_set_string (nuvashalauncherInstance *self, const char *property,
                                  const char *value, GError **error);
gboolean nuvashalauncher_instance_set_loader (nuvashalauncherInstance *self, nuvashalauncherLoader loader,
                                  const char *version, GError **error);
gboolean nuvashalauncher_instance_set_name (nuvashalauncherInstance *self, const char *name,
                                GError **error);
gboolean nuvashalauncher_instance_set_directory (nuvashalauncherInstance *self, const char *directory,
                                     GError **error);

/* The unique id, never NULL. */
const char *nuvashalauncher_instance_get_id (nuvashalauncherInstance *self);

/* Borrowed component map keyed by stable component id. Property access and
 * legacy JSON remain compatibility views over this map. */
GHashTable *nuvashalauncher_instance_get_components (nuvashalauncherInstance *self);

G_END_DECLS

#endif /* nuvashalauncher_INSTANCE_H */
