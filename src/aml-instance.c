/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstance: instance data with validation and JSON serialization. */

#include "aml-instance.h"

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

struct _AmlInstance
{
  GObject parent_instance;

  char *id;
  char *name;
  char *directory;
  char *minecraft_version;
  AmlLoader loader;
  char *loader_version; /* NULL: vanilla or not set yet */
  char *icon;
  char *java_path;      /* NULL: the closest compatible Java, automatically */
  char *jvm_args;
  GHashTable *components; /* stable component id -> owned version */
};

G_DEFINE_TYPE (AmlInstance, aml_instance, G_TYPE_OBJECT)

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
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* One table feeds both the enum type of the "loader" property and the
 * string conversion used when serializing to JSON. */
static const GEnumValue aml_loader_values[] = {
  { AML_LOADER_VANILLA,  "AML_LOADER_VANILLA",  "vanilla"  },
  { AML_LOADER_FABRIC,   "AML_LOADER_FABRIC",   "fabric"   },
  { AML_LOADER_FORGE,    "AML_LOADER_FORGE",    "forge"    },
  { AML_LOADER_NEOFORGE, "AML_LOADER_NEOFORGE", "neoforge" },
  { AML_LOADER_QUILT,    "AML_LOADER_QUILT",    "quilt"    },
  { 0, NULL, NULL }
};

GQuark
aml_instance_error_quark (void)
{
  return g_quark_from_static_string ("aml-instance-error");
}

GType
aml_loader_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id))
    {
      GType loader_type =
        g_enum_register_static ("AmlLoader", aml_loader_values);
      g_once_init_leave (&type_id, loader_type);
    }

  return type_id;
}

const char *
aml_loader_to_string (AmlLoader loader)
{
  for (guint i = 0; aml_loader_values[i].value_name != NULL; i++)
    {
      if ((AmlLoader) aml_loader_values[i].value == loader)
        return aml_loader_values[i].value_nick;
    }

  return NULL;
}

gboolean
aml_loader_from_string (const char *string, AmlLoader *loader)
{
  g_return_val_if_fail (loader != NULL, FALSE);

  if (string == NULL)
    return FALSE;

  for (guint i = 0; aml_loader_values[i].value_name != NULL; i++)
    {
      if (g_str_equal (aml_loader_values[i].value_nick, string))
        {
          *loader = (AmlLoader) aml_loader_values[i].value;
          return TRUE;
        }
    }

  return FALSE;
}

char *
aml_instance_sanitize_name (const char *name)
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

      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_MISSING_FIELD,
                   "instance %s is missing", field);
      return FALSE;
    }

  /* Blank is as unusable as empty: a name of only spaces would show as
   * an unnamed instance. */
  g_autofree char *stripped = g_strdup (value);
  g_strstrip (stripped);

  if (stripped[0] == '\0')
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance %s must not be empty", field);
      return FALSE;
    }

  if (!g_utf8_validate (value, -1, NULL))
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance %s must be valid UTF-8", field);
      return FALSE;
    }

  return TRUE;
}

gboolean
aml_instance_validate (AmlInstance *self, GError **error)
{
  g_return_val_if_fail (AML_IS_INSTANCE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->id == NULL || self->id[0] == '\0')
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_MISSING_FIELD,
                   "instance id is missing");
      return FALSE;
    }

  if (!g_uuid_string_is_valid (self->id))
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance id \"%s\" is not a valid uuid", self->id);
      return FALSE;
    }

  if (!validate_text (self->name, "name", FALSE, error))
    return FALSE;

  if (self->directory == NULL || !g_path_is_absolute (self->directory))
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance directory must be an absolute path");
      return FALSE;
    }

  g_autofree char *canonical =
    g_canonicalize_filename (self->directory, NULL);

  if (g_strcmp0 (canonical, self->directory) != 0)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance directory \"%s\" is not canonical",
                   self->directory);
      return FALSE;
    }

  if (g_strcmp0 (self->directory, G_DIR_SEPARATOR_S) == 0)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "the filesystem root cannot be an instance directory");
      return FALSE;
    }

  if (!validate_text (self->minecraft_version, "minecraft version",
                      FALSE, error))
    return FALSE;

  if (aml_loader_to_string (self->loader) == NULL)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
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

AmlInstance *
aml_instance_new (const char  *id,
                  const char  *name,
                  const char  *directory,
                  const char  *minecraft_version,
                  AmlLoader    loader,
                  GError     **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  g_autofree char *generated_id = NULL;
  g_autofree char *canonical = NULL;

  if (directory == NULL || !g_path_is_absolute (directory))
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance directory must be an absolute path");
      return NULL;
    }

  if (aml_loader_to_string (loader) == NULL)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "unknown mod loader");
      return NULL;
    }

  canonical = g_canonicalize_filename (directory, NULL);

  if (id == NULL)
    {
      generated_id = g_uuid_string_random ();
      id = generated_id;
    }

  AmlInstance *self =
    g_object_new (AML_TYPE_INSTANCE,
                  "id", id,
                  "name", name,
                  "directory", canonical,
                  "minecraft_version", minecraft_version,
                  "loader", loader,
                  NULL);

  if (!aml_instance_validate (self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

/* Reads a required string member from a JSON object. The returned
 * string belongs to the parser and stays valid while it lives. */
static const char *
get_required_string (JsonObject  *object,
                     const char  *member,
                     GError     **error)
{
  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_MISSING_FIELD,
                   "instance is missing \"%s\"", member);
      return NULL;
    }

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance field \"%s\" must be a string", member);
      return NULL;
    }

  return json_node_get_string (node);
}

/* Reads an optional string member: absent, null and empty all become
 * NULL, which is how the nullable fields are stored. */
static gboolean
get_optional_string (JsonObject   *object,
                     const char   *member,
                     const char  **value,
                     GError      **error)
{
  *value = NULL;

  JsonNode *node = json_object_get_member (object, member);

  if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    return TRUE;

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance field \"%s\" must be a string", member);
      return FALSE;
    }

  const char *string = json_node_get_string (node);

  if (string != NULL && string[0] != '\0')
    *value = string;

  return TRUE;
}

static gboolean
get_loader (JsonObject *object, AmlLoader *loader, GError **error)
{
  const char *string = get_required_string (object, "loader", error);

  if (string == NULL)
    return FALSE;

  if (!aml_loader_from_string (string, loader))
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "unknown mod loader \"%s\"", string);
      return FALSE;
    }

  return TRUE;
}

/* "jvm_args" is optional and may be an empty string: an instance the
   * user never gave any arguments to has none, and the launcher adds
   * nothing from it. */
static gboolean
get_jvm_args (JsonObject *object, const char **jvm_args, GError **error)
{
  JsonNode *node = json_object_get_member (object, "jvm_args");

  if (node == NULL)
    {
      *jvm_args = DEFAULT_JVM_ARGS;
      return TRUE;
    }

  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance field \"jvm_args\" must be a string");
      return FALSE;
    }

  const char *string = json_node_get_string (node);

  if (string == NULL)
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance field \"jvm_args\" must not be null");
      return FALSE;
    }

  *jvm_args = string;
  return TRUE;
}

AmlInstance *
aml_instance_new_from_json (JsonNode *node, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "instance entry is not a JSON object");
      return NULL;
    }

  JsonObject *object = json_node_get_object (node);

  const char *id = get_required_string (object, "id", error);
  if (id == NULL)
    return NULL;

  const char *name = get_required_string (object, "name", error);
  if (name == NULL)
    return NULL;

  const char *directory = get_required_string (object, "directory", error);
  if (directory == NULL)
    return NULL;

  const char *minecraft_version =
    get_required_string (object, "minecraft_version", error);
  if (minecraft_version == NULL)
    return NULL;

  AmlLoader loader;
  if (!get_loader (object, &loader, error))
    return NULL;

  const char *loader_version = NULL;
  if (!get_optional_string (object, "loader_version", &loader_version, error))
    return NULL;

  const char *icon = NULL;
  if (!get_optional_string (object, "icon", &icon, error))
    return NULL;

  const char *java_path = NULL;
  if (!get_optional_string (object, "java_path", &java_path, error))
    return NULL;

  const char *jvm_args = NULL;
  if (!get_jvm_args (object, &jvm_args, error))
    return NULL;

  AmlInstance *self =
    aml_instance_new (id, name, directory, minecraft_version, loader, error);

  if (self == NULL)
    return NULL;

  g_object_set (self,
                "loader_version", loader_version,
                "icon", icon,
                "java_path", java_path,
                "jvm_args", jvm_args,
                NULL);

  if (!aml_instance_validate (self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  return self;
}

static void
add_optional_member (JsonBuilder *builder,
                     const char  *member_name,
                     const char  *value)
{
  json_builder_set_member_name (builder, member_name);

  if (value == NULL)
    json_builder_add_null_value (builder);
  else
    json_builder_add_string_value (builder, value);
}

JsonNode *
aml_instance_to_json (AmlInstance *self)
{
  g_return_val_if_fail (AML_IS_INSTANCE (self), NULL);

  if (!aml_instance_validate (self, NULL))
    {
      g_critical ("AmlInstance: refusing to serialize an invalid instance");
      return NULL;
    }

  g_autoptr (JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "id");
  json_builder_add_string_value (builder, self->id);

  json_builder_set_member_name (builder, "name");
  json_builder_add_string_value (builder, self->name);

  json_builder_set_member_name (builder, "directory");
  json_builder_add_string_value (builder, self->directory);

  json_builder_set_member_name (builder, "minecraft_version");
  json_builder_add_string_value (builder, self->minecraft_version);

  json_builder_set_member_name (builder, "loader");
  json_builder_add_string_value (builder,
                                 aml_loader_to_string (self->loader));

  add_optional_member (builder, "loader_version", self->loader_version);
  add_optional_member (builder, "icon", self->icon);
  add_optional_member (builder, "java_path", self->java_path);

  add_optional_member (builder, "jvm_args", self->jvm_args);

  json_builder_end_object (builder);

  return json_builder_get_root (builder);
}

const char *
aml_instance_get_id (AmlInstance *self)
{
  g_return_val_if_fail (AML_IS_INSTANCE (self), NULL);

  return self->id;
}

GHashTable *
aml_instance_get_components (AmlInstance *self)
{
  g_return_val_if_fail (AML_IS_INSTANCE (self), NULL);

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
aml_instance_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  AmlInstance *self = AML_INSTANCE (object);

  switch (prop_id)
    {
    case PROP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
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

          if (self->loader == AML_LOADER_FABRIC)
            component = "net.fabricmc.fabric-loader";
          else if (self->loader == AML_LOADER_FORGE)
            component = "net.minecraftforge";
          else if (self->loader == AML_LOADER_NEOFORGE)
            component = "net.neoforged.neoforge";
          else if (self->loader == AML_LOADER_QUILT)
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

          if (self->loader == AML_LOADER_FABRIC)
            component = "net.fabricmc.fabric-loader";
          else if (self->loader == AML_LOADER_FORGE)
            component = "net.minecraftforge";
          else if (self->loader == AML_LOADER_NEOFORGE)
            component = "net.neoforged.neoforge";
          else if (self->loader == AML_LOADER_QUILT)
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
      self->jvm_args = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
aml_instance_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  AmlInstance *self = AML_INSTANCE (object);

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

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
aml_instance_finalize (GObject *object)
{
  AmlInstance *self = AML_INSTANCE (object);

  g_free (self->id);
  g_free (self->name);
  g_free (self->directory);
  g_free (self->minecraft_version);
  g_free (self->loader_version);
  g_free (self->icon);
  g_free (self->java_path);
  g_free (self->jvm_args);
  g_clear_pointer (&self->components, g_hash_table_unref);

  G_OBJECT_CLASS (aml_instance_parent_class)->finalize (object);
}

static void
aml_instance_class_init (AmlInstanceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = aml_instance_set_property;
  object_class->get_property = aml_instance_get_property;
  object_class->finalize = aml_instance_finalize;

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
                       aml_loader_get_type (),
                       AML_LOADER_VANILLA,
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

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
aml_instance_init (AmlInstance *self)
{
  /* Property defaults only apply to construct properties, so the JVM
   * arguments are set here. */
  self->jvm_args = g_strdup (DEFAULT_JVM_ARGS);
  self->loader = AML_LOADER_VANILLA;
  self->components = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             g_free);
}
