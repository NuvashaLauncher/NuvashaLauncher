/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaInstance: one launcher instance, a Minecraft installation with its
 * own directory, game version, mod loader and Java settings.
 *
 * Plain data object with JSON serialization: no widgets and no file or
 * network I/O live here. */

#ifndef leppoisa_INSTANCE_H
#define leppoisa_INSTANCE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Mod loaders leppoisa knows about. In JSON the loader is the lowercase
 * string, e.g. "fabric". */
typedef enum {
  leppoisa_LOADER_VANILLA,
  leppoisa_LOADER_FABRIC,
  leppoisa_LOADER_FORGE,
  leppoisa_LOADER_NEOFORGE,
  leppoisa_LOADER_QUILT,
} LeppoisaLoader;

#define leppoisa_TYPE_INSTANCE (leppoisa_instance_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaInstance, leppoisa_instance, leppoisa, INSTANCE, GObject)

#define leppoisa_INSTANCE_ERROR (leppoisa_instance_error_quark ())

typedef enum {
  leppoisa_INSTANCE_ERROR_INVALID,      /* a field holds a bad value */
  leppoisa_INSTANCE_ERROR_MISSING_FIELD /* a required field is absent */
} LeppoisaInstanceError;

GQuark leppoisa_instance_error_quark (void);

GType leppoisa_loader_get_type (void);

/* String form of @loader as written to JSON, or NULL if @loader is not
 * a known value. */
const char *leppoisa_loader_to_string (LeppoisaLoader loader);

/* Parse the JSON form of a loader. Returns FALSE for unknown values. */
gboolean leppoisa_loader_from_string (const char *string, LeppoisaLoader *loader);

/* A name that is safe to use as a directory component: no slashes, no
 * "..", no leading or trailing dots or spaces, never empty. Never
 * returns NULL or an empty string. */
char *leppoisa_instance_sanitize_name (const char *name);

/* Return the packaged default instance icon path, or NULL when it is not
 * available. The returned path is owned by the caller. */
char *leppoisa_instance_default_icon_path (void);

/* (transfer full) NFC-normalized display name with Unicode surrounding
 * whitespace removed. Control characters remain in the result so validation
 * can reject them rather than silently changing what the user entered. */
char *leppoisa_instance_normalize_name (const char *name);

/* Creates an instance and validates every field. @id may be NULL to
 * generate a random UUID. @directory must be an absolute path and is
 * canonicalized; it must exist or be creatable by the caller. */
LeppoisaInstance *leppoisa_instance_new (const char *id,
                               const char *name,
                               const char *directory,
                               const char *minecraft_version,
                               LeppoisaLoader   loader,
                               GError    **error);

/* Reads one instance from a JSON object. Malformed or incomplete input
 * fails with a GError instead of producing a half built instance. */
LeppoisaInstance *leppoisa_instance_new_from_json (JsonNode *node, GError **error);

/* (transfer full). The instance must be valid, see
 * leppoisa_instance_validate(). */
JsonNode *leppoisa_instance_to_json (LeppoisaInstance *self);

/* TRUE when every field of @self holds a usable value. */
gboolean leppoisa_instance_validate (LeppoisaInstance *self, GError **error);

/* Model-level mutation helpers. Callers should use these instead of writing
 * instance properties directly; validation is performed before the value is
 * accepted. */
gboolean leppoisa_instance_set_string (LeppoisaInstance *self, const char *property,
                                  const char *value, GError **error);
gboolean leppoisa_instance_set_loader (LeppoisaInstance *self, LeppoisaLoader loader,
                                  const char *version, GError **error);
gboolean leppoisa_instance_set_name (LeppoisaInstance *self, const char *name,
                                GError **error);
gboolean leppoisa_instance_set_directory (LeppoisaInstance *self, const char *directory,
                                     GError **error);

/* The unique id, never NULL. */
const char *leppoisa_instance_get_id (LeppoisaInstance *self);

/* Borrowed component map keyed by stable component id. Property access and
 * legacy JSON remain compatibility views over this map. */
GHashTable *leppoisa_instance_get_components (LeppoisaInstance *self);

G_END_DECLS

#endif /* leppoisa_INSTANCE_H */
