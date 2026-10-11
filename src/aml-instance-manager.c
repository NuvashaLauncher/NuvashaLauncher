/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstanceManager: owns the instance list and keeps instances.json
 * in sync with it.
 *
 * Slow work (copying and deleting instance directories, launching the
 * file manager) runs through GIO async calls so the main loop never
 * blocks. */

/* realpath wants glibc's default feature set; AML is Linux-only. */
#define _GNU_SOURCE

#include "aml-instance-manager.h"
#include "aml-i18n.h"
#include "aml-fs.h"
#include "aml-task.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout version of instances.json. */
#define INSTANCES_FORMAT_VERSION 1

#define METADATA_FILE "instances.json"
#define CORRUPT_SUFFIX ".corrupt"

struct _AmlInstanceManager
{
  GObject parent_instance;

  GListStore *instances;
  char *data_dir;
  char *metadata_path;
  char *instances_dir;
};

G_DEFINE_TYPE (AmlInstanceManager, aml_instance_manager, G_TYPE_OBJECT)

GQuark
aml_instance_manager_error_quark (void)
{
  return g_quark_from_static_string ("aml-instance-manager-error");
}

static char *
instance_directory (AmlInstance *instance)
{
  char *directory = NULL;

  g_object_get (instance, "directory", &directory, NULL);
  return directory;
}

static char *
instance_name (AmlInstance *instance)
{
  char *name = NULL;

  g_object_get (instance, "name", &name, NULL);
  return name;
}

static char *
instance_name_key (const char *name)
{
  g_autofree char *normalized = aml_instance_normalize_name (name);
  return g_utf8_casefold (normalized, -1);
}

static gboolean
name_is_taken (AmlInstanceManager *self, AmlInstance *except,
               const char *name)
{
  g_autofree char *wanted = instance_name_key (name);
  guint count = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < count; i++)
    {
      g_autoptr (AmlInstance) other =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);
      g_autofree char *other_name = instance_name (other);
      g_autofree char *other_key = instance_name_key (other_name);

      if (other != except && g_strcmp0 (wanted, other_key) == 0)
        return TRUE;
    }

  return FALSE;
}

/* TRUE when @path contains a ".." component, which would let a path
 * point outside of what was actually picked. */
static gboolean
path_has_parent_reference (const char *path)
{
  const char *segment = path;

  while (*segment != '\0')
    {
      const char *end = strchr (segment, '/');

      if (end == NULL)
        end = segment + strlen (segment);

      if (end - segment == 2 && segment[0] == '.' && segment[1] == '.')
        return TRUE;

      segment = (*end == '\0') ? end : end + 1;
    }

  return FALSE;
}

/* TRUE when @parent is @child or an ancestor of it. Both must be
 * canonical absolute paths. */
static gboolean
path_contains (const char *parent, const char *child)
{
  gsize length = strlen (parent);

  if (g_strcmp0 (parent, child) == 0)
    return TRUE;

  return g_str_has_prefix (child, parent) && child[length] == '/';
}

static char *
default_instance_directory (AmlInstanceManager *self,
                            const char *name,
                            const char *id)
{
  g_autofree char *safe_name = aml_instance_sanitize_name (name);
  g_autofree char *leaf = g_strdup_printf ("%s-%.8s", safe_name, id);

  return g_build_filename (self->instances_dir, leaf, NULL);
}

/* Checks that @directory may become an instance directory: absolute,
 * without "..", not the filesystem root and neither overlapping nor
 * nested with the directory of an instance that already exists.
 * Returns the canonical path.
 *
 * @moved is the instance whose directory is being changed, or NULL when
 * a new one is being made: an instance never overlaps itself, so the one
 * being moved is not counted against it. */
static gboolean
check_instance_directory (AmlInstanceManager  *self,
                          const char          *directory,
                          AmlInstance         *moved,
                          char               **canonical,
                          GError             **error)
{
  if (directory == NULL || !g_path_is_absolute (directory))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the instance directory must be an absolute path");
      return FALSE;
    }

  if (path_has_parent_reference (directory))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the instance directory must not contain \"..\"");
      return FALSE;
    }

  g_autofree char *path = g_canonicalize_filename (directory, NULL);

  /* canonicalize is lexical: it keeps a symlink as it is written. When
     the directory exists, resolve it too, so a link cannot hide the real
     directory from the overlap checks below. */
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      char *resolved = realpath (path, NULL);

      if (resolved != NULL)
        {
          g_free (path);
          path = resolved;
        }
    }

  g_autofree char *parent = g_path_get_dirname (path);
  if (!aml_fs_directory_is_real (parent))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the parent of the instance directory must not contain "
                   "symlinks and must already exist");
      return FALSE;
    }

  if (g_strcmp0 (path, G_DIR_SEPARATOR_S) == 0)
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the filesystem root cannot be an instance directory");
      return FALSE;
    }

  /* The data directory holds instances.json: an instance may not be the
     data directory itself, or its files would sit beside the metadata. */
  if (self->data_dir != NULL && g_strcmp0 (path, self->data_dir) == 0)
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the AML data directory cannot be an instance directory");
      return FALSE;
    }

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (AmlInstance) other =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);
      g_autofree char *other_name = instance_name (other);
      g_autofree char *other_directory = instance_directory (other);

      if (other == moved)
        continue;

      if (path_contains (path, other_directory) ||
          path_contains (other_directory, path))
        {
          if (g_strcmp0 (path, other_directory) == 0)
            g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                         AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                         "the directory is already used by instance \"%s\"",
                         other_name);
          else
            g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                         AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                         "the directory \"%s\" overlaps the directory of "
                         "instance \"%s\"",
                         path, other_name);
          return FALSE;
        }
    }

  *canonical = g_steal_pointer (&path);
  return TRUE;
}

static void
aml_instance_manager_finalize (GObject *object)
{
  AmlInstanceManager *self = AML_INSTANCE_MANAGER (object);

  g_clear_object (&self->instances);
  g_free (self->data_dir);
  g_free (self->metadata_path);
  g_free (self->instances_dir);

  G_OBJECT_CLASS (aml_instance_manager_parent_class)->finalize (object);
}

static void
aml_instance_manager_class_init (AmlInstanceManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = aml_instance_manager_finalize;
}

static void
aml_instance_manager_init (AmlInstanceManager *self)
{
  self->instances = g_list_store_new (AML_TYPE_INSTANCE);
}

AmlInstanceManager *
aml_instance_manager_new (const char *data_dir)
{
  g_return_val_if_fail (data_dir == NULL || g_path_is_absolute (data_dir),
                        NULL);

  AmlInstanceManager *self =
    g_object_new (AML_TYPE_INSTANCE_MANAGER, NULL);

  if (data_dir == NULL)
    {
      g_autofree char *user_dir = g_build_filename (g_get_user_data_dir (),
                                                    "AML", NULL);
      self->data_dir = g_canonicalize_filename (user_dir, NULL);
    }
  else
    self->data_dir = g_canonicalize_filename (data_dir, NULL);

  self->metadata_path =
    g_build_filename (self->data_dir, METADATA_FILE, NULL);
  self->instances_dir =
    g_build_filename (self->data_dir, "instances", NULL);

  return self;
}

GListStore *
aml_instance_manager_get_instances (AmlInstanceManager *self)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);

  return self->instances;
}

char *
aml_instance_manager_default_directory (AmlInstanceManager *self,
                                        const char *name)
{
  g_autofree char *id = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);

  id = g_uuid_string_random ();

  return default_instance_directory (self, name, id);
}

AmlInstance *
aml_instance_manager_get_by_id (AmlInstanceManager *self, const char *id)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);

  if (id == NULL)
    return NULL;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (AmlInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);

      if (g_str_equal (aml_instance_get_id (instance), id))
        return instance;
    }

  return NULL;
}

/* Serializes every registered instance. Fails when one of them does
 * not validate, so an invalid edit never reaches the file. */
static char *
build_metadata_json (AmlInstanceManager *self, GError **error)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "version");
  json_builder_add_int_value (builder, INSTANCES_FORMAT_VERSION);

  json_builder_set_member_name (builder, "instances");
  json_builder_begin_array (builder);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (AmlInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);

      if (!aml_instance_validate (instance, error))
        return NULL;

      /* The builder takes ownership of the node. */
      json_builder_add_value (builder, aml_instance_to_json (instance));
    }

  json_builder_end_array (builder);
  json_builder_end_object (builder);

  g_autoptr (JsonNode) root = json_builder_get_root (builder);
  g_autoptr (JsonGenerator) generator = json_generator_new ();

  json_generator_set_pretty (generator, TRUE);
  json_generator_set_root (generator, root);

  return json_generator_to_data (generator, NULL);
}

gboolean
aml_instance_manager_save (AmlInstanceManager *self, GError **error)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_autofree char *json = build_metadata_json (self, error);

  if (json == NULL)
    return FALSE;

  g_autoptr (GError) directory_error = NULL;
  if (!aml_fs_make_directory (self->data_dir, 0700, &directory_error))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_IO,
                   "cannot create \"%s\": %s",
                    self->data_dir, directory_error->message);
      return FALSE;
    }

  g_autoptr (GError) local_error = NULL;

  /* The data goes to a temporary file in the same directory, is flushed
   * and fsynced and then renamed over instances.json, so a save that
   * fails or is interrupted can never leave a half written file. */
  if (!aml_fs_replace_contents (self->metadata_path, json, -1, 0600,
                                &local_error))
    {
      g_prefix_error (&local_error, "cannot save \"%s\": ",
                      self->metadata_path);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

static gboolean
get_metadata_version (JsonObject *object, GError **error)
{
  JsonNode *node = json_object_get_member (object, "version");

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_INT64)
    {
      if (json_node_get_int (node) == INSTANCES_FORMAT_VERSION)
        return TRUE;

      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "unsupported instances.json version %" G_GINT64_FORMAT,
                   json_node_get_int (node));
      return FALSE;
    }

  g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
               AML_INSTANCE_MANAGER_ERROR_CORRUPT,
               "instances.json has no integer \"version\"");
  return FALSE;
}

/* Parses the whole file into a list of instances. Any problem fails
 * with a GError and returns NULL: a file is either fully understood or
 * treated as corrupt. */
static GList *
parse_instances_json (const char *contents, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, contents, length, error))
    return NULL;

  JsonNode *root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "instances.json is not a JSON object");
      return NULL;
    }

  JsonObject *object = json_node_get_object (root);

  if (!get_metadata_version (object, error))
    return NULL;

  JsonNode *array_node = json_object_get_member (object, "instances");

  if (array_node == NULL || !JSON_NODE_HOLDS_ARRAY (array_node))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "instances.json has no \"instances\" array");
      return NULL;
    }

  JsonArray *array = json_node_get_array (array_node);
  guint n_items = json_array_get_length (array);
  GList *list = NULL;

  for (guint i = 0; i < n_items; i++)
    {
      AmlInstance *instance =
        aml_instance_new_from_json (json_array_get_element (array, i), error);

      if (instance == NULL)
        {
          g_list_free_full (list, g_object_unref);
          return NULL;
        }

      const char *id = aml_instance_get_id (instance);
      g_autofree char *instance_name_value = instance_name (instance);
      g_autofree char *instance_key =
        instance_name_key (instance_name_value);

      for (GList *l = list; l != NULL; l = l->next)
        {
          AmlInstance *seen = l->data;
          g_autofree char *seen_name = instance_name (seen);
          g_autofree char *seen_key =
            instance_name_key (seen_name);

          if (g_str_equal (aml_instance_get_id (seen), id))
            {
              g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                           AML_INSTANCE_MANAGER_ERROR_CORRUPT,
                           "instances.json lists the id \"%s\" twice", id);
              g_object_unref (instance);
              g_list_free_full (list, g_object_unref);
              return NULL;
            }

          if (g_str_equal (seen_key, instance_key))
            {
              g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                           AML_INSTANCE_MANAGER_ERROR_CORRUPT,
                           "instances.json lists the normalized name \"%s\" "
                           "more than once",
                           instance_name_value);
              g_object_unref (instance);
              g_list_free_full (list, g_object_unref);
              return NULL;
            }
        }

      list = g_list_prepend (list, instance);
    }

  return g_list_reverse (list);
}

/* Keeps an unreadable file around instead of deleting it. */
static void
quarantine_corrupt_file (const char *path)
{
  g_autofree char *base = g_strconcat (path, CORRUPT_SUFFIX, NULL);
  g_autofree char *corrupt_path = NULL;

  for (guint i = 0; i < 1000; i++)
    {
      corrupt_path = i == 0 ? g_strdup (base)
                            : g_strdup_printf ("%s.%u", base, i);
      if (!g_file_test (corrupt_path, G_FILE_TEST_EXISTS))
        break;
      g_clear_pointer (&corrupt_path, g_free);
    }

  if (corrupt_path == NULL || !aml_fs_rename (path, corrupt_path, NULL))
    g_warning ("could not rename the corrupt file to \"%s\": %s",
               corrupt_path, g_strerror (errno));
}

gboolean
aml_instance_manager_load (AmlInstanceManager *self, GError **error)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_autofree char *contents = NULL;
  gsize length = 0;
  g_autoptr (GError) local_error = NULL;

  if ((g_file_test (self->metadata_path, G_FILE_TEST_EXISTS) &&
       !aml_fs_file_size_within (self->metadata_path,
                                 AML_FS_MAX_INSTANCE_METADATA_SIZE,
                                 &local_error)) ||
      !g_file_get_contents (self->metadata_path, &contents, &length,
                            &local_error))
    {
      /* First run: there simply is nothing to load yet. */
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_list_store_remove_all (self->instances);
          return TRUE;
        }

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  GList *list = parse_instances_json (contents, length, &local_error);

  /* A failure is signalled by the error, not by the list: a file with
   * an empty "instances" array parses to a NULL list just fine. */
  if (local_error != NULL)
    {
      /* Never delete what we do not understand: keep it as
       * instances.json.corrupt and start empty. */
      quarantine_corrupt_file (self->metadata_path);
      g_list_store_remove_all (self->instances);

      g_prefix_error (&local_error, "instances.json is unusable: ");
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_list_store_remove_all (self->instances);

  for (GList *l = list; l != NULL; l = l->next)
    g_list_store_append (self->instances, l->data);

  g_list_free_full (list, g_object_unref);
  return TRUE;
}

AmlInstance *
aml_instance_manager_create (AmlInstanceManager *self,
                             const char *name,
                             const char *directory,
                             const char *minecraft_version,
                             AmlLoader loader,
                             const char *loader_version,
                             GError **error)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  g_autofree char *id = g_uuid_string_random ();
  g_autofree char *path = NULL;
  g_autofree char *normalized_name = aml_instance_normalize_name (name);

  if (normalized_name[0] == '\0' || name_is_taken (self, NULL, normalized_name))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "an instance with that name already exists or the name is empty");
      return NULL;
    }

  if (directory != NULL)
    {
      if (!check_instance_directory (self, directory, NULL, &path, error))
        return NULL;
    }
  else
    {
      g_autofree char *derived =
        default_instance_directory (self, name, id);

      if (!check_instance_directory (self, derived, NULL, &path, error))
        return NULL;
    }

  g_autoptr (AmlInstance) instance =
     aml_instance_new (id, normalized_name, path, minecraft_version, loader,
                       error);

  if (instance == NULL)
    return NULL;

  if (loader_version != NULL)
    g_object_set (instance, "loader_version", loader_version, NULL);

  g_autoptr (GError) path_error = NULL;
  if (!g_file_test (path, G_FILE_TEST_IS_DIR) &&
      !aml_fs_make_directory (path, 0755, &path_error))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_IO,
                    "cannot create \"%s\": %s", path,
                    path_error->message);
      return NULL;
    }

  g_list_store_append (self->instances, instance);

  if (!aml_instance_manager_save (self, error))
    {
      /* Nothing on disk lists the instance, so nothing lists it here. */
      g_list_store_remove (self->instances,
                           g_list_model_get_n_items (G_LIST_MODEL (self->instances)) - 1);
      return NULL;
    }

  return g_steal_pointer (&instance);
}

gboolean
aml_instance_manager_rename (AmlInstanceManager *self,
                             AmlInstance *instance,
                             const char *new_name,
                             GError **error)
{
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  g_autofree char *old_name = NULL;
  g_object_get (instance, "name", &old_name, NULL);

  g_autofree char *stripped = aml_instance_normalize_name (new_name);

  if (stripped[0] == '\0' || name_is_taken (self, instance, stripped))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "an instance with that name already exists or the name is empty");
      return FALSE;
    }

  g_autoptr (GError) local_error = NULL;

  if (!aml_instance_set_name (instance, stripped, &local_error))
    {
      aml_instance_set_name (instance, old_name, NULL);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (!aml_instance_manager_save (self, error))
    {
      aml_instance_set_name (instance, old_name, NULL);
      return FALSE;
    }

  return TRUE;
}

gboolean
aml_instance_manager_set_string (AmlInstanceManager *self,
                                 AmlInstance *instance,
                                 const char *property,
                                 const char *value,
                                 GError **error)
{
  static const char *allowed[] = {
    "minecraft-version", "loader-version", "icon", "java-path", "jvm-args",
    NULL
  };
  g_autofree char *old = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (property != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error_literal (error, AML_INSTANCE_MANAGER_ERROR,
                           AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                           "the instance is not registered");
      return FALSE;
    }

  gboolean known = FALSE;
  for (guint i = 0; allowed[i] != NULL; i++)
    known |= g_str_equal (property, allowed[i]);
  if (!known)
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "instance property \"%s\" cannot be edited here", property);
      return FALSE;
    }

  g_object_get (instance, property, &old, NULL);
  if (!aml_instance_set_string (instance, property, value, error) ||
      !aml_instance_manager_save (self, error))
    {
      g_object_set (instance, property, old, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
aml_instance_manager_set_loader (AmlInstanceManager *self,
                                 AmlInstance *instance,
                                 AmlLoader loader,
                                 const char *version,
                                 GError **error)
{
  AmlLoader old_loader = AML_LOADER_VANILLA;
  g_autofree char *old_version = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error_literal (error, AML_INSTANCE_MANAGER_ERROR,
                           AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                           "the instance is not registered");
      return FALSE;
    }

  g_object_get (instance, "loader", &old_loader, "loader-version", &old_version,
                NULL);
  if (!aml_instance_set_loader (instance, loader, version, error) ||
      !aml_instance_manager_save (self, error))
    {
      aml_instance_set_loader (instance, old_loader, old_version, NULL);
      return FALSE;
    }
  return TRUE;
}

static gboolean
remove_instance_and_save (AmlInstanceManager *self,
                          AmlInstance *instance,
                          GError **error)
{
  guint position = 0;

  if (!g_list_store_find (self->instances, instance, &position))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  g_list_store_remove (self->instances, position);

  if (!aml_instance_manager_save (self, error))
    {
      g_list_store_insert (self->instances, position, instance);
      return FALSE;
    }

  return TRUE;
}

static void
copy_tree_async (GFile *source, GFile *destination,
                 GCancellable *cancellable,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  aml_fs_copy_tree_async (source, destination, cancellable, callback,
                          user_data);
}

static gboolean
copy_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Duplicate
 * ---------------------------------------------------------- */

typedef struct
{
  AmlInstanceManager *self;
  GTask *task;
  AmlTaskScope *scope;
  AmlInstance *copy; /* built, not registered yet */
} DuplicateData;

static void
duplicate_data_free (DuplicateData *data)
{
  g_clear_object (&data->task);
  aml_task_scope_free (data->scope);
  g_clear_object (&data->self);
  g_clear_object (&data->copy);
  g_free (data);
}

static void
duplicate_data_return_error (DuplicateData *data, GError *error)
{
  g_task_return_error (data->task, error);
  duplicate_data_free (data);
}

static void
duplicate_copy_ready (GObject *source G_GNUC_UNUSED,
                      GAsyncResult *result, gpointer user_data)
{
  DuplicateData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!copy_tree_finish (result, &error))
    {
      /* Nothing was registered: the copy stays a plain directory. */
      duplicate_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  g_list_store_append (data->self->instances, data->copy);

  if (!aml_instance_manager_save (data->self, &error))
    {
      guint position = 0;

      /* The file on disk never knew about the copy. */
      if (g_list_store_find (data->self->instances, data->copy, &position))
        g_list_store_remove (data->self->instances, position);

      duplicate_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (data->task, data->copy, g_object_unref);
  data->copy = NULL; /* the task hands it to the finish call */
  duplicate_data_free (data);
}

void
aml_instance_manager_duplicate_async (AmlInstanceManager *self,
                                      AmlInstance *source_instance,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  g_return_if_fail (AML_IS_INSTANCE_MANAGER (self));
  g_return_if_fail (AML_IS_INSTANCE (source_instance));

  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  if (!g_list_store_find (self->instances, source_instance, NULL))
    {
      g_task_return_new_error (task, AML_INSTANCE_MANAGER_ERROR,
                               AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                               "the instance is not registered");
      return;
    }

  g_autofree char *source_name = instance_name (source_instance);
  g_autofree char *source_directory = instance_directory (source_instance);
  g_autofree char *minecraft_version = NULL;
  g_autofree char *loader_version = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *java_path = NULL;
  g_autofree char *jvm_args = NULL;
  AmlLoader loader = AML_LOADER_VANILLA;

  g_object_get (source_instance,
                "minecraft_version", &minecraft_version,
                "loader", &loader,
                "loader_version", &loader_version,
                "icon", &icon,
                "java_path", &java_path,
                "jvm_args", &jvm_args,
                NULL);

  g_autofree char *id = g_uuid_string_random ();
   g_autofree char *name = g_strdup_printf (_("Copy of %s"), source_name);
  g_autofree char *directory = default_instance_directory (self, name, id);
  g_autofree char *canonical = NULL;
  g_autoptr (GError) local_error = NULL;

  if (!check_instance_directory (self, directory, NULL, &canonical, &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  g_free (directory);
  directory = g_steal_pointer (&canonical);

  g_autoptr (AmlInstance) copy =
    aml_instance_new (id, name, directory, minecraft_version, loader,
                      &local_error);

  if (copy == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  g_object_set (copy,
                "loader_version", loader_version,
                "icon", icon,
                "java_path", java_path,
                "jvm_args", jvm_args,
                NULL);

  g_autoptr (GError) instances_error = NULL;
  if (!aml_fs_make_directory (self->instances_dir, 0755,
                              &instances_error))
    {
      g_task_return_new_error (task, AML_INSTANCE_MANAGER_ERROR,
                               AML_INSTANCE_MANAGER_ERROR_IO,
                               "cannot create \"%s\": %s",
                               self->instances_dir, instances_error->message);
      return;
    }

  DuplicateData *data = g_new0 (DuplicateData, 1);
  data->self = g_object_ref (self);
  data->task = g_object_ref (task);
  data->scope = aml_task_scope_borrow_task (task);
  data->copy = g_steal_pointer (&copy);

  g_autoptr (GFile) source_file = g_file_new_for_path (source_directory);
  g_autoptr (GFile) copy_file = g_file_new_for_path (directory);

  copy_tree_async (source_file, copy_file, cancellable,
                   duplicate_copy_ready, data);
}

AmlInstance *
aml_instance_manager_duplicate_finish (AmlInstanceManager *self,
                                       GAsyncResult *result,
                                       GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
delete_tree_async (GFile *file, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  g_autofree char *path = g_file_get_path (file);

  aml_fs_remove_tree_async (path, cancellable, callback, user_data);
}

static gboolean
delete_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Delete
 * ---------------------------------------------------------- */

typedef struct
{
  AmlInstanceManager *self;
  GTask *task;
  AmlTaskScope *scope;
  AmlInstance *instance;
} DeleteData;

static void
delete_data_free (DeleteData *data)
{
  g_clear_object (&data->task);
  aml_task_scope_free (data->scope);
  g_clear_object (&data->self);
  g_clear_object (&data->instance);
  g_free (data);
}

static void
delete_data_return_error (DeleteData *data, GError *error)
{
  g_task_return_error (data->task, error);
  delete_data_free (data);
}

static void
delete_directory_ready (GObject *source G_GNUC_UNUSED,
                        GAsyncResult *result, gpointer user_data)
{
  DeleteData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!delete_tree_finish (result, &error))
    {
      /* The directory is still there, so keep the instance. */
      delete_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  if (!remove_instance_and_save (data->self, data->instance, &error))
    {
      delete_data_return_error (data, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (data->task, TRUE);
  delete_data_free (data);
}

void
aml_instance_manager_delete_async (AmlInstanceManager *self,
                                   AmlInstance *instance,
                                   gboolean delete_directory,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_return_if_fail (AML_IS_INSTANCE_MANAGER (self));
  g_return_if_fail (AML_IS_INSTANCE (instance));

  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_task_return_new_error (task, AML_INSTANCE_MANAGER_ERROR,
                               AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                               "the instance is not registered");
      return;
    }

  if (!delete_directory)
    {
      g_autoptr (GError) error = NULL;

      if (!remove_instance_and_save (self, instance, &error))
        g_task_return_error (task, g_steal_pointer (&error));
      else
        g_task_return_boolean (task, TRUE);

      return;
    }

  g_autofree char *directory = instance_directory (instance);
  g_autoptr (GFile) file = g_file_new_for_path (directory);

  DeleteData *data = g_new0 (DeleteData, 1);
  data->self = g_object_ref (self);
  data->task = g_object_ref (task);
   data->scope = aml_task_scope_borrow_task (task);
  data->instance = g_object_ref (instance);

  delete_tree_async (file, cancellable, delete_directory_ready, data);
}

gboolean
aml_instance_manager_delete_finish (AmlInstanceManager *self,
                                    GAsyncResult *result,
                                    GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Change directory
 *
 * The files are copied to the new place first and the metadata is
 * written only once they are all there, so a move that fails leaves the
 * old directory and the stored path exactly as they were. The old copy is
 * removed at the very end: a directory that outlives the change is a
 * leftover, while a directory that went before the metadata did would be
 * files with nothing pointing at them.
 * ---------------------------------------------------------- */

gboolean
aml_instance_manager_check_new_directory (AmlInstanceManager  *self,
                                          AmlInstance         *instance,
                                          const char          *directory,
                                          char               **canonical,
                                          GError             **error)
{
  g_autofree char *path = NULL;
  g_autofree char *current = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (AML_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (canonical == NULL || *canonical == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  if (!check_instance_directory (self, directory, instance, &path, error))
    return FALSE;

  g_object_get (instance, "directory", &current, NULL);

  /* The directory it already has is not a change, and asking for it
     would move a directory onto itself. */
  if (g_strcmp0 (path, current) == 0)
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the directory is already \"%s\"", path);
      return FALSE;
    }

  /* An existing directory is not touched: something else put it there
     and the files of this instance must not land on top of it. */
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "\"%s\" already exists", path);
      return FALSE;
    }

  /* The directory the new one hangs off has to be there already. Left to
     find out during the copy, this would be reported as a file that
     cannot be written, which says nothing about why. */
  {
    g_autofree char *parent = g_path_get_dirname (path);

    if (!g_file_test (parent, G_FILE_TEST_IS_DIR))
      {
        g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                     AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                     "the directory \"%s\" does not exist", parent);
        return FALSE;
      }
  }

  if (canonical != NULL)
    *canonical = g_steal_pointer (&path);

  return TRUE;
}

typedef struct
{
  AmlInstanceManager *self;
  GTask *task;
  AmlTaskScope *scope;
  AmlInstance *instance;

  char *old_directory;
  char *new_directory;
  GFile *destination;
  GError *undo_error; /* the failure the undo is cleaning up after */
} MoveData;

static void
move_data_free (MoveData *data)
{
  g_clear_object (&data->task);
  aml_task_scope_free (data->scope);
  g_clear_object (&data->self);
  g_clear_object (&data->instance);
  g_clear_object (&data->destination);
  g_free (data->old_directory);
  g_free (data->new_directory);
  g_clear_error (&data->undo_error);
  g_free (data);
}

static void
move_return_error (MoveData *data, GError *error)
{
  g_task_return_error (data->task, error);
  move_data_free (data);
}

/* Puts @instance back to the directory it had and drops whatever landed
 * in the new one. Nothing about the old directory is touched: it was
 * never modified. The destination may hold a partial copy, so it is
 * removed recursively, and @error is returned once that is done. */
static void
move_undo_deleted (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                   gpointer user_data)
{
  MoveData *data = user_data;
  g_autoptr (GError) ignored = NULL;

  /* Best effort: the error that caused the undo is what the caller
     needs to hear, not a failure to clean up after it. */
  delete_tree_finish (result, &ignored);

  move_return_error (data, g_steal_pointer (&data->undo_error));
}

static void
move_undo (MoveData *data, gboolean restore_instance, GError *error)
{
  if (restore_instance)
    aml_instance_set_directory (data->instance, data->old_directory, NULL);

  data->undo_error = error;

  if (data->destination != NULL)
    delete_tree_async (data->destination, NULL, move_undo_deleted, data);
  else
    move_return_error (data, g_steal_pointer (&data->undo_error));
}

/* The move has already happened and been saved. What is left is a
 * directory that now holds a second copy of files that exist somewhere
 * else; failing to remove it leaves files behind rather than taking any
 * away, so the change stands and the leftover is said out loud. */
static void
move_old_removed (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  MoveData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!delete_tree_finish (result, &error))
    g_warning ("the old directory \"%s\" could not be removed: %s",
               data->old_directory, error->message);

  g_task_return_boolean (data->task, TRUE);
  move_data_free (data);
}

static void
move_copy_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  MoveData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!copy_tree_finish (result, &error))
    {
      move_undo (data, FALSE, g_steal_pointer (&error));
      return;
    }

  /* The files are all in the new place; only now does the stored path
     change, and the file that records it is written whole. */
    aml_instance_set_directory (data->instance, data->new_directory, NULL);

  if (!aml_instance_manager_save (data->self, &error))
    {
      move_undo (data, TRUE, g_steal_pointer (&error));
      return;
    }

  /* The old directory is now a second copy of a directory that exists.
     Taking it away is the last thing done. */
  {
    g_autoptr (GFile) old = g_file_new_for_path (data->old_directory);

    delete_tree_async (old, NULL, move_old_removed, data);
  }
}

void
aml_instance_manager_change_directory_async (AmlInstanceManager *self,
                                             AmlInstance *instance,
                                             const char *directory,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data)
{
  g_return_if_fail (AML_IS_INSTANCE_MANAGER (self));
  g_return_if_fail (AML_IS_INSTANCE (instance));

  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_autofree char *path = NULL;
  g_autofree char *old_directory = NULL;
  g_autoptr (GError) local_error = NULL;

  if (!aml_instance_manager_check_new_directory (self, instance, directory,
                                                 &path, &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  g_object_get (instance, "directory", &old_directory, NULL);

  MoveData *data = g_new0 (MoveData, 1);
  data->self = g_object_ref (self);
  data->task = g_steal_pointer (&task);
  data->scope = aml_task_scope_borrow_task (data->task);
  data->instance = g_object_ref (instance);
  data->old_directory = g_steal_pointer (&old_directory);
  data->new_directory = g_strdup (path);
  data->destination = g_file_new_for_path (path);

  /* An instance whose directory is gone has nothing to move: the path is
     changed and saved, which is all that is left to do. */
  if (!g_file_test (data->old_directory, G_FILE_TEST_IS_DIR))
    {
      aml_instance_set_directory (instance, data->new_directory, NULL);

      if (!aml_instance_manager_save (self, &local_error))
        {
          aml_instance_set_directory (instance, data->old_directory, NULL);
          move_return_error (data, g_steal_pointer (&local_error));
          return;
        }

      g_task_return_boolean (data->task, TRUE);
      move_data_free (data);
      return;
    }

  {
    g_autoptr (GFile) source = g_file_new_for_path (data->old_directory);

    copy_tree_async (source, data->destination, cancellable, move_copy_ready,
                     data);
  }
}

gboolean
aml_instance_manager_change_directory_finish (AmlInstanceManager *self,
                                               GAsyncResult *result,
                                               GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Open folder
 * ---------------------------------------------------------- */

static void
open_folder_launch_ready (GObject *source G_GNUC_UNUSED,
                          GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data; /* holds its own reference */
  g_autoptr (GError) error = NULL;

  if (g_app_info_launch_default_for_uri_finish (result, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, g_steal_pointer (&error));

  g_object_unref (task);
}

void
aml_instance_manager_open_folder_async (AmlInstanceManager *self,
                                        AmlInstance *instance,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
  g_return_if_fail (AML_IS_INSTANCE_MANAGER (self));
  g_return_if_fail (AML_IS_INSTANCE (instance));

  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data_full (G_OBJECT (task), "aml-task-scope",
                          aml_task_scope_borrow_task (task),
                          (GDestroyNotify) aml_task_scope_free);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_task_return_new_error (task, AML_INSTANCE_MANAGER_ERROR,
                               AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                               "the instance is not registered");
      return;
    }

  g_autofree char *directory = instance_directory (instance);

  if (!g_file_test (directory, G_FILE_TEST_IS_DIR))
    {
      g_task_return_new_error (task, AML_INSTANCE_MANAGER_ERROR,
                               AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                               "the directory \"%s\" does not exist",
                               directory);
      return;
    }

  g_autoptr (GError) error = NULL;
  g_autofree char *uri = g_filename_to_uri (directory, NULL, &error);

  if (uri == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_app_info_launch_default_for_uri_async (uri, NULL, cancellable,
                                           open_folder_launch_ready,
                                           g_object_ref (task));
}

gboolean
aml_instance_manager_open_folder_finish (AmlInstanceManager *self,
                                         GAsyncResult *result,
                                         GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
