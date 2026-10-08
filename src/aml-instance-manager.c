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

  if (g_mkdir_with_parents (self->data_dir, 0700) != 0)
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_IO,
                   "cannot create \"%s\": %s",
                   self->data_dir, g_strerror (errno));
      return FALSE;
    }

  g_autoptr (GError) local_error = NULL;

  /* The data goes to a temporary file in the same directory, is flushed
   * and fsynced and then renamed over instances.json, so a save that
   * fails or is interrupted can never leave a half written file. */
  if (!g_file_set_contents_full (self->metadata_path, json, -1,
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_DURABLE,
                                 0600, &local_error))
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

      for (GList *l = list; l != NULL; l = l->next)
        {
          AmlInstance *seen = l->data;

          if (g_str_equal (aml_instance_get_id (seen), id))
            {
              g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                           AML_INSTANCE_MANAGER_ERROR_CORRUPT,
                           "instances.json lists the id \"%s\" twice", id);
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
  g_autofree char *corrupt_path =
    g_strconcat (path, CORRUPT_SUFFIX, NULL);

  if (g_rename (path, corrupt_path) != 0)
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

  if (!g_file_get_contents (self->metadata_path, &contents, &length,
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
    aml_instance_new (id, name, path, minecraft_version, loader, error);

  if (instance == NULL)
    return NULL;

  if (loader_version != NULL)
    g_object_set (instance, "loader_version", loader_version, NULL);

  if (!g_file_test (path, G_FILE_TEST_IS_DIR) &&
      g_mkdir_with_parents (path, 0755) != 0)
    {
      g_set_error (error, AML_INSTANCE_MANAGER_ERROR,
                   AML_INSTANCE_MANAGER_ERROR_IO,
                   "cannot create \"%s\": %s", path, g_strerror (errno));
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

  g_autofree char *stripped =
    g_strdup (new_name != NULL ? new_name : "");
  g_strstrip (stripped);

  g_object_set (instance, "name", stripped, NULL);

  g_autoptr (GError) local_error = NULL;

  if (!aml_instance_validate (instance, &local_error))
    {
      g_object_set (instance, "name", old_name, NULL);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (!aml_instance_manager_save (self, error))
    {
      g_object_set (instance, "name", old_name, NULL);
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

/* ---------------------------------------------------------- *
 * Asynchronous recursive copy
 *
 * GIO has no "copy directory tree" call, so the tree is walked with
 * g_file_enumerate_children_async() and every entry starts its own
 * async operation. CopyTree counts the operations still in flight and
 * completes its GTask when the count reaches zero. All callbacks run
 * in the main context, so no locking is needed.
 * ---------------------------------------------------------- */

typedef struct
{
  GTask        *task;
  guint         pending;     /* operations still in flight */
  GError       *error;       /* first error, if any */
  GCancellable *cancellable; /* borrowed; the task keeps a reference */
  GFile        *source;
  GFile        *destination;
} CopyTree;

typedef struct
{
  CopyTree        *tree;
  GFile           *source;
  GFile           *destination;
  GFileEnumerator *enumerator;
} CopyDir;

typedef struct
{
  CopyTree *tree;
  GFile    *source;
  GFile    *destination;
} CopyFile;

typedef struct
{
  CopyTree *tree;
  GFile    *destination;
  char     *target;
} CopyLink;

static void copy_tree_start_dir (CopyTree *tree, GFile *source,
                                 GFile *destination);
static void copy_tree_start_file (CopyTree *tree, GFile *source,
                                  GFile *destination);
static void copy_tree_start_link (CopyTree *tree, GFile *destination,
                                  const char *target);
static void make_directory_ready (GObject *source, GAsyncResult *result,
                                  gpointer user_data);
static void enumerate_ready (GObject *source, GAsyncResult *result,
                             gpointer user_data);
static void next_files_ready (GObject *source, GAsyncResult *result,
                              gpointer user_data);
static void enumerator_close_ready (GObject *source, GAsyncResult *result,
                                    gpointer user_data);
static void file_copy_ready (GObject *source, GAsyncResult *result,
                             gpointer user_data);
static void link_copy_ready (GObject *source, GAsyncResult *result,
                             gpointer user_data);
static void copy_tree_query_ready (GObject *source, GAsyncResult *result,
                                   gpointer user_data);

static void
copy_tree_record_error (CopyTree *tree, GError *error)
{
  if (tree->error == NULL)
    tree->error = error;
  else
    g_error_free (error);
}

static void
copy_tree_done (CopyTree *tree)
{
  g_assert (tree->pending > 0);

  if (--tree->pending > 0)
    return;

  if (tree->error != NULL)
    g_task_return_error (tree->task, g_steal_pointer (&tree->error));
  else
    g_task_return_boolean (tree->task, TRUE);

  g_clear_object (&tree->task);
  g_clear_object (&tree->source);
  g_clear_object (&tree->destination);
  g_free (tree);
}

static void
copy_dir_free (CopyDir *dir)
{
  g_clear_object (&dir->enumerator);
  g_clear_object (&dir->source);
  g_clear_object (&dir->destination);
  g_free (dir);
}

static void
copy_dir_finished (CopyDir *dir)
{
  CopyTree *tree = dir->tree;

  copy_dir_free (dir);
  copy_tree_done (tree);
}

static void
copy_tree_start_dir (CopyTree *tree, GFile *source, GFile *destination)
{
  CopyDir *dir = g_new0 (CopyDir, 1);

  dir->tree = tree;
  dir->source = source;
  dir->destination = destination;

  tree->pending++;
  g_file_make_directory_async (destination, G_PRIORITY_DEFAULT,
                               tree->cancellable,
                               make_directory_ready, dir);
}

static void
copy_tree_start_file (CopyTree *tree, GFile *source, GFile *destination)
{
  CopyFile *file = g_new0 (CopyFile, 1);

  file->tree = tree;
  file->source = source;
  file->destination = destination;

  tree->pending++;
  g_file_copy_async (source, destination, G_FILE_COPY_NONE,
                     G_PRIORITY_DEFAULT, tree->cancellable,
                     NULL, NULL, file_copy_ready, file);
}

static void
copy_tree_start_link (CopyTree *tree, GFile *destination, const char *target)
{
  if (target == NULL)
    {
      /* Without a target there is nothing to recreate. */
      g_object_unref (destination);
      return;
    }

  CopyLink *link = g_new0 (CopyLink, 1);

  link->tree = tree;
  link->destination = destination;
  link->target = g_strdup (target);

  tree->pending++;
  g_file_make_symbolic_link_async (destination, target, G_PRIORITY_DEFAULT,
                                   tree->cancellable,
                                   link_copy_ready, link);
}

static void
make_directory_ready (GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
  CopyDir *dir = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_make_directory_finish (G_FILE (source), result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        g_clear_error (&error);
      else
        {
          copy_tree_record_error (dir->tree, g_steal_pointer (&error));
          copy_dir_finished (dir);
          return;
        }
    }

  g_file_enumerate_children_async (dir->source,
                                   G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                   G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                   G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   G_PRIORITY_DEFAULT,
                                   dir->tree->cancellable,
                                   enumerate_ready, dir);
}

static void
enumerate_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CopyDir *dir = user_data;
  g_autoptr (GError) error = NULL;

  dir->enumerator =
    g_file_enumerate_children_finish (G_FILE (source), result, &error);

  if (dir->enumerator == NULL)
    {
      copy_tree_record_error (dir->tree, g_steal_pointer (&error));
      copy_dir_finished (dir);
      return;
    }

  g_file_enumerator_next_files_async (dir->enumerator, 100,
                                      G_PRIORITY_DEFAULT,
                                      dir->tree->cancellable,
                                      next_files_ready, dir);
}

static void
enumerator_close_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  CopyDir *dir = user_data;

  /* Closing after everything was read is best effort. */
  g_file_enumerator_close_finish (G_FILE_ENUMERATOR (source), result, NULL);
  copy_dir_finished (dir);
}

static void
next_files_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  CopyDir *dir = user_data;
  g_autoptr (GError) error = NULL;
  GList *files =
    g_file_enumerator_next_files_finish (dir->enumerator, result, &error);
  GList *l;

  if (error != NULL)
    copy_tree_record_error (dir->tree, g_steal_pointer (&error));

  if (files == NULL)
    {
      /* End of the directory, or a failure. This directory is done; its
       * children keep their own slots in the counter. */
      g_file_enumerator_close_async (dir->enumerator, G_PRIORITY_DEFAULT,
                                     dir->tree->cancellable,
                                     enumerator_close_ready, dir);
      return;
    }

  for (l = files; l != NULL; l = l->next)
    {
      GFileInfo *info = l->data;
      const char *name = g_file_info_get_name (info);
      GFile *child_source = g_file_get_child (dir->source, name);
      GFile *child_destination = g_file_get_child (dir->destination, name);

      switch (g_file_info_get_file_type (info))
        {
        case G_FILE_TYPE_DIRECTORY:
          copy_tree_start_dir (dir->tree, child_source, child_destination);
          break;

        case G_FILE_TYPE_REGULAR:
          copy_tree_start_file (dir->tree, child_source, child_destination);
          break;

        case G_FILE_TYPE_SYMBOLIC_LINK:
          copy_tree_start_link (dir->tree, child_destination,
                                g_file_info_get_symlink_target (info));
          g_object_unref (child_source);
          break;

        default:
          g_object_unref (child_source);
          g_object_unref (child_destination);
          break;
        }
    }

  g_list_free_full (files, g_object_unref);

  g_file_enumerator_next_files_async (dir->enumerator, 100,
                                      G_PRIORITY_DEFAULT,
                                      dir->tree->cancellable,
                                      next_files_ready, dir);
}

static void
file_copy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CopyFile *file = user_data;
  g_autoptr (GError) error = NULL;
  CopyTree *tree = file->tree;

  if (!g_file_copy_finish (G_FILE (source), result, &error))
    copy_tree_record_error (tree, g_steal_pointer (&error));

  g_clear_object (&file->source);
  g_clear_object (&file->destination);
  g_free (file);

  copy_tree_done (tree);
}

static void
link_copy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CopyLink *link = user_data;
  g_autoptr (GError) error = NULL;
  CopyTree *tree = link->tree;

  if (!g_file_make_symbolic_link_finish (G_FILE (source), result, &error))
    copy_tree_record_error (tree, g_steal_pointer (&error));

  g_clear_object (&link->destination);
  g_free (link->target);
  g_free (link);

  copy_tree_done (tree);
}

static void
copy_tree_query_ready (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  CopyTree *tree = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInfo) info =
    g_file_query_info_finish (G_FILE (source), result, &error);

  if (info == NULL)
    {
      copy_tree_record_error (tree, g_steal_pointer (&error));
      copy_tree_done (tree);
      return;
    }

  switch (g_file_info_get_file_type (info))
    {
    case G_FILE_TYPE_DIRECTORY:
      copy_tree_start_dir (tree, g_object_ref (tree->source),
                           g_object_ref (tree->destination));
      break;

    case G_FILE_TYPE_REGULAR:
      copy_tree_start_file (tree, g_object_ref (tree->source),
                            g_object_ref (tree->destination));
      break;

    case G_FILE_TYPE_SYMBOLIC_LINK:
      copy_tree_start_link (tree, g_object_ref (tree->destination),
                            g_file_info_get_symlink_target (info));
      break;

    default:
      g_autofree char *path = g_file_get_path (tree->source);
      copy_tree_record_error (tree,
                              g_error_new (AML_INSTANCE_MANAGER_ERROR,
                                           AML_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                                           "\"%s\" is neither a directory "
                                           "nor a regular file",
                                           path));
      break;
    }

  copy_tree_done (tree);
}

static void
copy_tree_async (GFile *source, GFile *destination,
                 GCancellable *cancellable,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  CopyTree *tree = g_new0 (CopyTree, 1);

  tree->task = g_task_new (NULL, cancellable, callback, user_data);
  tree->cancellable = cancellable; /* the task keeps a reference */
  tree->pending = 1;
  tree->source = g_object_ref (source);
  tree->destination = g_object_ref (destination);

  g_file_query_info_async (source,
                           G_FILE_ATTRIBUTE_STANDARD_NAME ","
                           G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                           G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                           G_PRIORITY_DEFAULT, cancellable,
                           copy_tree_query_ready, tree);
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
  AmlInstance *copy; /* built, not registered yet */
} DuplicateData;

static void
duplicate_data_free (DuplicateData *data)
{
  g_clear_object (&data->task);
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
  g_autofree char *name = g_strdup_printf ("Copy of %s", source_name);
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

  if (g_mkdir_with_parents (self->instances_dir, 0755) != 0)
    {
      g_task_return_new_error (task, AML_INSTANCE_MANAGER_ERROR,
                               AML_INSTANCE_MANAGER_ERROR_IO,
                               "cannot create \"%s\": %s",
                               self->instances_dir, g_strerror (errno));
      return;
    }

  DuplicateData *data = g_new0 (DuplicateData, 1);
  data->self = g_object_ref (self);
  data->task = g_object_ref (task);
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

/* ---------------------------------------------------------- *
 * Asynchronous recursive delete
 *
 * Same idea as the copy: a directory is only removed once all of its
 * children are gone, and the tree completes when the last pending
 * operation reports back.
 * ---------------------------------------------------------- */

typedef struct
{
  GTask        *task;
  guint         pending;
  GError       *error;
  GCancellable *cancellable;
  GFile        *file;
} DeleteTree;

typedef struct _DeleteNode
{
  DeleteTree         *tree;
  struct _DeleteNode *parent;
  GFile              *file;
  GFileEnumerator    *enumerator;
  guint children;      /* children still being removed */
  gboolean enumerated; /* no more children will show up */
  gboolean closed;     /* the enumerator is closed */
  gboolean started;    /* the node itself is being removed */
} DeleteNode;

static void delete_tree_start_node (DeleteTree *tree, DeleteNode *parent,
                                    GFile *file, GFileType type);
static void delete_node_maybe_delete_self (DeleteNode *node);
static void delete_self_ready (GObject *source, GAsyncResult *result,
                               gpointer user_data);
static void delete_enumerate_ready (GObject *source, GAsyncResult *result,
                                    gpointer user_data);
static void delete_next_files_ready (GObject *source, GAsyncResult *result,
                                     gpointer user_data);
static void delete_enumerator_close_ready (GObject *source,
                                           GAsyncResult *result,
                                           gpointer user_data);
static void delete_tree_query_ready (GObject *source, GAsyncResult *result,
                                     gpointer user_data);

static void
delete_tree_record_error (DeleteTree *tree, GError *error)
{
  if (tree->error == NULL)
    tree->error = error;
  else
    g_error_free (error);
}

static void
delete_tree_done (DeleteTree *tree)
{
  g_assert (tree->pending > 0);

  if (--tree->pending > 0)
    return;

  if (tree->error != NULL)
    g_task_return_error (tree->task, g_steal_pointer (&tree->error));
  else
    g_task_return_boolean (tree->task, TRUE);

  g_clear_object (&tree->task);
  g_clear_object (&tree->file);
  g_free (tree);
}

static void
delete_node_free (DeleteNode *node)
{
  g_clear_object (&node->enumerator);
  g_clear_object (&node->file);
  g_free (node);
}

static void
delete_node_finished (DeleteNode *node)
{
  DeleteTree *tree = node->tree;
  DeleteNode *parent = node->parent;

  delete_node_free (node);

  if (parent != NULL)
    {
      parent->children--;
      delete_node_maybe_delete_self (parent);
    }

  delete_tree_done (tree);
}

static void
delete_node_maybe_delete_self (DeleteNode *node)
{
  if (node->enumerated && node->closed && node->children == 0 &&
      !node->started)
    {
      node->started = TRUE;
      g_file_delete_async (node->file, G_PRIORITY_DEFAULT,
                           node->tree->cancellable,
                           delete_self_ready, node);
    }
}

static void
delete_tree_start_node (DeleteTree *tree, DeleteNode *parent,
                        GFile *file, GFileType type)
{
  DeleteNode *node = g_new0 (DeleteNode, 1);

  node->tree = tree;
  node->parent = parent;
  node->file = file; /* takes the reference */
  tree->pending++;

  if (parent != NULL)
    parent->children++;

  if (type == G_FILE_TYPE_DIRECTORY)
    {
      g_file_enumerate_children_async (node->file,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                       G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       G_PRIORITY_DEFAULT,
                                       tree->cancellable,
                                       delete_enumerate_ready, node);
      return;
    }

  /* Everything else, symlinks included, is simply removed:
   * g_file_delete never follows a symlink, so its target is safe. */
  node->enumerated = TRUE;
  node->closed = TRUE;
  node->started = TRUE;
  g_file_delete_async (node->file, G_PRIORITY_DEFAULT, tree->cancellable,
                       delete_self_ready, node);
}

static void
delete_self_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  DeleteNode *node = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_delete_finish (G_FILE (source), result, &error))
    {
      /* A directory that vanished on its own is fine. */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_clear_error (&error);
      else
        delete_tree_record_error (node->tree, g_steal_pointer (&error));
    }

  delete_node_finished (node);
}

static void
delete_enumerate_ready (GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
  DeleteNode *node = user_data;
  g_autoptr (GError) error = NULL;

  node->enumerator =
    g_file_enumerate_children_finish (G_FILE (source), result, &error);

  if (node->enumerator == NULL)
    {
      delete_tree_record_error (node->tree, g_steal_pointer (&error));
      delete_node_finished (node);
      return;
    }

  g_file_enumerator_next_files_async (node->enumerator, 100,
                                      G_PRIORITY_DEFAULT,
                                      node->tree->cancellable,
                                      delete_next_files_ready, node);
}

static void
delete_enumerator_close_ready (GObject *source, GAsyncResult *result,
                               gpointer user_data)
{
  DeleteNode *node = user_data;

  g_file_enumerator_close_finish (G_FILE_ENUMERATOR (source), result, NULL);
  node->closed = TRUE;
  delete_node_maybe_delete_self (node);
}

static void
delete_next_files_ready (GObject *source G_GNUC_UNUSED,
                         GAsyncResult *result, gpointer user_data)
{
  DeleteNode *node = user_data;
  g_autoptr (GError) error = NULL;
  GList *files =
    g_file_enumerator_next_files_finish (node->enumerator, result, &error);
  GList *l;

  if (error != NULL)
    delete_tree_record_error (node->tree, g_steal_pointer (&error));

  if (files == NULL)
    {
      /* End of the directory, or a failure: no more children. */
      node->enumerated = TRUE;
      g_file_enumerator_close_async (node->enumerator, G_PRIORITY_DEFAULT,
                                     node->tree->cancellable,
                                     delete_enumerator_close_ready, node);
      return;
    }

  for (l = files; l != NULL; l = l->next)
    {
      GFileInfo *info = l->data;

      delete_tree_start_node (node->tree, node,
                              g_file_get_child (node->file,
                                                g_file_info_get_name (info)),
                              g_file_info_get_file_type (info));
    }

  g_list_free_full (files, g_object_unref);

  g_file_enumerator_next_files_async (node->enumerator, 100,
                                      G_PRIORITY_DEFAULT,
                                      node->tree->cancellable,
                                      delete_next_files_ready, node);
}

static void
delete_tree_query_ready (GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
  DeleteTree *tree = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInfo) info =
    g_file_query_info_finish (G_FILE (source), result, &error);

  if (info == NULL)
    {
      /* Nothing to remove: a directory that is already gone counts as
       * removed. */
      if (error != NULL &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_clear_error (&error);
      else if (error != NULL)
        delete_tree_record_error (tree, g_steal_pointer (&error));

      delete_tree_done (tree);
      return;
    }

  delete_tree_start_node (tree, NULL, g_object_ref (tree->file),
                          g_file_info_get_file_type (info));
  delete_tree_done (tree);
}

static void
delete_tree_async (GFile *file, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  DeleteTree *tree = g_new0 (DeleteTree, 1);

  tree->task = g_task_new (NULL, cancellable, callback, user_data);
  tree->cancellable = cancellable; /* the task keeps a reference */
  tree->pending = 1;
  tree->file = g_object_ref (file);

  g_file_query_info_async (file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                           G_PRIORITY_DEFAULT, cancellable,
                           delete_tree_query_ready, tree);
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
  AmlInstance *instance;
} DeleteData;

static void
delete_data_free (DeleteData *data)
{
  g_clear_object (&data->task);
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
    g_object_set (data->instance, "directory", data->old_directory, NULL);

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
  g_object_set (data->instance, "directory", data->new_directory, NULL);

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
  data->instance = g_object_ref (instance);
  data->old_directory = g_steal_pointer (&old_directory);
  data->new_directory = g_strdup (path);
  data->destination = g_file_new_for_path (path);

  /* An instance whose directory is gone has nothing to move: the path is
     changed and saved, which is all that is left to do. */
  if (!g_file_test (data->old_directory, G_FILE_TEST_IS_DIR))
    {
      g_object_set (instance, "directory", data->new_directory, NULL);

      if (!aml_instance_manager_save (self, &local_error))
        {
          g_object_set (instance, "directory", data->old_directory, NULL);
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
