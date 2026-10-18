/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaInstance: one launcher instance, a Minecraft installation with its
 * own directory, game version, mod loader and Java settings.
 *
 * Plain data object with JSON serialization: no widgets and no file or
 * network I/O live here. */

#ifndef nuvasha_INSTANCE_H
#define nuvasha_INSTANCE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Mod loaders nuvasha knows about. In JSON the loader is the lowercase
 * string, e.g. "fabric". */
typedef enum {
  nuvasha_LOADER_VANILLA,
  nuvasha_LOADER_FABRIC,
  nuvasha_LOADER_FORGE,
  nuvasha_LOADER_NEOFORGE,
  nuvasha_LOADER_QUILT,
} NuvashaLoader;

#define nuvasha_TYPE_INSTANCE (nuvasha_instance_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaInstance, nuvasha_instance, nuvasha, INSTANCE, GObject)

#define nuvasha_INSTANCE_ERROR (nuvasha_instance_error_quark ())

typedef enum {
  nuvasha_INSTANCE_ERROR_INVALID,      /* a field holds a bad value */
  nuvasha_INSTANCE_ERROR_MISSING_FIELD /* a required field is absent */
} NuvashaInstanceError;

GQuark nuvasha_instance_error_quark (void);

GType nuvasha_loader_get_type (void);

/* String form of @loader as written to JSON, or NULL if @loader is not
 * a known value. */
const char *nuvasha_loader_to_string (NuvashaLoader loader);

/* Parse the JSON form of a loader. Returns FALSE for unknown values. */
gboolean nuvasha_loader_from_string (const char *string, NuvashaLoader *loader);

/* A name that is safe to use as a directory component: no slashes, no
 * "..", no leading or trailing dots or spaces, never empty. Never
 * returns NULL or an empty string. */
char *nuvasha_instance_sanitize_name (const char *name);

/* Return the packaged default instance icon path, or NULL when it is not
 * available. The returned path is owned by the caller. */
char *nuvasha_instance_default_icon_path (void);

/* (transfer full) NFC-normalized display name with Unicode surrounding
 * whitespace removed. Control characters remain in the result so validation
 * can reject them rather than silently changing what the user entered. */
char *nuvasha_instance_normalize_name (const char *name);

/* Creates an instance and validates every field. @id may be NULL to
 * generate a random UUID. @directory must be an absolute path and is
 * canonicalized; it must exist or be creatable by the caller. */
NuvashaInstance *nuvasha_instance_new (const char *id,
                               const char *name,
                               const char *directory,
                               const char *minecraft_version,
                               NuvashaLoader   loader,
                               GError    **error);

/* Reads one instance from a JSON object. Malformed or incomplete input
 * fails with a GError instead of producing a half built instance. */
NuvashaInstance *nuvasha_instance_new_from_json (JsonNode *node, GError **error);

/* (transfer full). The instance must be valid, see
 * nuvasha_instance_validate(). */
JsonNode *nuvasha_instance_to_json (NuvashaInstance *self);

/* TRUE when every field of @self holds a usable value. */
gboolean nuvasha_instance_validate (NuvashaInstance *self, GError **error);

/* Model-level mutation helpers. Callers should use these instead of writing
 * instance properties directly; validation is performed before the value is
 * accepted. */
gboolean nuvasha_instance_set_string (NuvashaInstance *self, const char *property,
                                  const char *value, GError **error);
gboolean nuvasha_instance_set_loader (NuvashaInstance *self, NuvashaLoader loader,
                                  const char *version, GError **error);
gboolean nuvasha_instance_set_name (NuvashaInstance *self, const char *name,
                                GError **error);
gboolean nuvasha_instance_set_directory (NuvashaInstance *self, const char *directory,
                                     GError **error);

/* The unique id, never NULL. */
const char *nuvasha_instance_get_id (NuvashaInstance *self);

/* Borrowed component map keyed by stable component id. Property access and
 * legacy JSON remain compatibility views over this map. */
GHashTable *nuvasha_instance_get_components (NuvashaInstance *self);

G_END_DECLS

#endif /* nuvasha_INSTANCE_H */
