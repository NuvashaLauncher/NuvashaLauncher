#ifndef nuvashalauncher_MINECRAFT_VERSIONS_JSON_PRIVATE_H
#define nuvashalauncher_MINECRAFT_VERSIONS_JSON_PRIVATE_H

#include "nuvashalauncher-minecraft-versions.h"

#include <json-glib/json-glib.h>


void nuvashalauncher_mc_free_download_info (gpointer data);
void nuvashalauncher_mc_free_rule (gpointer data);
void nuvashalauncher_mc_free_library (gpointer data);
void nuvashalauncher_mc_free_argument (gpointer data);
void nuvashalauncher_mc_free_logging (gpointer data);
void nuvashalauncher_mc_free_java_version (gpointer data);
void nuvashalauncher_mc_free_entry (gpointer data);

#endif

gboolean nuvashalauncher_mc_missing_field (const char *member, GError **error);
gboolean nuvashalauncher_mc_get_string (JsonObject *object, const char *member, gboolean required, char **value, GError **error);
gboolean nuvashalauncher_mc_get_object (JsonObject *object, const char *member, gboolean required, JsonObject **value, GError **error);
gboolean nuvashalauncher_mc_get_array (JsonObject *object, const char *member, gboolean required, JsonArray **value, GError **error);
gboolean nuvashalauncher_mc_get_int64 (JsonObject *object, const char *member, gboolean required, gint64 *value, GError **error);
JsonObject *nuvashalauncher_mc_root_object (const char *json, gssize length, JsonParser **parser, GError **error);

nuvashalauncherRule *nuvashalauncher_mc_parse_rule (JsonObject *object, GError **error);
GPtrArray *nuvashalauncher_mc_parse_rules (JsonNode *node, GError **error);

gboolean nuvashalauncher_mc_get_rules_member (JsonObject *object, const char *member, GPtrArray **rules, GError **error);
