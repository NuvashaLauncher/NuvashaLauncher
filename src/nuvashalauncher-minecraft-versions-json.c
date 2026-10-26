/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Mojang version metadata types, rule evaluation and JSON parsing. The
 * asynchronous cache/fetch pipeline lives in nuvashalauncher-minecraft-versions.c. */

#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-minecraft-versions-json-private.h"

#include <math.h>
#include <string.h>

#define MAX_VERSION_REGEX_LENGTH 1024
#define MAX_VERSION_VALUE_LENGTH 1024
#include <sys/utsname.h>

/* ---------------------------------------------------------- *
 * Types and errors
 * ---------------------------------------------------------- */

GQuark
nuvashalauncher_mc_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-minecraft-versions-error");
}

/* The order has to match nuvashalauncherMcVersionType. */
static const char * const version_type_names[] = {
  "release",
  "snapshot",
  "old_beta",
  "old_alpha",
};

gboolean
nuvashalauncher_mc_version_type_from_string (const char *string, nuvashalauncherMcVersionType *type)
{
  g_return_val_if_fail (type != NULL, FALSE);

  if (string == NULL)
    return FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (version_type_names); i++)
    {
      if (g_str_equal (string, version_type_names[i]))
        {
          *type = (nuvashalauncherMcVersionType) i;
          return TRUE;
        }
    }

  return FALSE;
}

const char *
nuvashalauncher_mc_version_type_to_string (nuvashalauncherMcVersionType type)
{
  if ((guint) type >= G_N_ELEMENTS (version_type_names))
    return NULL;

  return version_type_names[type];
}

G_DEFINE_FINAL_TYPE (nuvashalauncherVersionManifest, nuvashalauncher_version_manifest, G_TYPE_OBJECT)

static nuvashalauncherVersionEntry *
parse_entry (JsonObject *object, GError **error)
{
  nuvashalauncherVersionEntry *entry = g_new0 (nuvashalauncherVersionEntry, 1);
  g_autofree char *type_name = NULL;

  if (!nuvashalauncher_mc_get_string (object, "id", TRUE, &entry->id, error) ||
      !nuvashalauncher_mc_get_string (object, "type", TRUE, &type_name, error) ||
      !nuvashalauncher_mc_get_string (object, "url", TRUE, &entry->url, error) ||
      !nuvashalauncher_mc_get_string (object, "sha1", TRUE, &entry->sha1, error) ||
      !nuvashalauncher_mc_get_string (object, "releaseTime", TRUE, &entry->release_time, error))
    {
      nuvashalauncher_mc_free_entry (entry);
      return NULL;
    }

  if (!nuvashalauncher_mc_version_type_from_string (type_name, &entry->type))
    {
      g_set_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID,
                   "unknown version type \"%s\"", type_name);
      nuvashalauncher_mc_free_entry (entry);
      return NULL;
    }

  return entry;
}

static gboolean
fill_manifest (JsonObject *root, nuvashalauncherVersionManifest *self, GError **error)
{
  JsonObject *latest = NULL;
  JsonArray *versions = NULL;

  if (!nuvashalauncher_mc_get_object (root, "latest", TRUE, &latest, error))
    return FALSE;

  if (latest == NULL)
    return nuvashalauncher_mc_missing_field ("latest", error);

  if (!nuvashalauncher_mc_get_string (latest, "release", TRUE, &self->latest_release, error) ||
      !nuvashalauncher_mc_get_string (latest, "snapshot", TRUE, &self->latest_snapshot, error))
    return FALSE;

  if (!nuvashalauncher_mc_get_array (root, "versions", TRUE, &versions, error))
    return FALSE;

  if (versions == NULL)
    return nuvashalauncher_mc_missing_field ("versions", error);

  self->entries = g_ptr_array_new_with_free_func (nuvashalauncher_mc_free_entry);

  for (guint i = 0; i < json_array_get_length (versions); i++)
    {
      JsonNode *item = json_array_get_element (versions, i);
      nuvashalauncherVersionEntry *entry;

      if (!JSON_NODE_HOLDS_OBJECT (item))
        {
          g_set_error (error, nuvashalauncher_MC_ERROR, nuvashalauncher_MC_ERROR_INVALID,
                       "version %u is not an object", i);
          return FALSE;
        }

      entry = parse_entry (json_node_get_object (item), error);

      if (entry == NULL)
        return FALSE;

      g_ptr_array_add (self->entries, entry);
    }

  return TRUE;
}

nuvashalauncherVersionManifest *
nuvashalauncher_version_manifest_new_from_json (const char *json,
                                    gssize      length,
                                    GError    **error)
{
  g_autoptr (JsonParser) parser = NULL;
  nuvashalauncherVersionManifest *self;
  JsonObject *root;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (json != NULL, NULL);

  root = nuvashalauncher_mc_root_object (json, length, &parser, error);

  if (root == NULL)
    return NULL;

  self = g_object_new (nuvashalauncher_TYPE_VERSION_MANIFEST, NULL);

  if (!fill_manifest (root, self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

const nuvashalauncherVersionEntry *
nuvashalauncher_version_manifest_find (nuvashalauncherVersionManifest *self, const char *id)
{
  g_return_val_if_fail (nuvashalauncher_IS_VERSION_MANIFEST (self), NULL);
  g_return_val_if_fail (id != NULL, NULL);

  if (self->entries == NULL)
    return NULL;

  for (guint i = 0; i < self->entries->len; i++)
    {
      nuvashalauncherVersionEntry *entry = g_ptr_array_index (self->entries, i);

      if (g_str_equal (entry->id, id))
        return entry;
    }

  return NULL;
}

static void
nuvashalauncher_version_manifest_finalize (GObject *object)
{
  nuvashalauncherVersionManifest *self = nuvashalauncher_VERSION_MANIFEST (object);

  g_free (self->latest_release);
  g_free (self->latest_snapshot);
  g_clear_pointer (&self->entries, g_ptr_array_unref);

  G_OBJECT_CLASS (nuvashalauncher_version_manifest_parent_class)->finalize (object);
}

static void
nuvashalauncher_version_manifest_class_init (nuvashalauncherVersionManifestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nuvashalauncher_version_manifest_finalize;
}

static void
nuvashalauncher_version_manifest_init (nuvashalauncherVersionManifest *self)
{
  (void) self;
}

/* ---------------------------------------------------------- *
 * Merging a version with the one it inherits from
 * ---------------------------------------------------------- */

static char *
either (const char *child_value, const char *parent_value)
{
  return g_strdup (child_value != NULL ? child_value : parent_value);
}

static nuvashalauncherDownloadInfo *
download_copy (const nuvashalauncherDownloadInfo *source)
{
  nuvashalauncherDownloadInfo *copy;

  if (source == NULL)
    return NULL;

  copy = g_new0 (nuvashalauncherDownloadInfo, 1);
  copy->id = g_strdup (source->id);
  copy->url = g_strdup (source->url);
  copy->sha1 = g_strdup (source->sha1);
  copy->path = g_strdup (source->path);
  copy->size = source->size;
  copy->total_size = source->total_size;

  return copy;
}

static nuvashalauncherRule *
rule_copy (const nuvashalauncherRule *source)
{
  nuvashalauncherRule *copy = g_new0 (nuvashalauncherRule, 1);

  copy->allow = source->allow;
  copy->os_name = g_strdup (source->os_name);
  copy->os_arch = g_strdup (source->os_arch);
  copy->os_version = g_strdup (source->os_version);

  if (source->features != NULL)
    {
      GHashTableIter iter;
      gpointer name = NULL;
      gpointer required = NULL;

      copy->features = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);
      g_hash_table_iter_init (&iter, source->features);

      while (g_hash_table_iter_next (&iter, &name, &required))
        g_hash_table_replace (copy->features, g_strdup (name), required);
    }

  return copy;
}

static GPtrArray *
rules_copy (GPtrArray *source)
{
  GPtrArray *copy;

  if (source == NULL)
    return NULL;

  copy = g_ptr_array_new_with_free_func (nuvashalauncher_mc_free_rule);

  for (guint i = 0; i < source->len; i++)
    g_ptr_array_add (copy, rule_copy (g_ptr_array_index (source, i)));

  return copy;
}

static GPtrArray *
strings_copy (GPtrArray *source)
{
  GPtrArray *copy;

  if (source == NULL)
    return NULL;

  copy = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < source->len; i++)
    g_ptr_array_add (copy, g_strdup (g_ptr_array_index (source, i)));

  return copy;
}

static GHashTable *
classifiers_copy (GHashTable *source)
{
  GHashTable *copy;
  GHashTableIter iter;
  gpointer name = NULL;
  gpointer info = NULL;

  if (source == NULL)
    return NULL;

  copy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                nuvashalauncher_mc_free_download_info);
  g_hash_table_iter_init (&iter, source);

  while (g_hash_table_iter_next (&iter, &name, &info))
    g_hash_table_replace (copy, g_strdup (name), download_copy (info));

  return copy;
}

static GHashTable *
natives_copy (GHashTable *source)
{
  GHashTable *copy;
  GHashTableIter iter;
  gpointer name = NULL;
  gpointer classifier = NULL;

  if (source == NULL)
    return NULL;

  copy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_iter_init (&iter, source);

  while (g_hash_table_iter_next (&iter, &name, &classifier))
    g_hash_table_replace (copy, g_strdup (name), g_strdup (classifier));

  return copy;
}

static nuvashalauncherLibrary *
library_copy (const nuvashalauncherLibrary *source)
{
  nuvashalauncherLibrary *copy = g_new0 (nuvashalauncherLibrary, 1);

  copy->name = g_strdup (source->name);
  copy->artifact = download_copy (source->artifact);
  copy->classifiers = classifiers_copy (source->classifiers);
  copy->natives = natives_copy (source->natives);
  copy->rules = rules_copy (source->rules);
  copy->exclude = strings_copy (source->exclude);

  return copy;
}

static nuvashalauncherArgument *
argument_copy (const nuvashalauncherArgument *source)
{
  nuvashalauncherArgument *copy = g_new0 (nuvashalauncherArgument, 1);

  copy->rules = rules_copy (source->rules);
  copy->values = strings_copy (source->values);

  return copy;
}

static nuvashalauncherLogging *
logging_copy (const nuvashalauncherLogging *source)
{
  nuvashalauncherLogging *copy;

  if (source == NULL)
    return NULL;

  copy = g_new0 (nuvashalauncherLogging, 1);
  copy->argument = g_strdup (source->argument);
  copy->type = g_strdup (source->type);
  copy->file = download_copy (source->file);

  return copy;
}

static nuvashalauncherJavaVersion *
java_version_copy (const nuvashalauncherJavaVersion *source)
{
  nuvashalauncherJavaVersion *copy;

  if (source == NULL)
    return NULL;

  copy = g_new0 (nuvashalauncherJavaVersion, 1);
  copy->component = g_strdup (source->component);
  copy->major_version = source->major_version;

  return copy;
}

static GPtrArray *
concat_libraries (GPtrArray *parent, GPtrArray *child)
{
  GPtrArray *all = g_ptr_array_new_with_free_func (nuvashalauncher_mc_free_library);

  for (guint i = 0; parent != NULL && i < parent->len; i++)
    g_ptr_array_add (all, library_copy (g_ptr_array_index (parent, i)));

  for (guint i = 0; child != NULL && i < child->len; i++)
    g_ptr_array_add (all, library_copy (g_ptr_array_index (child, i)));

  return all;
}

static GPtrArray *
concat_arguments (GPtrArray *parent, GPtrArray *child)
{
  GPtrArray *all = g_ptr_array_new_with_free_func (nuvashalauncher_mc_free_argument);

  for (guint i = 0; parent != NULL && i < parent->len; i++)
    g_ptr_array_add (all, argument_copy (g_ptr_array_index (parent, i)));

  for (guint i = 0; child != NULL && i < child->len; i++)
    g_ptr_array_add (all, argument_copy (g_ptr_array_index (child, i)));

  return all;
}

nuvashalauncherVersionDescription *
nuvashalauncher_version_description_merge (nuvashalauncherVersionDescription *parent,
                               nuvashalauncherVersionDescription *child)
{
  nuvashalauncherVersionDescription *self;

  g_return_val_if_fail (nuvashalauncher_IS_VERSION_DESCRIPTION (parent), NULL);
  g_return_val_if_fail (nuvashalauncher_IS_VERSION_DESCRIPTION (child), NULL);

  self = g_object_new (nuvashalauncher_TYPE_VERSION_DESCRIPTION, NULL);

  /* The identity of the merged version is the child's: that is the
   * name the user picked. */
  self->id = g_strdup (child->id);
  self->type = child->type;
  self->main_class = either (child->main_class, parent->main_class);
  self->assets = either (child->assets, parent->assets);
  self->minecraft_arguments = either (child->minecraft_arguments,
                                      parent->minecraft_arguments);
  self->modern_arguments = child->modern_arguments || parent->modern_arguments;

  /* The merged version keeps climbing the parent's chain, so a caller
   * can resolve a whole line of profiles one step at a time. */
  self->inherits_from = g_strdup (parent->inherits_from);

  self->asset_index = download_copy (child->asset_index != NULL
                                     ? child->asset_index
                                     : parent->asset_index);
  self->client = download_copy (child->client != NULL
                                ? child->client
                                : parent->client);
  self->logging = logging_copy (child->logging != NULL
                                ? child->logging
                                : parent->logging);
  self->java_version = java_version_copy (child->java_version != NULL
                                          ? child->java_version
                                          : parent->java_version);

  self->libraries = concat_libraries (parent->libraries, child->libraries);
  self->jvm_arguments = concat_arguments (parent->jvm_arguments,
                                          child->jvm_arguments);
  self->game_arguments = concat_arguments (parent->game_arguments,
                                           child->game_arguments);

  return self;
}

GPtrArray *
nuvashalauncher_version_description_legacy_argv (nuvashalauncherVersionDescription *self)
{
  GPtrArray *words;
  gchar **argv = NULL;
  gint argc = 0;

  g_return_val_if_fail (nuvashalauncher_IS_VERSION_DESCRIPTION (self), NULL);

  words = g_ptr_array_new_with_free_func (g_free);

  if (self->minecraft_arguments == NULL)
    return words;

  if (g_shell_parse_argv (self->minecraft_arguments, &argc, &argv, NULL))
    {
      for (gint i = 0; i < argc; i++)
        g_ptr_array_add (words, g_strdup (argv[i]));

      g_strfreev (argv);
      return words;
    }

  /* Not a command line after all. The version that wrote it simply
   * split the words on spaces, and so does this. */
  {
    gchar **parts = g_strsplit (self->minecraft_arguments, " ", -1);

    for (guint i = 0; parts[i] != NULL; i++)
      {
        if (parts[i][0] != '\0')
          g_ptr_array_add (words, g_strdup (parts[i]));
      }

    g_strfreev (parts);
  }

  return words;
}
