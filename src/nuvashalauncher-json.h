/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_JSON_H
#define nuvashalauncher_JSON_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define nuvashalauncher_JSON_ERROR (nuvashalauncher_json_error_quark ())
typedef enum {
  nuvashalauncher_JSON_ERROR_TYPE,
  nuvashalauncher_JSON_ERROR_MISSING
} nuvashalauncherJsonError;

GQuark nuvashalauncher_json_error_quark (void);

/* Small, non-throwing readers for the JSON shapes returned by APIs. A
 * missing field and a field with the wrong type both use the fallback. */
JsonObject *nuvashalauncher_json_object_of (JsonNode *node);
char *nuvashalauncher_json_string (JsonNode *node, const char *name);
gboolean nuvashalauncher_json_boolean (JsonNode *node, const char *name,
                           gboolean fallback);
gint64 nuvashalauncher_json_int (JsonNode *node, const char *name, gint64 fallback);
GPtrArray *nuvashalauncher_json_string_array (JsonNode *node, const char *name);

/* Required-field readers for API documents. These distinguish malformed
 * responses from absent optional data. */
gboolean nuvashalauncher_json_require_string (JsonNode *node, const char *name,
                                  char **value, GError **error);

G_END_DECLS

#endif /* nuvashalauncher_JSON_H */
