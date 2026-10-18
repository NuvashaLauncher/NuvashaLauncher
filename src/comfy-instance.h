/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyInstance: one launcher instance, a Minecraft installation with its
 * own directory, game version, mod loader and Java settings.
 *
 * Plain data object with JSON serialization: no widgets and no file or
 * network I/O live here. */

#ifndef comfy_INSTANCE_H
#define comfy_INSTANCE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Mod loaders comfy knows about. In JSON the loader is the lowercase
 * string, e.g. "fabric". */
typedef enum {
  comfy_LOADER_VANILLA,
  comfy_LOADER_FABRIC,
  comfy_LOADER_FORGE,
  comfy_LOADER_NEOFORGE,
  comfy_LOADER_QUILT,
} ComfyLoader;

#define comfy_TYPE_INSTANCE (comfy_instance_get_type ())

G_DECLARE_FINAL_TYPE (ComfyInstance, comfy_instance, comfy, INSTANCE, GObject)

#define comfy_INSTANCE_ERROR (comfy_instance_error_quark ())

typedef enum {
  comfy_INSTANCE_ERROR_INVALID,      /* a field holds a bad value */
  comfy_INSTANCE_ERROR_MISSING_FIELD /* a required field is absent */
} ComfyInstanceError;

GQuark comfy_instance_error_quark (void);

GType comfy_loader_get_type (void);

/* String form of @loader as written to JSON, or NULL if @loader is not
 * a known value. */
const char *comfy_loader_to_string (ComfyLoader loader);

/* Parse the JSON form of a loader. Returns FALSE for unknown values. */
gboolean comfy_loader_from_string (const char *string, ComfyLoader *loader);

/* A name that is safe to use as a directory component: no slashes, no
 * "..", no leading or trailing dots or spaces, never empty. Never
 * returns NULL or an empty string. */
char *comfy_instance_sanitize_name (const char *name);

/* Return the packaged default instance icon path, or NULL when it is not
 * available. The returned path is owned by the caller. */
char *comfy_instance_default_icon_path (void);

/* (transfer full) NFC-normalized display name with Unicode surrounding
 * whitespace removed. Control characters remain in the result so validation
 * can reject them rather than silently changing what the user entered. */
char *comfy_instance_normalize_name (const char *name);

/* Creates an instance and validates every field. @id may be NULL to
 * generate a random UUID. @directory must be an absolute path and is
 * canonicalized; it must exist or be creatable by the caller. */
ComfyInstance *comfy_instance_new (const char *id,
                               const char *name,
                               const char *directory,
                               const char *minecraft_version,
                               ComfyLoader   loader,
                               GError    **error);

/* Reads one instance from a JSON object. Malformed or incomplete input
 * fails with a GError instead of producing a half built instance. */
ComfyInstance *comfy_instance_new_from_json (JsonNode *node, GError **error);

/* (transfer full). The instance must be valid, see
 * comfy_instance_validate(). */
JsonNode *comfy_instance_to_json (ComfyInstance *self);

/* TRUE when every field of @self holds a usable value. */
gboolean comfy_instance_validate (ComfyInstance *self, GError **error);

/* Model-level mutation helpers. Callers should use these instead of writing
 * instance properties directly; validation is performed before the value is
 * accepted. */
gboolean comfy_instance_set_string (ComfyInstance *self, const char *property,
                                  const char *value, GError **error);
gboolean comfy_instance_set_loader (ComfyInstance *self, ComfyLoader loader,
                                  const char *version, GError **error);
gboolean comfy_instance_set_name (ComfyInstance *self, const char *name,
                                GError **error);
gboolean comfy_instance_set_directory (ComfyInstance *self, const char *directory,
                                     GError **error);

/* The unique id, never NULL. */
const char *comfy_instance_get_id (ComfyInstance *self);

/* Borrowed component map keyed by stable component id. Property access and
 * legacy JSON remain compatibility views over this map. */
GHashTable *comfy_instance_get_components (ComfyInstance *self);

G_END_DECLS

#endif /* comfy_INSTANCE_H */
