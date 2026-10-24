/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Owns the instance list and keeps instances.json synchronized with it.
 *
 * Copying, deleting, and opening directories use asynchronous GIO calls. */

/* realpath wants glibc's default feature set; nuvasha is Linux-only. */
#define _GNU_SOURCE

#include "nuvasha-instance-manager-private.h"
#include "nuvasha-i18n.h"
#include "nuvasha-fs.h"
#include "nuvasha-task-private.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static void assign_default_instance_icon (NuvashaInstance *instance,
                                          const char *directory);


/* Layout version of instances.json. */
#define INSTANCES_FORMAT_VERSION 2

#define METADATA_FILE "instances.json"
#define CORRUPT_SUFFIX ".corrupt"


G_DEFINE_TYPE (NuvashaInstanceManager, nuvasha_instance_manager, G_TYPE_OBJECT)

gboolean check_instance_directories (gpointer user_data);

GQuark
nuvasha_instance_manager_error_quark (void)
{
  return g_quark_from_static_string ("nuvasha-instance-manager-error");
}

char *
instance_directory (NuvashaInstance *instance)
{
  char *directory = NULL;

  g_object_get (instance, "directory", &directory, NULL);
  return directory;
}

char *
instance_name (NuvashaInstance *instance)
{
  char *name = NULL;

  g_object_get (instance, "name", &name, NULL);
  return name;
}

char *
instance_name_key (const char *name)
{
  g_autofree char *normalized = nuvasha_instance_normalize_name (name);
  return g_utf8_casefold (normalized, -1);
}

/* The display name is also the automatically chosen directory name. Do not
 * silently turn a name that cannot be a directory component into another
 * name; the user should fix it instead. */
static gboolean
instance_name_is_usable (const char *name)
{
  g_autofree char *safe = nuvasha_instance_sanitize_name (name);

  return g_strcmp0 (name, safe) == 0;
}

static const char *
instance_name_forbidden_character (const char *name)
{
  if (name == NULL || *name == '\0')
    return NULL;

  if (strchr (name, '/') != NULL)
    return "/";

  if (strstr (name, "..") != NULL || name[0] == '.' ||
      name[strlen (name) - 1] == '.')
    return ".";

  for (const char *p = name; *p != '\0'; p = g_utf8_next_char (p))
    if (g_unichar_iscntrl (g_utf8_get_char (p)))
      return "control characters";

  return NULL;
}

static gboolean
name_is_taken (NuvashaInstanceManager *self, NuvashaInstance *except,
               const char *name)
{
  g_autofree char *wanted = instance_name_key (name);
  guint count = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < count; i++)
    {
      g_autoptr (NuvashaInstance) other =
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

char *
default_instance_directory (NuvashaInstanceManager *self,
                            const char *name)
{
  g_autofree char *safe_name = nuvasha_instance_sanitize_name (name);

  return g_build_filename (self->instances_dir, safe_name, NULL);
}

/* Instance names are case-insensitively unique. Keep the first requested
 * name unchanged and give later instances the smallest numeric suffix that
 * is still available: test, test 1, test 2, ... . */
char *
unique_instance_name (NuvashaInstanceManager *self, const char *name)
{
  if (!name_is_taken (self, NULL, name))
    return g_strdup (name);

  for (guint suffix = 1; ; suffix++)
    {
      g_autofree char *candidate =
        g_strdup_printf ("%s %u", name, suffix);

      if (!name_is_taken (self, NULL, candidate))
        return g_steal_pointer (&candidate);
    }
}

/* Checks that @directory may become an instance directory: absolute,
 * without "..", not the filesystem root and neither overlapping nor
 * nested with the directory of an instance that already exists.
 * Returns the canonical path.
 *
 * @moved is the instance whose directory is being changed, or NULL when
 * a new one is being made: an instance never overlaps itself, so the one
 * being moved is not counted against it. */
gboolean
check_instance_directory (NuvashaInstanceManager  *self,
                          const char          *directory,
                          NuvashaInstance         *moved,
                          char               **canonical,
                          GError             **error)
{
  if (directory == NULL || !g_path_is_absolute (directory))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the instance directory must be an absolute path");
      return FALSE;
    }

  if (path_has_parent_reference (directory))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
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
  if (!nuvasha_fs_directory_is_real (parent))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the parent of the instance directory must not contain "
                   "symlinks and must already exist");
      return FALSE;
    }

  if (g_strcmp0 (path, G_DIR_SEPARATOR_S) == 0)
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the filesystem root cannot be an instance directory");
      return FALSE;
    }

  /* The data directory holds instances.json: an instance may not be the
     data directory itself, or its files would sit beside the metadata. */
  if (self->data_dir != NULL && g_strcmp0 (path, self->data_dir) == 0)
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the nuvasha data directory cannot be an instance directory");
      return FALSE;
    }

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (NuvashaInstance) other =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);
      g_autofree char *other_name = instance_name (other);
      g_autofree char *other_directory = instance_directory (other);

      if (other == moved)
        continue;

      if (path_contains (path, other_directory) ||
          path_contains (other_directory, path))
        {
          if (g_strcmp0 (path, other_directory) == 0)
            g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                         nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                         "the directory is already used by instance \"%s\"",
                         other_name);
          else
            g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                         nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
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
nuvasha_instance_manager_finalize (GObject *object)
{
  NuvashaInstanceManager *self = nuvasha_INSTANCE_MANAGER (object);

  if (self->directory_check_source != 0)
    g_source_remove (self->directory_check_source);
  g_clear_object (&self->instances);
  g_free (self->data_dir);
  g_free (self->metadata_path);
  g_free (self->instances_dir);

  G_OBJECT_CLASS (nuvasha_instance_manager_parent_class)->finalize (object);
}

static void
nuvasha_instance_manager_class_init (NuvashaInstanceManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nuvasha_instance_manager_finalize;
}

static void
nuvasha_instance_manager_init (NuvashaInstanceManager *self)
{
  self->instances = g_list_store_new (nuvasha_TYPE_INSTANCE);
  self->directory_check_source =
    g_timeout_add_seconds (1, check_instance_directories, self);
  g_source_set_name_by_id (self->directory_check_source,
                           "nuvasha-instance-directory-check");
}

NuvashaInstanceManager *
nuvasha_instance_manager_new (const char *data_dir)
{
  g_return_val_if_fail (data_dir == NULL || g_path_is_absolute (data_dir),
                        NULL);

  NuvashaInstanceManager *self =
    g_object_new (nuvasha_TYPE_INSTANCE_MANAGER, NULL);

  if (data_dir == NULL)
    {
      g_autofree char *user_dir = g_build_filename (g_get_user_data_dir (),
                                                    "nuvasha", NULL);
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
nuvasha_instance_manager_get_instances (NuvashaInstanceManager *self)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), NULL);

  return self->instances;
}

char *
nuvasha_instance_manager_default_directory (NuvashaInstanceManager *self,
                                        const char *name)
{
  g_autofree char *normalized = NULL;
  g_autofree char *unique = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), NULL);

  normalized = nuvasha_instance_normalize_name (name);
  if (normalized[0] == '\0')
    return NULL;

  unique = unique_instance_name (self, normalized);
  return default_instance_directory (self, unique);
}

static void
assign_default_instance_icon (NuvashaInstance *instance, const char *directory)
{
  g_autofree char *source_path = nuvasha_instance_default_icon_path ();
  g_autofree char *target_path = g_build_filename (directory, "icon.png", NULL);
  /* An imported instance (for example a modpack) may already contain its
   * own icon. Never overwrite that user/pack-provided file with the default
   * icon while registering the directory. */
  if (source_path != NULL &&
      !g_file_test (target_path, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (GFile) source = g_file_new_for_path (source_path);
      g_autoptr (GFile) target = g_file_new_for_path (target_path);
      if (nuvasha_fs_copy_file (source, target, G_FILE_COPY_OVERWRITE, NULL, NULL))
        nuvasha_instance_set_string (instance, "icon", target_path, NULL);
    }
}

NuvashaInstance *
nuvasha_instance_manager_get_by_id (NuvashaInstanceManager *self, const char *id)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), NULL);

  if (id == NULL)
    return NULL;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (NuvashaInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);

      if (g_str_equal (nuvasha_instance_get_id (instance), id))
        return instance;
    }

  return NULL;
}

NuvashaInstance *
nuvasha_instance_manager_create (NuvashaInstanceManager *self,
                             const char *name,
                             const char *directory,
                             const char *minecraft_version,
                             NuvashaLoader loader,
                             const char *loader_version,
                             GError **error)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  g_autofree char *id = g_uuid_string_random ();
  g_autofree char *path = NULL;
  g_autofree char *normalized_name = nuvasha_instance_normalize_name (name);

  if (normalized_name[0] == '\0' ||
      !instance_name_is_usable (normalized_name))
    {
      const char *forbidden = instance_name_forbidden_character (normalized_name);

      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   normalized_name[0] == '\0'
                      ? _("the instance name is empty")
                      : forbidden != NULL
                        ? _("Instances cannot contain %s in their name")
                        : _("the instance name cannot be used as a directory name"),
                   normalized_name[0] == '\0' || forbidden == NULL
                     ? "" : forbidden);
      return NULL;
    }

  {
    g_autofree char *unique =
      unique_instance_name (self, normalized_name);
    g_free (normalized_name);
    normalized_name = g_steal_pointer (&unique);
  }

  if (directory != NULL)
    {
      if (!check_instance_directory (self, directory, NULL, &path, error))
        return NULL;
    }
  else
    {
      g_autofree char *derived =
        default_instance_directory (self, normalized_name);

      if (!check_instance_directory (self, derived, NULL, &path, error))
        return NULL;
    }

  g_autoptr (NuvashaInstance) instance =
     nuvasha_instance_new (id, normalized_name, path, minecraft_version, loader,
                       error);

  if (instance == NULL)
    return NULL;

  if (loader_version != NULL)
    g_object_set (instance, "loader_version", loader_version, NULL);

  g_autoptr (GError) path_error = NULL;
  if (!g_file_test (path, G_FILE_TEST_IS_DIR) &&
      !nuvasha_fs_make_directory (path, 0755, &path_error))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_IO,
                    "cannot create \"%s\": %s", path,
                    path_error->message);
      return NULL;
    }

  assign_default_instance_icon (instance, path);
  g_list_store_append (self->instances, instance);

  if (!nuvasha_instance_manager_save (self, error))
    {
      /* Nothing on disk lists the instance, so nothing lists it here. */
      g_list_store_remove (self->instances,
                           g_list_model_get_n_items (G_LIST_MODEL (self->instances)) - 1);
      return NULL;
    }

  return g_steal_pointer (&instance);
}

gboolean
nuvasha_instance_manager_rename (NuvashaInstanceManager *self,
                             NuvashaInstance *instance,
                             const char *new_name,
                             GError **error)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  g_autofree char *old_name = NULL;
  g_object_get (instance, "name", &old_name, NULL);

  g_autofree char *stripped = nuvasha_instance_normalize_name (new_name);

  if (stripped[0] == '\0' || !instance_name_is_usable (stripped))
    {
      const char *forbidden = instance_name_forbidden_character (stripped);

      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                    stripped[0] == '\0'
                      ? _("the instance name is empty")
                      : forbidden != NULL
                        ? _("Instances cannot contain %s in their name")
                        : _("the instance name cannot be used as a directory name"),
                   stripped[0] == '\0' || forbidden == NULL
                     ? "" : forbidden);
      return FALSE;
    }

  if (name_is_taken (self, instance, stripped))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "an instance with that name already exists");
      return FALSE;
    }

  g_autoptr (GError) local_error = NULL;

  if (!nuvasha_instance_set_name (instance, stripped, &local_error))
    {
      nuvasha_instance_set_name (instance, old_name, NULL);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (!nuvasha_instance_manager_save (self, error))
    {
      nuvasha_instance_set_name (instance, old_name, NULL);
      return FALSE;
    }

  return TRUE;
}

gboolean
nuvasha_instance_manager_set_string (NuvashaInstanceManager *self,
                                 NuvashaInstance *instance,
                                 const char *property,
                                 const char *value,
                                 GError **error)
{
  static const char *allowed[] = {
    "minecraft-version", "loader-version", "icon", "java-path", "jvm-args",
    NULL
  };
  g_autofree char *old = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (property != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error_literal (error, nuvasha_INSTANCE_MANAGER_ERROR,
                           nuvasha_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                           "the instance is not registered");
      return FALSE;
    }

  gboolean known = FALSE;
  for (guint i = 0; allowed[i] != NULL; i++)
    known |= g_str_equal (property, allowed[i]);
  if (!known)
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "instance property \"%s\" cannot be edited here", property);
      return FALSE;
    }

  g_object_get (instance, property, &old, NULL);
  if (!nuvasha_instance_set_string (instance, property, value, error) ||
      !nuvasha_instance_manager_save (self, error))
    {
      g_object_set (instance, property, old, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
nuvasha_instance_manager_set_loader (NuvashaInstanceManager *self,
                                 NuvashaInstance *instance,
                                 NuvashaLoader loader,
                                 const char *version,
                                 GError **error)
{
  NuvashaLoader old_loader = nuvasha_LOADER_VANILLA;
  g_autofree char *old_version = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error_literal (error, nuvasha_INSTANCE_MANAGER_ERROR,
                           nuvasha_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                           "the instance is not registered");
      return FALSE;
    }

  g_object_get (instance, "loader", &old_loader, "loader-version", &old_version,
                NULL);
  if (!nuvasha_instance_set_loader (instance, loader, version, error) ||
      !nuvasha_instance_manager_save (self, error))
    {
      nuvasha_instance_set_loader (instance, old_loader, old_version, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
remove_instance_and_save (NuvashaInstanceManager *self,
                          NuvashaInstance *instance,
                          GError **error)
{
  guint position = 0;

  if (!g_list_store_find (self->instances, instance, &position))
    {
      g_set_error (error, nuvasha_INSTANCE_MANAGER_ERROR,
                   nuvasha_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  g_list_store_remove (self->instances, position);

  if (!nuvasha_instance_manager_save (self, error))
    {
      g_list_store_insert (self->instances, position, instance);
      return FALSE;
    }

  return TRUE;
}

/* Keep the model consistent with the filesystem. This deliberately checks
 * directories outside the normal nuvasha instances root too, because users can
 * choose an arbitrary instance directory. */
gboolean
check_instance_directories (gpointer user_data)
{
  NuvashaInstanceManager *self = nuvasha_INSTANCE_MANAGER (user_data);

  for (guint i = g_list_model_get_n_items (G_LIST_MODEL (self->instances));
       i > 0;
       i--)
    {
      guint position = i - 1;
      g_autoptr (NuvashaInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (self->instances), position);
      g_autofree char *directory = instance_directory (instance);

      if (g_file_test (directory, G_FILE_TEST_IS_DIR))
        continue;

      g_autoptr (GError) error = NULL;
      if (!remove_instance_and_save (self, instance, &error))
        {
          g_warning ("could not remove missing instance directory from the "
                     "instance list: %s", error->message);
          continue;
        }

      g_debug ("removed instance whose directory disappeared: %s", directory);
    }

  return G_SOURCE_CONTINUE;
}

NuvashaTaskScope *
copy_tree_async (GFile *source, GFile *destination,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  return nuvasha_fs_copy_tree_async (source, destination, callback, user_data);
}

gboolean
copy_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
