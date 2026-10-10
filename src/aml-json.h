/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_JSON_H
#define AML_JSON_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Small, non-throwing readers for the JSON shapes returned by APIs. A
 * missing field and a field with the wrong type both use the fallback. */
JsonObject *aml_json_object_of (JsonNode *node);
char *aml_json_string (JsonNode *node, const char *name);
gboolean aml_json_boolean (JsonNode *node, const char *name,
                           gboolean fallback);
gint64 aml_json_int (JsonNode *node, const char *name, gint64 fallback);
GPtrArray *aml_json_string_array (JsonNode *node, const char *name);

G_END_DECLS

#endif /* AML_JSON_H */
