/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstance: one launcher instance, a Minecraft installation with its
 * own directory, game version, mod loader and Java settings.
 *
 * Plain data object with JSON serialization: no widgets and no file or
 * network I/O live here. */

#ifndef AML_INSTANCE_H
#define AML_INSTANCE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Mod loaders AML knows about. In JSON the loader is the lowercase
 * string, e.g. "fabric". */
typedef enum {
  AML_LOADER_VANILLA,
  AML_LOADER_FABRIC,
  AML_LOADER_FORGE,
  AML_LOADER_NEOFORGE,
  AML_LOADER_QUILT,
} AmlLoader;

#define AML_TYPE_INSTANCE (aml_instance_get_type ())

G_DECLARE_FINAL_TYPE (AmlInstance, aml_instance, AML, INSTANCE, GObject)

#define AML_INSTANCE_ERROR (aml_instance_error_quark ())

typedef enum {
  AML_INSTANCE_ERROR_INVALID,      /* a field holds a bad value */
  AML_INSTANCE_ERROR_MISSING_FIELD /* a required field is absent */
} AmlInstanceError;

GQuark aml_instance_error_quark (void);

GType aml_loader_get_type (void);

/* String form of @loader as written to JSON, or NULL if @loader is not
 * a known value. */
const char *aml_loader_to_string (AmlLoader loader);

/* Parse the JSON form of a loader. Returns FALSE for unknown values. */
gboolean aml_loader_from_string (const char *string, AmlLoader *loader);

/* A name that is safe to use as a directory component: no slashes, no
 * "..", no leading or trailing dots or spaces, never empty. Never
 * returns NULL or an empty string. */
char *aml_instance_sanitize_name (const char *name);

/* (transfer full) NFC-normalized display name with Unicode surrounding
 * whitespace removed. Control characters remain in the result so validation
 * can reject them rather than silently changing what the user entered. */
char *aml_instance_normalize_name (const char *name);

/* Creates an instance and validates every field. @id may be NULL to
 * generate a random UUID. @directory must be an absolute path and is
 * canonicalized; it must exist or be creatable by the caller. */
AmlInstance *aml_instance_new (const char *id,
                               const char *name,
                               const char *directory,
                               const char *minecraft_version,
                               AmlLoader   loader,
                               GError    **error);

/* Reads one instance from a JSON object. Malformed or incomplete input
 * fails with a GError instead of producing a half built instance. */
AmlInstance *aml_instance_new_from_json (JsonNode *node, GError **error);

/* (transfer full). The instance must be valid, see
 * aml_instance_validate(). */
JsonNode *aml_instance_to_json (AmlInstance *self);

/* TRUE when every field of @self holds a usable value. */
gboolean aml_instance_validate (AmlInstance *self, GError **error);

/* Model-level mutation helpers. Callers should use these instead of writing
 * instance properties directly; validation is performed before the value is
 * accepted. */
gboolean aml_instance_set_string (AmlInstance *self, const char *property,
                                  const char *value, GError **error);
gboolean aml_instance_set_loader (AmlInstance *self, AmlLoader loader,
                                  const char *version, GError **error);
gboolean aml_instance_set_name (AmlInstance *self, const char *name,
                                GError **error);
gboolean aml_instance_set_directory (AmlInstance *self, const char *directory,
                                     GError **error);

/* The unique id, never NULL. */
const char *aml_instance_get_id (AmlInstance *self);

/* Borrowed component map keyed by stable component id. Property access and
 * legacy JSON remain compatibility views over this map. */
GHashTable *aml_instance_get_components (AmlInstance *self);

G_END_DECLS

#endif /* AML_INSTANCE_H */
