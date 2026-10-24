/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaInstance: instance data with validation and JSON serialization. */

#include "nuvasha-instance.h"

#include <string.h>

/* Default JVM arguments of a new instance: none. The version document
 * already carries the heap the game needs, and a guess that is wrong for
 * a machine the user never asked about is worse than nothing. */
#define DEFAULT_JVM_ARGS ""

/* Longest sanitized name in bytes, so that the directory leaf
 * "<name>-<short id>" always fits into a file name. */
#define MAX_NAME_LENGTH 64

/* Used when a name sanitizes down to nothing. */
#define FALLBACK_NAME "instance"

char *
nuvasha_instance_default_icon_path (void)
{
  g_autofree char *installed =
    g_build_filename (nuvasha_DATA_DIR, "pack.png", NULL);
  if (g_file_test (installed, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&installed);

  g_autofree char *source =
    g_build_filename (nuvasha_SOURCE_DATA_DIR, "pack.png", NULL);
  if (g_file_test (source, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&source);

  return NULL;
}

#include "nuvasha-instance-private.h"


G_DEFINE_TYPE (NuvashaInstance, nuvasha_instance, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_ID,
  PROP_NAME,
  PROP_DIRECTORY,
  PROP_MINECRAFT_VERSION,
  PROP_LOADER,
  PROP_LOADER_VERSION,
  PROP_ICON,
  PROP_JAVA_PATH,
  PROP_JVM_ARGS,
  PROP_MODPACK_NAME,
  PROP_MODPACK_SUMMARY,
  PROP_MODPACK_VERSION_ID,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* One table feeds both the enum type of the "loader" property and the
 * string conversion used when serializing to JSON. */
static const GEnumValue nuvasha_loader_values[] = {
  { nuvasha_LOADER_VANILLA,  "nuvasha_LOADER_VANILLA",  "vanilla"  },
  { nuvasha_LOADER_FABRIC,   "nuvasha_LOADER_FABRIC",   "fabric"   },
  { nuvasha_LOADER_FORGE,    "nuvasha_LOADER_FORGE",    "forge"    },
  { nuvasha_LOADER_NEOFORGE, "nuvasha_LOADER_NEOFORGE", "neoforge" },
  { nuvasha_LOADER_QUILT,    "nuvasha_LOADER_QUILT",    "quilt"    },
  { 0, NULL, NULL }
};

GQuark
nuvasha_instance_error_quark (void)
{
  return g_quark_from_static_string ("nuvasha-instance-error");
}

GType
nuvasha_loader_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType loader_type =
        g_enum_register_static ("NuvashaLoader", nuvasha_loader_values);
      g_once_init_leave (&type_id, loader_type);
    }

  return type_id;
}

const char *
nuvasha_loader_to_string (NuvashaLoader loader)
{
  for (guint i = 0; nuvasha_loader_values[i].value_name != NULL; i++)
    {
      if ((NuvashaLoader) nuvasha_loader_values[i].value == loader)
        return nuvasha_loader_values[i].value_nick;
    }

  return NULL;
}

gboolean
nuvasha_loader_from_string (const char *string, NuvashaLoader *loader)
{
  g_return_val_if_fail (loader != NULL, FALSE);

  if (string == NULL)
    return FALSE;

  for (guint i = 0; nuvasha_loader_values[i].value_name != NULL; i++)
    {
      if (g_str_equal (nuvasha_loader_values[i].value_nick, string))
        {
          *loader = (NuvashaLoader) nuvasha_loader_values[i].value;
          return TRUE;
        }
    }

  return FALSE;
}

char *
nuvasha_instance_sanitize_name (const char *name)
{
  g_autofree char *valid =
    g_utf8_make_valid (name != NULL ? name : "", -1);
  g_autoptr (GString) out = g_string_new (NULL);
  gboolean last_dot = FALSE;

  for (const char *p = valid; *p != '\0'; p++)
    {
      char c = *p;

      /* Control characters have no business in a file name. */
      if ((unsigned char) c < 0x20 || c == 0x7f)
        continue;

      if (c == '/')
        c = '-';                      /* never a path separator */
      else if (c == '.' && last_dot)
        continue;                     /* never ".." */

      g_string_append_c (out, c);
      last_dot = (c == '.');
    }

  /* Cap the length, but only at a character boundary. */
  if (out->len > MAX_NAME_LENGTH)
    {
      gsize offset = 0;
      gsize limit = 0;

      while (offset <= MAX_NAME_LENGTH && offset < out->len)
        {
          limit = offset;
          offset = g_utf8_next_char (out->str + offset) - out->str;
        }

      g_string_truncate (out, limit);
    }

  /* No leading or trailing dots and spaces: they are hidden files or
   * awkward directory names on their own. */
  const char *start = out->str;
  while (*start == ' ' || *start == '.')
    start++;

  const char *end = out->str + out->len;
  while (end > start && (end[-1] == ' ' || end[-1] == '.'))
    end--;

  if (end == start)
    return g_strdup (FALLBACK_NAME);

  return g_strndup (start, end - start);
}

char *
nuvasha_instance_normalize_name (const char *name)
{
  g_autofree char *normalized =
    g_utf8_normalize (name != NULL ? name : "", -1, G_NORMALIZE_NFC);

  /* Preserve invalid input so validation can reject it instead of turning it
   * into a different, apparently valid name. */
  if (normalized == NULL)
    return g_strdup (name != NULL ? name : "");

  const char *start = normalized;
  const char *end = normalized + strlen (normalized);

  while (*start != '\0')
    {
      gunichar character = g_utf8_get_char (start);
      if (!g_unichar_isspace (character))
        break;
      start = g_utf8_next_char (start);
    }

  while (end > start)
    {
      const char *previous = g_utf8_prev_char (end);
      if (!g_unichar_isspace (g_utf8_get_char (previous)))
        break;
      end = previous;
    }

  return g_strndup (start, end - start);
}

/* Checks one string field: non-NULL (unless @nullable), non-empty and
 * valid UTF-8 so that it can always be written back to JSON. */
static gboolean
validate_text (const char  *value,
               const char  *field,
               gboolean     nullable,
               GError     **error)
{
  if (value == NULL)
    {
      if (nullable)
        return TRUE;

      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_MISSING_FIELD,
                   "instance %s is missing", field);
      return FALSE;
    }

  /* Blank is as unusable as empty: a name of only spaces would show as
   * an unnamed instance. */
  g_autofree char *stripped = g_strdup (value);
  g_strstrip (stripped);

  if (stripped[0] == '\0')
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance %s must not be empty", field);
      return FALSE;
    }

  if (!g_utf8_validate (value, -1, NULL))
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance %s must be valid UTF-8", field);
      return FALSE;
    }

  for (const char *p = value; *p != '\0'; p = g_utf8_next_char (p))
    if (g_unichar_iscntrl (g_utf8_get_char (p)))
      {
        g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                     "instance %s must not contain control characters", field);
        return FALSE;
      }

  return TRUE;
}

gboolean
nuvasha_instance_validate (NuvashaInstance *self, GError **error)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->id == NULL || self->id[0] == '\0')
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_MISSING_FIELD,
                   "instance id is missing");
      return FALSE;
    }

  if (!g_uuid_string_is_valid (self->id))
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance id \"%s\" is not a valid uuid", self->id);
      return FALSE;
    }

  if (!validate_text (self->name, "name", FALSE, error))
    return FALSE;

  if (self->directory == NULL || !g_path_is_absolute (self->directory))
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance directory must be an absolute path");
      return FALSE;
    }

  g_autofree char *canonical =
    g_canonicalize_filename (self->directory, NULL);

  if (g_strcmp0 (canonical, self->directory) != 0)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance directory \"%s\" is not canonical",
                   self->directory);
      return FALSE;
    }

  if (g_strcmp0 (self->directory, G_DIR_SEPARATOR_S) == 0)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "the filesystem root cannot be an instance directory");
      return FALSE;
    }

  if (!validate_text (self->minecraft_version, "minecraft version",
                      FALSE, error))
    return FALSE;

  if (nuvasha_loader_to_string (self->loader) == NULL)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "unknown mod loader");
      return FALSE;
    }

  if (!validate_text (self->loader_version, "loader version", TRUE, error))
    return FALSE;

  if (!validate_text (self->icon, "icon", TRUE, error))
    return FALSE;

  if (!validate_text (self->java_path, "Java path", TRUE, error))
    return FALSE;

  /* JVM arguments are the one free text field that may be empty: empty is
   * what an instance has when the user has not written anything, and the
   * launcher then adds nothing of its own. */
  if (self->jvm_args != NULL && *self->jvm_args != '\0' &&
      !validate_text (self->jvm_args, "JVM arguments", TRUE, error))
    return FALSE;

  return TRUE;
}

gboolean
nuvasha_instance_set_string (NuvashaInstance *self, const char *property,
                         const char *value, GError **error)
{
  static const char *allowed[] = {
    "minecraft-version", "loader-version", "icon", "java-path", "jvm-args",
    NULL
  };
  g_autofree char *old = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), FALSE);
  g_return_val_if_fail (property != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  gboolean known = FALSE;
  for (guint i = 0; allowed[i] != NULL; i++)
    known |= g_str_equal (property, allowed[i]);
  if (!known)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance property \"%s\" cannot be changed", property);
      return FALSE;
    }

  g_object_get (self, property, &old, NULL);
  g_object_set (self, property, value, NULL);
  if (!nuvasha_instance_validate (self, error))
    {
      g_object_set (self, property, old, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
nuvasha_instance_set_loader (NuvashaInstance *self, NuvashaLoader loader,
                         const char *version, GError **error)
{
  NuvashaLoader old_loader = nuvasha_LOADER_VANILLA;
  g_autofree char *old_version = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_object_get (self, "loader", &old_loader, "loader-version", &old_version,
                NULL);
  g_object_set (self, "loader", loader, "loader-version", version, NULL);
  if (!nuvasha_instance_validate (self, error))
    {
      g_object_set (self, "loader", old_loader, "loader-version",
                    old_version, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
nuvasha_instance_set_name (NuvashaInstance *self, const char *name, GError **error)
{
  g_autofree char *old = NULL;
  g_autofree char *normalized = nuvasha_instance_normalize_name (name);

  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  old = g_strdup (self->name);
  g_object_set (self, "name", normalized, NULL);
  if (!nuvasha_instance_validate (self, error))
    {
      g_object_set (self, "name", old, NULL);
      return FALSE;
    }
  return TRUE;
}

gboolean
nuvasha_instance_set_directory (NuvashaInstance *self, const char *directory,
                            GError **error)
{
  g_autofree char *old = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  old = g_strdup (self->directory);
  g_object_set (self, "directory", directory, NULL);
  if (!nuvasha_instance_validate (self, error))
    {
      g_object_set (self, "directory", old, NULL);
      return FALSE;
    }
  return TRUE;
}

NuvashaInstance *
nuvasha_instance_new (const char  *id,
                  const char  *name,
                  const char  *directory,
                  const char  *minecraft_version,
                  NuvashaLoader    loader,
                  GError     **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  g_autofree char *generated_id = NULL;
  g_autofree char *canonical = NULL;

  if (directory == NULL || !g_path_is_absolute (directory))
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "instance directory must be an absolute path");
      return NULL;
    }

  if (nuvasha_loader_to_string (loader) == NULL)
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "unknown mod loader");
      return NULL;
    }

  canonical = g_canonicalize_filename (directory, NULL);

  if (id == NULL)
    {
      generated_id = g_uuid_string_random ();
      id = generated_id;
    }

  g_autofree char *normalized_name = nuvasha_instance_normalize_name (name);
  NuvashaInstance *self =
    g_object_new (nuvasha_TYPE_INSTANCE,
                  "id", id,
                  "name", normalized_name,
                  "directory", canonical,
                  "minecraft_version", minecraft_version,
                  "loader", loader,
                  NULL);

  if (!nuvasha_instance_validate (self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

const char *
nuvasha_instance_get_id (NuvashaInstance *self)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), NULL);

  return self->id;
}

GHashTable *
nuvasha_instance_get_components (NuvashaInstance *self)
{
  g_return_val_if_fail (nuvasha_IS_INSTANCE (self), NULL);

  return self->components;
}

/* Returns an allocated copy of an empty string as NULL: the nullable
 * fields use NULL, not "", for "not set". */
static char *
optional_string (const char *value)
{
  if (value == NULL || value[0] == '\0')
    return NULL;

  return g_strdup (value);
}

static void
nuvasha_instance_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  NuvashaInstance *self = nuvasha_INSTANCE (object);

  switch (prop_id)
    {
    case PROP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case PROP_NAME:
      {
        g_autofree char *normalized =
          nuvasha_instance_normalize_name (g_value_get_string (value));
        g_free (self->name);
        self->name = g_steal_pointer (&normalized);
      }
      break;

    case PROP_DIRECTORY:
      g_free (self->directory);
      self->directory = g_value_dup_string (value);
      break;

    case PROP_MINECRAFT_VERSION:
      g_free (self->minecraft_version);
      self->minecraft_version = g_value_dup_string (value);
      if (self->components != NULL)
        {
          if (self->minecraft_version != NULL)
            g_hash_table_replace (self->components, g_strdup ("net.minecraft"),
                                  g_strdup (self->minecraft_version));
          else
            g_hash_table_remove (self->components, "net.minecraft");
        }
      break;

    case PROP_LOADER:
      if (self->components != NULL)
        {
          g_hash_table_remove (self->components,
                               "net.fabricmc.fabric-loader");
          g_hash_table_remove (self->components, "net.minecraftforge");
          g_hash_table_remove (self->components,
                               "net.neoforged.neoforge");
          g_hash_table_remove (self->components, "org.quiltmc.quilt-loader");
        }
      self->loader = g_value_get_enum (value);
      if (self->components != NULL && self->loader_version != NULL)
        {
          const char *component = NULL;

          if (self->loader == nuvasha_LOADER_FABRIC)
            component = "net.fabricmc.fabric-loader";
          else if (self->loader == nuvasha_LOADER_FORGE)
            component = "net.minecraftforge";
          else if (self->loader == nuvasha_LOADER_NEOFORGE)
            component = "net.neoforged.neoforge";
          else if (self->loader == nuvasha_LOADER_QUILT)
            component = "org.quiltmc.quilt-loader";

          if (component != NULL)
            g_hash_table_replace (self->components, g_strdup (component),
                                  g_strdup (self->loader_version));
        }
      break;

    case PROP_LOADER_VERSION:
      g_free (self->loader_version);
      self->loader_version = optional_string (g_value_get_string (value));
      if (self->components != NULL)
        {
          const char *component = NULL;

          if (self->loader == nuvasha_LOADER_FABRIC)
            component = "net.fabricmc.fabric-loader";
          else if (self->loader == nuvasha_LOADER_FORGE)
            component = "net.minecraftforge";
          else if (self->loader == nuvasha_LOADER_NEOFORGE)
            component = "net.neoforged.neoforge";
          else if (self->loader == nuvasha_LOADER_QUILT)
            component = "org.quiltmc.quilt-loader";

          if (component != NULL)
            {
              if (self->loader_version != NULL)
                g_hash_table_replace (self->components, g_strdup (component),
                                      g_strdup (self->loader_version));
              else
                g_hash_table_remove (self->components, component);
            }
        }
      break;

    case PROP_ICON:
      g_free (self->icon);
      self->icon = optional_string (g_value_get_string (value));
      break;

    case PROP_JAVA_PATH:
      g_free (self->java_path);
      self->java_path = optional_string (g_value_get_string (value));
      break;

    case PROP_JVM_ARGS:
      g_free (self->jvm_args);
      /* An instance with no arguments holds an empty string, never a
       * null. What is here is what gets serialized, and the parser
       * refuses a null, so storing one writes a file that cannot be read
       * back and the manager answers by quarantining every instance in
       * it. */
      self->jvm_args = g_strdup (g_value_get_string (value) != NULL
                                    ? g_value_get_string (value) : "");
      break;

    case PROP_MODPACK_NAME:
      g_free (self->modpack_name);
      self->modpack_name = optional_string (g_value_get_string (value));
      break;

    case PROP_MODPACK_SUMMARY:
      g_free (self->modpack_summary);
      self->modpack_summary = optional_string (g_value_get_string (value));
      break;

    case PROP_MODPACK_VERSION_ID:
      g_free (self->modpack_version_id);
      self->modpack_version_id = optional_string (g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nuvasha_instance_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  NuvashaInstance *self = nuvasha_INSTANCE (object);

  switch (prop_id)
    {
    case PROP_ID:
      g_value_set_string (value, self->id);
      break;

    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case PROP_DIRECTORY:
      g_value_set_string (value, self->directory);
      break;

    case PROP_MINECRAFT_VERSION:
      g_value_set_string (value, self->minecraft_version);
      break;

    case PROP_LOADER:
      g_value_set_enum (value, self->loader);
      break;

    case PROP_LOADER_VERSION:
      g_value_set_string (value, self->loader_version);
      break;

    case PROP_ICON:
      g_value_set_string (value, self->icon);
      break;

    case PROP_JAVA_PATH:
      g_value_set_string (value, self->java_path);
      break;

    case PROP_JVM_ARGS:
      g_value_set_string (value, self->jvm_args);
      break;

    case PROP_MODPACK_NAME:
      g_value_set_string (value, self->modpack_name);
      break;

    case PROP_MODPACK_SUMMARY:
      g_value_set_string (value, self->modpack_summary);
      break;

    case PROP_MODPACK_VERSION_ID:
      g_value_set_string (value, self->modpack_version_id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nuvasha_instance_finalize (GObject *object)
{
  NuvashaInstance *self = nuvasha_INSTANCE (object);

  g_free (self->id);
  g_free (self->name);
  g_free (self->directory);
  g_free (self->minecraft_version);
  g_free (self->loader_version);
  g_free (self->icon);
  g_free (self->java_path);
  g_free (self->jvm_args);
  g_free (self->modpack_name);
  g_free (self->modpack_summary);
  g_free (self->modpack_version_id);
  g_clear_pointer (&self->components, g_hash_table_unref);

  G_OBJECT_CLASS (nuvasha_instance_parent_class)->finalize (object);
}

static void
nuvasha_instance_class_init (NuvashaInstanceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = nuvasha_instance_set_property;
  object_class->get_property = nuvasha_instance_get_property;
  object_class->finalize = nuvasha_instance_finalize;

  properties[PROP_ID] =
    g_param_spec_string ("id", "Id", "Unique instance id",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  properties[PROP_NAME] =
    g_param_spec_string ("name", "Name", "Display name",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /* Settable, because an instance can be moved to another directory; the
   manager does the moving and the saving around it. */
  properties[PROP_DIRECTORY] =
    g_param_spec_string ("directory", "Directory",
                         "Absolute instance directory",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_MINECRAFT_VERSION] =
    g_param_spec_string ("minecraft-version", "Minecraft version",
                         "Minecraft version to run",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_LOADER] =
    g_param_spec_enum ("loader", "Loader", "Mod loader",
                       nuvasha_loader_get_type (),
                       nuvasha_LOADER_VANILLA,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_LOADER_VERSION] =
    g_param_spec_string ("loader-version", "Loader version",
                         "Mod loader version, or NULL",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ICON] =
    g_param_spec_string ("icon", "Icon",
                         "Icon name or path, or NULL",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_JAVA_PATH] =
    g_param_spec_string ("java-path", "Java path",
                         "Java executable, or NULL for the automatic choice",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_JVM_ARGS] =
    g_param_spec_string ("jvm-args", "JVM arguments",
                         "Command line arguments passed to the JVM",
                         DEFAULT_JVM_ARGS,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_MODPACK_NAME] =
    g_param_spec_string ("modpack-name", "Modpack name",
                         "Original modpack name, or NULL",
                         NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_MODPACK_SUMMARY] =
    g_param_spec_string ("modpack-summary", "Modpack summary",
                         "Original modpack summary, or NULL",
                         NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_MODPACK_VERSION_ID] =
    g_param_spec_string ("modpack-version-id", "Modpack version ID",
                         "Original modpack version ID, or NULL",
                         NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
nuvasha_instance_init (NuvashaInstance *self)
{
  /* Property defaults only apply to construct properties, so the JVM
   * arguments are set here. */
  self->jvm_args = g_strdup (DEFAULT_JVM_ARGS);
  self->loader = nuvasha_LOADER_VANILLA;
  self->components = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             g_free);
}
