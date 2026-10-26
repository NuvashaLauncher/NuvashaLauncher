/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for the nuvashalauncherInstance model: loader strings, name sanitization,
 * validation and JSON round trip. Runs headless and touches no
 * directories. */

#include "nuvashalauncher-instance.h"
#include "nuvashalauncher-instance-components.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#define TEST_UUID "11111111-2222-3333-4444-555555555555"

/* Parses test JSON. The returned node belongs to the parser and stays
 * valid as long as the parser lives. */
static JsonParser *
parse_json (const char *text)
{
  g_autoptr (GError) error = NULL;
  JsonParser *parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, text, -1, &error))
    g_error ("test json does not parse: %s", error->message);

  return parser;
}

static char *
prop_string (nuvashalauncherInstance *instance, const char *property)
{
  char *value = NULL;

  g_object_get (instance, property, &value, NULL);
  return value;
}

static void
assert_safe_name (const char *name)
{
  g_assert_nonnull (name);
  g_assert_cmpstr (name, !=, "");
  g_assert_null (strstr (name, "/"));
  g_assert_null (strstr (name, ".."));
  g_assert_cmpint (name[0], !=, '.');
}

static void
test_loader_strings (void)
{
  const nuvashalauncherLoader loaders[] = {
    nuvashalauncher_LOADER_VANILLA, nuvashalauncher_LOADER_FABRIC, nuvashalauncher_LOADER_FORGE,
    nuvashalauncher_LOADER_NEOFORGE, nuvashalauncher_LOADER_QUILT
  };
  const char *names[] = {
    "vanilla", "fabric", "forge", "neoforge", "quilt"
  };

  for (guint i = 0; i < G_N_ELEMENTS (loaders); i++)
    {
      g_assert_cmpstr (nuvashalauncher_loader_to_string (loaders[i]), ==, names[i]);

      nuvashalauncherLoader parsed = nuvashalauncher_LOADER_VANILLA;
      g_assert_true (nuvashalauncher_loader_from_string (names[i], &parsed));
      g_assert_cmpint (parsed, ==, loaders[i]);
    }

  nuvashalauncherLoader unknown = nuvashalauncher_LOADER_VANILLA;
  g_assert_false (nuvashalauncher_loader_from_string ("sodium", &unknown));
  g_assert_false (nuvashalauncher_loader_from_string (NULL, &unknown));
  g_assert_null (nuvashalauncher_loader_to_string ((nuvashalauncherLoader) 99));

  g_print ("ok - loader strings\n");
}

static void
test_sanitize_name (void)
{
  g_autofree char *plain = nuvashalauncher_instance_sanitize_name ("Hello World");
  g_assert_cmpstr (plain, ==, "Hello World");

  g_autofree char *slash = nuvashalauncher_instance_sanitize_name ("a/b");
  g_assert_cmpstr (slash, ==, "a-b");

  g_autofree char *dot_dot = nuvashalauncher_instance_sanitize_name ("..");
  g_assert_cmpstr (dot_dot, ==, "instance");

  g_autofree char *dots = nuvashalauncher_instance_sanitize_name ("...");
  g_assert_cmpstr (dots, ==, "instance");

  g_autofree char *empty = nuvashalauncher_instance_sanitize_name ("");
  g_assert_cmpstr (empty, ==, "instance");

  g_autofree char *null_name = nuvashalauncher_instance_sanitize_name (NULL);
  g_assert_cmpstr (null_name, ==, "instance");

  g_autofree char *blanks = nuvashalauncher_instance_sanitize_name ("   ");
  g_assert_cmpstr (blanks, ==, "instance");

  g_autofree char *hidden = nuvashalauncher_instance_sanitize_name (".hidden");
  g_assert_cmpstr (hidden, ==, "hidden");

  g_autofree char *inner = nuvashalauncher_instance_sanitize_name ("a..b");
  g_assert_cmpstr (inner, ==, "a.b");

  g_autofree char *padded = nuvashalauncher_instance_sanitize_name ("  padded  ");
  g_assert_cmpstr (padded, ==, "padded");

  g_autofree char *escape =
    nuvashalauncher_instance_sanitize_name ("../../../etc/passwd");
  assert_safe_name (escape);

  g_autofree char *utf8 =
    nuvashalauncher_instance_sanitize_name ("Caf\xc3\xa9" "/Montr\xc3\xa9" "al");
  g_assert_cmpstr (utf8, ==, "Caf\xc3\xa9" "-Montr\xc3\xa9" "al");

  g_autofree char *long_name = g_strnfill (100, 'a');
  g_autofree char *capped = nuvashalauncher_instance_sanitize_name (long_name);
  assert_safe_name (capped);
  g_assert_cmpuint (strlen (capped), <=, 64);
  g_assert_cmpuint (strlen (capped), <=, 64);

  g_print ("ok - name sanitization\n");
}

static void
test_normalize_name (void)
{
  g_autofree char *trimmed =
    nuvashalauncher_instance_normalize_name (" \tCafe\xCC\x81  ");
  g_autofree char *composed = nuvashalauncher_instance_normalize_name ("Caf\xC3\xA9");
  g_autofree char *stored = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_new (NULL, " \tCafe\xCC\x81  ", "/tmp/normalized-instance",
                      "1.21.1", nuvashalauncher_LOADER_VANILLA, &error);

  g_assert_cmpstr (trimmed, ==, "Caf\xC3\xA9");
  g_assert_cmpstr (composed, ==, "Caf\xC3\xA9");
  g_assert_no_error (error);
  g_object_get (instance, "name", &stored, NULL);
  g_assert_cmpstr (stored, ==, "Caf\xC3\xA9");

  g_print ("ok - instance names are normalized at the model boundary\n");
}

static void
test_jvm_args_survive_null (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_new (NULL, "Test", "/tmp/nuvashalauncher-jvm-args-test", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_autofree char *stored = NULL;
  g_autofree char *round_tripped = NULL;
  g_autoptr (JsonNode) node = NULL;
  g_autoptr (nuvashalauncherInstance) reloaded = NULL;

  g_assert_no_error (error);

  /* Clearing the field is the one write the model does not refuse:
     nuvashalauncher_instance_validate treats a null and an empty string alike, and
     the UI passes an empty string for a cleared row. */
  g_assert_true (nuvashalauncher_instance_set_string (instance, "jvm-args", NULL, NULL));
  g_assert_true (nuvashalauncher_instance_validate (instance, NULL));

  /* What must not happen is the field holding a null, because the file
     would then carry "jvm_args": null and the parser refuses it. */
  g_object_get (instance, "jvm-args", &stored, NULL);
  g_assert_cmpstr (stored, ==, "");

  /* The whole point: an instance written after this still reads back. */
  node = nuvashalauncher_instance_to_json (instance);
  g_assert_nonnull (node);

  reloaded = nuvashalauncher_instance_new_from_json (node, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reloaded);

  g_object_get (reloaded, "jvm-args", &round_tripped, NULL);
  g_assert_cmpstr (round_tripped, ==, "");

  g_print ("ok - clearing jvm-args leaves an instance that reads back\n");
}

static void
test_new_defaults (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_new (NULL, "Test", "/tmp/nuvashalauncher-instance-test", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);

  g_assert_no_error (error);
  g_assert_nonnull (instance);

  const char *id = nuvashalauncher_instance_get_id (instance);
  g_assert_nonnull (id);
  g_assert_true (g_uuid_string_is_valid (id));

  g_autofree char *jvm_args = prop_string (instance, "jvm-args");
  g_autofree char *loader_version = prop_string (instance, "loader-version");
  g_autofree char *java_path = prop_string (instance, "java-path");
  g_autofree char *icon = prop_string (instance, "icon");

  g_assert_cmpstr (jvm_args, ==, "");
  g_assert_null (loader_version);
  g_assert_null (java_path);
  g_assert_null (icon);

  g_assert_true (nuvashalauncher_instance_validate (instance, &error));
  g_assert_no_error (error);

  g_print ("ok - instance defaults\n");
}

static void
test_new_rejects_bad_values (void)
{
  g_autoptr (GError) error = NULL;

  /* A relative directory is not usable as an instance directory. */
  g_autoptr (nuvashalauncherInstance) relative =
    nuvashalauncher_instance_new (NULL, "Test", "relative/path", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_null (relative);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  g_autoptr (nuvashalauncherInstance) no_directory =
    nuvashalauncher_instance_new (NULL, "Test", NULL, "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_null (no_directory);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  /* An empty name must never become an instance. */
  g_autoptr (nuvashalauncherInstance) blank_name =
    nuvashalauncher_instance_new (NULL, "", "/tmp/nuvashalauncher-instance-test", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_null (blank_name);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  g_autoptr (nuvashalauncherInstance) spaces_name =
    nuvashalauncher_instance_new (NULL, "   ", "/tmp/nuvashalauncher-instance-test", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_null (spaces_name);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  g_autoptr (nuvashalauncherInstance) no_version =
    nuvashalauncher_instance_new (NULL, "Test", "/tmp/nuvashalauncher-instance-test", "",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_null (no_version);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  /* An unknown loader value is refused before it reaches the property. */
  g_autoptr (nuvashalauncherInstance) bad_loader =
    nuvashalauncher_instance_new (NULL, "Test", "/tmp/nuvashalauncher-instance-test", "1.21.1",
                      (nuvashalauncherLoader) 99, &error);
  g_assert_null (bad_loader);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  g_autoptr (nuvashalauncherInstance) bad_id =
    nuvashalauncher_instance_new ("not-a-uuid", "Test", "/tmp/nuvashalauncher-instance-test",
                      "1.21.1", nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_null (bad_id);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  /* The root directory is never an instance directory. */
  g_autoptr (nuvashalauncherInstance) root =
    nuvashalauncher_instance_new (NULL, "Test", "/", "1.21.1",
                      nuvashalauncher_LOADER_VANILLA, &error);
  g_assert_null (root);
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  g_print ("ok - invalid instance values are rejected\n");
}

static void
test_validate (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_new (NULL, "Valid", "/tmp/nuvashalauncher-instance-test", "1.21.1",
                      nuvashalauncher_LOADER_NEOFORGE, &error);

  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_assert_true (nuvashalauncher_instance_validate (instance, &error));
  g_assert_no_error (error);

  /* An empty name breaks validation. */
  g_object_set (instance, "name", "", NULL);
  g_assert_false (nuvashalauncher_instance_validate (instance, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);
  g_object_set (instance, "name", "Valid", NULL);

  /* JVM arguments are the one text field that may be empty: that is what
     an instance the user never wrote any for has. */
  g_object_set (instance, "jvm-args", "", NULL);
  g_assert_true (nuvashalauncher_instance_validate (instance, &error));
  g_assert_no_error (error);
  g_object_set (instance, "jvm-args", "-Xmx2G", NULL);

  /* A directory with ".." inside is not canonical. */
  g_autoptr (nuvashalauncherInstance) escaping =
    g_object_new (nuvashalauncher_TYPE_INSTANCE,
                  "id", TEST_UUID,
                  "name", "Escaping",
                  "directory", "/tmp/nuvashalauncher-instance-test/../elsewhere",
                  "minecraft-version", "1.21.1",
                  "loader", nuvashalauncher_LOADER_VANILLA,
                  NULL);
  g_assert_false (nuvashalauncher_instance_validate (escaping, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
  g_clear_error (&error);

  /* An object built without any field reports the missing id. */
  g_autoptr (nuvashalauncherInstance) empty = g_object_new (nuvashalauncher_TYPE_INSTANCE, NULL);
  g_assert_false (nuvashalauncher_instance_validate (empty, &error));
  g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_MISSING_FIELD);
  g_clear_error (&error);

  /* Empty means "not set" for the nullable fields. */
  g_object_set (instance, "loader-version", "", NULL);
  g_autofree char *loader_version =
    prop_string (instance, "loader-version");
  g_assert_null (loader_version);

  g_print ("ok - instance validation\n");
}

static void
test_json_round_trip (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) original =
    nuvashalauncher_instance_new (NULL, "Fabric Pack", "/tmp/nuvashalauncher-instance-fabric",
                      "1.21.1", nuvashalauncher_LOADER_FABRIC, &error);

  g_assert_no_error (error);
  g_assert_nonnull (original);

  g_object_set (original,
                "loader-version", "0.16.9",
                "icon", "applications-games-symbolic",
                "java-path", "/usr/bin/java",
                "jvm-args", "-Xmx4G -Xms1G",
                NULL);

  JsonNode *node = nuvashalauncher_instance_to_json (original);
  g_assert_nonnull (node);

  g_autoptr (nuvashalauncherInstance) copy =
    nuvashalauncher_instance_new_from_json (node, &error);
  json_node_unref (node);

  g_assert_no_error (error);
  g_assert_nonnull (copy);

  g_assert_cmpstr (nuvashalauncher_instance_get_id (copy), ==,
                   nuvashalauncher_instance_get_id (original));

  const char *fields[] = {
    "name", "directory", "minecraft-version", "loader-version", "icon",
    "java-path", "jvm-args"
  };

  for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
    {
      g_autofree char *expected = prop_string (original, fields[i]);
      g_autofree char *actual = prop_string (copy, fields[i]);
      g_assert_cmpstr (actual, ==, expected);
    }

  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;
  g_object_get (copy, "loader", &loader, NULL);
  g_assert_cmpint (loader, ==, nuvashalauncher_LOADER_FABRIC);

  g_print ("ok - json round trip\n");
}

/* An instance object without one of the required members. */
static JsonNode *
build_instance_json (const char *skip_member)
{
  static const char *members[] = {
    "id", "name", "directory", "minecraft_version", "loader",
    "loader_version"
  };
  static const char *values[] = {
    TEST_UUID, "Test", "/tmp/nuvashalauncher-instance-test", "1.21.1", "fabric",
    "0.16.9"
  };

  g_autoptr (JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);

  for (guint i = 0; i < G_N_ELEMENTS (members); i++)
    {
      if (g_strcmp0 (members[i], skip_member) == 0)
        continue;

      json_builder_set_member_name (builder, members[i]);
      json_builder_add_string_value (builder, values[i]);
    }

  json_builder_end_object (builder);

  return json_builder_get_root (builder);
}

static void
test_missing_fields (void)
{
  static const char *required[] = {
    "id", "name", "directory", "minecraft_version", "loader"
  };

  for (guint i = 0; i < G_N_ELEMENTS (required); i++)
    {
      g_autoptr (GError) error = NULL;
      JsonNode *node = build_instance_json (required[i]);

      g_autoptr (nuvashalauncherInstance) instance =
        nuvashalauncher_instance_new_from_json (node, &error);

      json_node_unref (node);

      g_assert_null (instance);
      g_assert_error (error, nuvashalauncher_INSTANCE_ERROR,
                      nuvashalauncher_INSTANCE_ERROR_MISSING_FIELD);
    }

  g_print ("ok - missing fields fail cleanly\n");
}

static void
test_malformed_json (void)
{
  g_autoptr (GError) error = NULL;

  /* Not a JSON object at all. */
  {
    g_autoptr (JsonParser) parser = parse_json ("42");
    g_autoptr (nuvashalauncherInstance) instance =
      nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);
    g_assert_null (instance);
    g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
    g_clear_error (&error);
  }

  {
    g_autoptr (JsonParser) parser = parse_json ("[1, 2]");
    g_autoptr (nuvashalauncherInstance) instance =
      nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);
    g_assert_null (instance);
    g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
    g_clear_error (&error);
  }

  /* Wrong types for fields that must be strings. */
  {
    g_autofree char *text =
      g_strdup_printf ("{\"id\": 42, \"name\": \"Test\", "
                       "\"directory\": \"/tmp/nuvashalauncher-instance-test\", "
                       "\"minecraft_version\": \"1.21.1\", "
                       "\"loader\": \"vanilla\"}");
    g_autoptr (JsonParser) parser = parse_json (text);
    g_autoptr (nuvashalauncherInstance) instance =
      nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);
    g_assert_null (instance);
    g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
    g_clear_error (&error);
  }

  {
    g_autofree char *text =
      g_strdup_printf ("{\"id\": \"%s\", \"name\": \"Test\", "
                       "\"directory\": \"/tmp/nuvashalauncher-instance-test\", "
                       "\"minecraft_version\": \"1.21.1\", "
                       "\"loader\": 7}",
                       TEST_UUID);
    g_autoptr (JsonParser) parser = parse_json (text);
    g_autoptr (nuvashalauncherInstance) instance =
      nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);
    g_assert_null (instance);
    g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
    g_clear_error (&error);
  }

  /* An unknown loader name. */
  {
    g_autofree char *text =
      g_strdup_printf ("{\"id\": \"%s\", \"name\": \"Test\", "
                       "\"directory\": \"/tmp/nuvashalauncher-instance-test\", "
                       "\"minecraft_version\": \"1.21.1\", "
                       "\"loader\": \"sodium\"}",
                       TEST_UUID);
    g_autoptr (JsonParser) parser = parse_json (text);
    g_autoptr (nuvashalauncherInstance) instance =
      nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);
    g_assert_null (instance);
    g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
    g_clear_error (&error);
  }

  /* Empty JVM arguments are what an instance has by default, so a file
     that says so is read, not refused. */
  {
    g_autofree char *text =
      g_strdup_printf ("{\"id\": \"%s\", \"name\": \"Test\", "
                       "\"directory\": \"/tmp/nuvashalauncher-instance-test\", "
                       "\"minecraft_version\": \"1.21.1\", "
                       "\"loader\": \"vanilla\", \"jvm_args\": \"\"}",
                       TEST_UUID);
    g_autoptr (JsonParser) parser = parse_json (text);
    g_autoptr (nuvashalauncherInstance) instance =
      nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);
    g_autofree char *jvm_args = NULL;

    g_assert_no_error (error);
    g_assert_nonnull (instance);
    g_object_get (instance, "jvm-args", &jvm_args, NULL);
    g_assert_cmpstr (jvm_args, ==, "");
  }

  /* A null one is not a value, though. */
  {
    g_autofree char *text =
      g_strdup_printf ("{\"id\": \"%s\", \"name\": \"Test\", "
                       "\"directory\": \"/tmp/nuvashalauncher-instance-test\", "
                       "\"minecraft_version\": \"1.21.1\", "
                       "\"loader\": \"vanilla\", \"jvm_args\": null}",
                       TEST_UUID);
    g_autoptr (JsonParser) parser = parse_json (text);
    g_autoptr (nuvashalauncherInstance) instance =
      nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);

    g_assert_null (instance);
    g_assert_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID);
    g_clear_error (&error);
  }

  g_print ("ok - malformed json fails cleanly\n");
}

static void
test_optional_fields (void)
{
  g_autoptr (GError) error = NULL;

  /* Absent optional fields become NULL, "jvm_args" gets the default, which
   * is none. */
  g_autofree char *text =
    g_strdup_printf ("{\"id\": \"%s\", \"name\": \"Minimal\", "
                     "\"directory\": \"/tmp/nuvashalauncher-instance-minimal\", "
                     "\"minecraft_version\": \"1.21.1\", "
                     "\"loader\": \"vanilla\"}",
                     TEST_UUID);
  g_autoptr (JsonParser) parser = parse_json (text);
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_new_from_json (json_parser_get_root (parser), &error);

  g_assert_no_error (error);
  g_assert_nonnull (instance);

  g_autofree char *loader_version = prop_string (instance, "loader-version");
  g_autofree char *java_path = prop_string (instance, "java-path");
  g_autofree char *jvm_args = prop_string (instance, "jvm-args");

  g_assert_null (loader_version);
  g_assert_null (java_path);
  g_assert_cmpstr (jvm_args, ==, "");

  g_print ("ok - optional fields\n");
}

static void
test_components (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstance) instance =
    nuvashalauncher_instance_new (TEST_UUID, "Components", "/tmp/components-instance",
                      "1.20.1", nuvashalauncher_LOADER_FABRIC, &error);
  g_autofree char *minecraft = NULL;
  g_autofree char *loader = NULL;

  g_assert_no_error (error);
  g_assert_true (g_str_equal (nuvashalauncher_instance_component_id_for_loader (
                                nuvashalauncher_LOADER_FABRIC), nuvashalauncher_COMPONENT_FABRIC));

  minecraft = nuvashalauncher_instance_component_get_version (instance,
                                                   nuvashalauncher_COMPONENT_MINECRAFT);
  loader = nuvashalauncher_instance_component_get_version (instance, nuvashalauncher_COMPONENT_FABRIC);
  g_assert_cmpstr (minecraft, ==, "1.20.1");
  g_assert_null (loader);

  g_assert_true (nuvashalauncher_instance_component_set_version (
    instance, nuvashalauncher_COMPONENT_FABRIC, "0.15.11", &error));
  g_assert_no_error (error);
  g_clear_pointer (&loader, g_free);
  loader = nuvashalauncher_instance_component_get_version (instance, nuvashalauncher_COMPONENT_FABRIC);
  g_assert_cmpstr (loader, ==, "0.15.11");

  g_print ("ok - component versions use stable component ids\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  test_loader_strings ();
  test_sanitize_name ();
  test_normalize_name ();
  test_jvm_args_survive_null ();
  test_new_defaults ();
  test_new_rejects_bad_values ();
  test_validate ();
  test_json_round_trip ();
  test_missing_fields ();
  test_malformed_json ();
  test_optional_fields ();
  test_components ();

  return EXIT_SUCCESS;
}
