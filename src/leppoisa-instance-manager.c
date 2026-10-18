/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaInstanceManager: owns the instance list and keeps instances.json
 * in sync with it.
 *
 * Slow work (copying and deleting instance directories, launching the
 * file manager) runs through GIO async calls so the main loop never
 * blocks. */

/* realpath wants glibc's default feature set; leppoisa is Linux-only. */
#define _GNU_SOURCE

#include "leppoisa-instance-manager-private.h"
#include "leppoisa-i18n.h"
#include "leppoisa-fs.h"
#include "leppoisa-task-private.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static void assign_default_instance_icon (LeppoisaInstance *instance,
                                          const char *directory);


/* Layout version of instances.json. */
#define INSTANCES_FORMAT_VERSION 1

#define METADATA_FILE "instances.json"
#define CORRUPT_SUFFIX ".corrupt"


G_DEFINE_TYPE (LeppoisaInstanceManager, leppoisa_instance_manager, G_TYPE_OBJECT)

static gboolean check_instance_directories (gpointer user_data);

GQuark
leppoisa_instance_manager_error_quark (void)
{
  return g_quark_from_static_string ("leppoisa-instance-manager-error");
}

char *
instance_directory (LeppoisaInstance *instance)
{
  char *directory = NULL;

  g_object_get (instance, "directory", &directory, NULL);
  return directory;
}

char *
instance_name (LeppoisaInstance *instance)
{
  char *name = NULL;

  g_object_get (instance, "name", &name, NULL);
  return name;
}

static char *
instance_name_key (const char *name)
{
  g_autofree char *normalized = leppoisa_instance_normalize_name (name);
  return g_utf8_casefold (normalized, -1);
}

/* The display name is also the automatically chosen directory name. Do not
 * silently turn a name that cannot be a directory component into another
 * name; the user should fix it instead. */
static gboolean
instance_name_is_usable (const char *name)
{
  g_autofree char *safe = leppoisa_instance_sanitize_name (name);

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
name_is_taken (LeppoisaInstanceManager *self, LeppoisaInstance *except,
               const char *name)
{
  g_autofree char *wanted = instance_name_key (name);
  guint count = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < count; i++)
    {
      g_autoptr (LeppoisaInstance) other =
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
default_instance_directory (LeppoisaInstanceManager *self,
                            const char *name)
{
  g_autofree char *safe_name = leppoisa_instance_sanitize_name (name);

  return g_build_filename (self->instances_dir, safe_name, NULL);
}

/* Instance names are case-insensitively unique. Keep the first requested
 * name unchanged and give later instances the smallest numeric suffix that
 * is still available: test, test 2, test 3, ... . */
char *
unique_instance_name (LeppoisaInstanceManager *self, const char *name)
{
  if (!name_is_taken (self, NULL, name))
    return g_strdup (name);

  for (guint suffix = 2; ; suffix++)
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
check_instance_directory (LeppoisaInstanceManager  *self,
                          const char          *directory,
                          LeppoisaInstance         *moved,
                          char               **canonical,
                          GError             **error)
{
  if (directory == NULL || !g_path_is_absolute (directory))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the instance directory must be an absolute path");
      return FALSE;
    }

  if (path_has_parent_reference (directory))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
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
  if (!leppoisa_fs_directory_is_real (parent))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the parent of the instance directory must not contain "
                   "symlinks and must already exist");
      return FALSE;
    }

  if (g_strcmp0 (path, G_DIR_SEPARATOR_S) == 0)
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the filesystem root cannot be an instance directory");
      return FALSE;
    }

  /* The data directory holds instances.json: an instance may not be the
     data directory itself, or its files would sit beside the metadata. */
  if (self->data_dir != NULL && g_strcmp0 (path, self->data_dir) == 0)
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "the leppoisa data directory cannot be an instance directory");
      return FALSE;
    }

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (LeppoisaInstance) other =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);
      g_autofree char *other_name = instance_name (other);
      g_autofree char *other_directory = instance_directory (other);

      if (other == moved)
        continue;

      if (path_contains (path, other_directory) ||
          path_contains (other_directory, path))
        {
          if (g_strcmp0 (path, other_directory) == 0)
            g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                         leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                         "the directory is already used by instance \"%s\"",
                         other_name);
          else
            g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                         leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
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
leppoisa_instance_manager_finalize (GObject *object)
{
  LeppoisaInstanceManager *self = leppoisa_INSTANCE_MANAGER (object);

  if (self->directory_check_source != 0)
    g_source_remove (self->directory_check_source);
  g_clear_object (&self->instances);
  g_free (self->data_dir);
  g_free (self->metadata_path);
  g_free (self->instances_dir);

  G_OBJECT_CLASS (leppoisa_instance_manager_parent_class)->finalize (object);
}

static void
leppoisa_instance_manager_class_init (LeppoisaInstanceManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = leppoisa_instance_manager_finalize;
}

static void
leppoisa_instance_manager_init (LeppoisaInstanceManager *self)
{
  self->instances = g_list_store_new (leppoisa_TYPE_INSTANCE);
  self->directory_check_source =
    g_timeout_add_seconds (1, check_instance_directories, self);
}

LeppoisaInstanceManager *
leppoisa_instance_manager_new (const char *data_dir)
{
  g_return_val_if_fail (data_dir == NULL || g_path_is_absolute (data_dir),
                        NULL);

  LeppoisaInstanceManager *self =
    g_object_new (leppoisa_TYPE_INSTANCE_MANAGER, NULL);

  if (data_dir == NULL)
    {
      g_autofree char *user_dir = g_build_filename (g_get_user_data_dir (),
                                                    "leppoisa", NULL);
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
leppoisa_instance_manager_get_instances (LeppoisaInstanceManager *self)
{
  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), NULL);

  return self->instances;
}

char *
leppoisa_instance_manager_default_directory (LeppoisaInstanceManager *self,
                                        const char *name)
{
  g_autofree char *normalized = NULL;
  g_autofree char *unique = NULL;

  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), NULL);

  normalized = leppoisa_instance_normalize_name (name);
  if (normalized[0] == '\0')
    return NULL;

  unique = unique_instance_name (self, normalized);
  return default_instance_directory (self, unique);
}

static void
assign_default_instance_icon (LeppoisaInstance *instance, const char *directory)
{
  g_autofree char *source_path = leppoisa_instance_default_icon_path ();
  g_autofree char *target_path = g_build_filename (directory, "icon.png", NULL);
  /* An imported instance (for example a modpack) may already contain its
   * own icon. Never overwrite that user/pack-provided file with the default
   * icon while registering the directory. */
  if (source_path != NULL &&
      !g_file_test (target_path, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (GFile) source = g_file_new_for_path (source_path);
      g_autoptr (GFile) target = g_file_new_for_path (target_path);
      if (leppoisa_fs_copy_file (source, target, G_FILE_COPY_OVERWRITE, NULL, NULL))
        leppoisa_instance_set_string (instance, "icon", target_path, NULL);
    }
}

LeppoisaInstance *
leppoisa_instance_manager_get_by_id (LeppoisaInstanceManager *self, const char *id)
{
  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), NULL);

  if (id == NULL)
    return NULL;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->instances));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (LeppoisaInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);

      if (g_str_equal (leppoisa_instance_get_id (instance), id))
        return instance;
    }

  return NULL;
}

/* Serializes every registered instance. Fails when one of them does
 * not validate, so an invalid edit never reaches the file. */
static char *
build_metadata_json (LeppoisaInstanceManager *self, GError **error)
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
      g_autoptr (LeppoisaInstance) instance =
        g_list_model_get_item (G_LIST_MODEL (self->instances), i);

      if (!leppoisa_instance_validate (instance, error))
        return NULL;

      /* The builder takes ownership of the node. */
      json_builder_add_value (builder, leppoisa_instance_to_json (instance));
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
leppoisa_instance_manager_save (LeppoisaInstanceManager *self, GError **error)
{
  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_autofree char *json = build_metadata_json (self, error);

  if (json == NULL)
    return FALSE;

  g_autoptr (GError) directory_error = NULL;
  if (!leppoisa_fs_make_directory (self->data_dir, 0700, &directory_error))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_IO,
                   "cannot create \"%s\": %s",
                    self->data_dir, directory_error->message);
      return FALSE;
    }

  g_autoptr (GError) local_error = NULL;

  /* The data goes to a temporary file in the same directory, is flushed
   * and fsynced and then renamed over instances.json, so a save that
   * fails or is interrupted can never leave a half written file. */
  if (!leppoisa_fs_replace_contents (self->metadata_path, json, -1, 0600,
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

      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "unsupported instances.json version %" G_GINT64_FORMAT,
                   json_node_get_int (node));
      return FALSE;
    }

  g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
               leppoisa_INSTANCE_MANAGER_ERROR_CORRUPT,
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
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "instances.json is not a JSON object");
      return NULL;
    }

  JsonObject *object = json_node_get_object (root);

  if (!get_metadata_version (object, error))
    return NULL;

  JsonNode *array_node = json_object_get_member (object, "instances");

  if (array_node == NULL || !JSON_NODE_HOLDS_ARRAY (array_node))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_CORRUPT,
                   "instances.json has no \"instances\" array");
      return NULL;
    }

  JsonArray *array = json_node_get_array (array_node);
  guint n_items = json_array_get_length (array);
  GList *list = NULL;

  for (guint i = 0; i < n_items; i++)
    {
      LeppoisaInstance *instance =
        leppoisa_instance_new_from_json (json_array_get_element (array, i), error);

      if (instance == NULL)
        {
          g_list_free_full (list, g_object_unref);
          return NULL;
        }

      const char *id = leppoisa_instance_get_id (instance);
      g_autofree char *instance_name_value = instance_name (instance);
      g_autofree char *instance_key =
        instance_name_key (instance_name_value);

      for (GList *l = list; l != NULL; l = l->next)
        {
          LeppoisaInstance *seen = l->data;
          g_autofree char *seen_name = instance_name (seen);
          g_autofree char *seen_key =
            instance_name_key (seen_name);

          if (g_str_equal (leppoisa_instance_get_id (seen), id))
            {
              g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                           leppoisa_INSTANCE_MANAGER_ERROR_CORRUPT,
                           "instances.json lists the id \"%s\" twice", id);
              g_object_unref (instance);
              g_list_free_full (list, g_object_unref);
              return NULL;
            }

          if (g_str_equal (seen_key, instance_key))
            {
              g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                           leppoisa_INSTANCE_MANAGER_ERROR_CORRUPT,
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

  if (corrupt_path == NULL || !leppoisa_fs_rename (path, corrupt_path, NULL))
    g_warning ("could not rename the corrupt file to \"%s\": %s",
               corrupt_path, g_strerror (errno));
}

gboolean
leppoisa_instance_manager_load (LeppoisaInstanceManager *self, GError **error)
{
  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_autofree char *contents = NULL;
  gsize length = 0;
  g_autoptr (GError) local_error = NULL;

  if (!leppoisa_fs_get_contents_within (self->metadata_path,
                                   leppoisa_FS_MAX_INSTANCE_METADATA_SIZE,
                                   &contents, &length, &local_error))
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

  /* A directory removed while leppoisa was not running means the instance was
   * removed outside leppoisa. Reflect that in the model and metadata as well. */
  check_instance_directories (self);

  return TRUE;
}

LeppoisaInstance *
leppoisa_instance_manager_create (LeppoisaInstanceManager *self,
                             const char *name,
                             const char *directory,
                             const char *minecraft_version,
                             LeppoisaLoader loader,
                             const char *loader_version,
                             GError **error)
{
  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  g_autofree char *id = g_uuid_string_random ();
  g_autofree char *path = NULL;
  g_autofree char *normalized_name = leppoisa_instance_normalize_name (name);

  if (normalized_name[0] == '\0' ||
      !instance_name_is_usable (normalized_name))
    {
      const char *forbidden = instance_name_forbidden_character (normalized_name);

      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   normalized_name[0] == '\0'
                     ? "the instance name is empty"
                     : forbidden != NULL
                       ? "Instances cannot contain %s in their name"
                       : "the instance name cannot be used as a directory name",
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

  g_autoptr (LeppoisaInstance) instance =
     leppoisa_instance_new (id, normalized_name, path, minecraft_version, loader,
                       error);

  if (instance == NULL)
    return NULL;

  if (loader_version != NULL)
    g_object_set (instance, "loader_version", loader_version, NULL);

  g_autoptr (GError) path_error = NULL;
  if (!g_file_test (path, G_FILE_TEST_IS_DIR) &&
      !leppoisa_fs_make_directory (path, 0755, &path_error))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_IO,
                    "cannot create \"%s\": %s", path,
                    path_error->message);
      return NULL;
    }

  assign_default_instance_icon (instance, path);
  g_list_store_append (self->instances, instance);

  if (!leppoisa_instance_manager_save (self, error))
    {
      /* Nothing on disk lists the instance, so nothing lists it here. */
      g_list_store_remove (self->instances,
                           g_list_model_get_n_items (G_LIST_MODEL (self->instances)) - 1);
      return NULL;
    }

  return g_steal_pointer (&instance);
}

gboolean
leppoisa_instance_manager_rename (LeppoisaInstanceManager *self,
                             LeppoisaInstance *instance,
                             const char *new_name,
                             GError **error)
{
  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (leppoisa_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  g_autofree char *old_name = NULL;
  g_object_get (instance, "name", &old_name, NULL);

  g_autofree char *stripped = leppoisa_instance_normalize_name (new_name);

  if (stripped[0] == '\0' || !instance_name_is_usable (stripped))
    {
      const char *forbidden = instance_name_forbidden_character (stripped);

      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   stripped[0] == '\0'
                     ? "the instance name is empty"
                     : forbidden != NULL
                       ? "Instances cannot contain %s in their name"
                       : "the instance name cannot be used as a directory name",
                   stripped[0] == '\0' || forbidden == NULL
                     ? "" : forbidden);
      return FALSE;
    }

  if (name_is_taken (self, instance, stripped))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "an instance with that name already exists");
      return FALSE;
    }

  g_autoptr (GError) local_error = NULL;

  if (!leppoisa_instance_set_name (instance, stripped, &local_error))
    {
      leppoisa_instance_set_name (instance, old_name, NULL);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (!leppoisa_instance_manager_save (self, error))
    {
      leppoisa_instance_set_name (instance, old_name, NULL);
      return FALSE;
    }

  return TRUE;
}

gboolean
leppoisa_instance_manager_set_string (LeppoisaInstanceManager *self,
                                 LeppoisaInstance *instance,
                                 const char *property,
                                 const char *value,
                                 GError **error)
{
  static const char *allowed[] = {
    "minecraft-version", "loader-version", "icon", "java-path", "jvm-args",
    NULL
  };
  g_autofree char *old = NULL;

  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (leppoisa_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (property != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error_literal (error, leppoisa_INSTANCE_MANAGER_ERROR,
                           leppoisa_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                           "the instance is not registered");
      return FALSE;
    }

  gboolean known = FALSE;
  for (guint i = 0; allowed[i] != NULL; i++)
    known |= g_str_equal (property, allowed[i]);
  if (!known)
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH,
                   "instance property \"%s\" cannot be edited here", property);
      return FALSE;
    }

  g_object_get (instance, property, &old, NULL);
  if (!leppoisa_instance_set_string (instance, property, value, error) ||
      !leppoisa_instance_manager_save (self, error))
    {
      g_object_set (instance, property, old, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
leppoisa_instance_manager_set_loader (LeppoisaInstanceManager *self,
                                 LeppoisaInstance *instance,
                                 LeppoisaLoader loader,
                                 const char *version,
                                 GError **error)
{
  LeppoisaLoader old_loader = leppoisa_LOADER_VANILLA;
  g_autofree char *old_version = NULL;

  g_return_val_if_fail (leppoisa_IS_INSTANCE_MANAGER (self), FALSE);
  g_return_val_if_fail (leppoisa_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_list_store_find (self->instances, instance, NULL))
    {
      g_set_error_literal (error, leppoisa_INSTANCE_MANAGER_ERROR,
                           leppoisa_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                           "the instance is not registered");
      return FALSE;
    }

  g_object_get (instance, "loader", &old_loader, "loader-version", &old_version,
                NULL);
  if (!leppoisa_instance_set_loader (instance, loader, version, error) ||
      !leppoisa_instance_manager_save (self, error))
    {
      leppoisa_instance_set_loader (instance, old_loader, old_version, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
remove_instance_and_save (LeppoisaInstanceManager *self,
                          LeppoisaInstance *instance,
                          GError **error)
{
  guint position = 0;

  if (!g_list_store_find (self->instances, instance, &position))
    {
      g_set_error (error, leppoisa_INSTANCE_MANAGER_ERROR,
                   leppoisa_INSTANCE_MANAGER_ERROR_NOT_FOUND,
                   "the instance is not registered");
      return FALSE;
    }

  g_list_store_remove (self->instances, position);

  if (!leppoisa_instance_manager_save (self, error))
    {
      g_list_store_insert (self->instances, position, instance);
      return FALSE;
    }

  return TRUE;
}

/* Keep the model consistent with the filesystem. This deliberately checks
 * directories outside the normal leppoisa instances root too, because users can
 * choose an arbitrary instance directory. */
static gboolean
check_instance_directories (gpointer user_data)
{
  LeppoisaInstanceManager *self = leppoisa_INSTANCE_MANAGER (user_data);

  for (guint i = g_list_model_get_n_items (G_LIST_MODEL (self->instances));
       i > 0;
       i--)
    {
      guint position = i - 1;
      g_autoptr (LeppoisaInstance) instance =
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

LeppoisaTaskScope *
copy_tree_async (GFile *source, GFile *destination,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  return leppoisa_fs_copy_tree_async (source, destination, callback, user_data);
}

gboolean
copy_tree_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
