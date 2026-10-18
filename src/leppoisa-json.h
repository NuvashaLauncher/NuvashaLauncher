/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_JSON_H
#define leppoisa_JSON_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define leppoisa_JSON_ERROR (leppoisa_json_error_quark ())
typedef enum {
  leppoisa_JSON_ERROR_TYPE,
  leppoisa_JSON_ERROR_MISSING
} LeppoisaJsonError;

GQuark leppoisa_json_error_quark (void);

/* Small, non-throwing readers for the JSON shapes returned by APIs. A
 * missing field and a field with the wrong type both use the fallback. */
JsonObject *leppoisa_json_object_of (JsonNode *node);
char *leppoisa_json_string (JsonNode *node, const char *name);
gboolean leppoisa_json_boolean (JsonNode *node, const char *name,
                           gboolean fallback);
gint64 leppoisa_json_int (JsonNode *node, const char *name, gint64 fallback);
GPtrArray *leppoisa_json_string_array (JsonNode *node, const char *name);

/* Required-field readers for API documents. Unlike the optional readers
 * above, these distinguish malformed responses from absent optional data. */
gboolean leppoisa_json_require_string (JsonNode *node, const char *name,
                                  char **value, GError **error);

G_END_DECLS

#endif /* leppoisa_JSON_H */
