#include "test-modpack-common.h"
#include "nuvashalauncher-loaders-private.h"

#include <sys/stat.h>

typedef struct
{
  GMainLoop *loop;
  nuvashalauncherTaskScope *scope;
  nuvashalauncherLoaderInstall *install;
  GError *error;
} ProcessorWait;

static void
processor_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                    gpointer user_data)
{
  ProcessorWait *wait = user_data;

  wait->install = g_task_propagate_pointer (G_TASK (result), &wait->error);
  nuvashalauncher_task_scope_unref (wait->scope);
  wait->scope = NULL;
  g_main_loop_quit (wait->loop);
}

static void
run_processor_fixture (gboolean succeed)
{
  g_autofree char *directory = make_tmp_dir ();
  g_autofree char *libraries = g_build_filename (directory, "libraries", NULL);
  g_autofree char *artifact_dir = g_build_filename (
    libraries, "net/minecraftforge/forge/47.2.0", NULL);
  g_autofree char *artifact = g_build_filename (artifact_dir, "forge-47.2.0.jar", NULL);
  g_autofree char *patched = g_build_filename (directory, "patched.jar", NULL);
  g_autofree char *java = g_build_filename (directory, "java", NULL);
  g_autofree char *script = NULL;
  g_autofree char *json = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (nuvashalauncherVersionDescription) description = NULL;
  g_autoptr (GPtrArray) processors = NULL;
  g_autoptr (GPtrArray) pending = NULL;
  g_autoptr (GHashTable) variables = NULL;
  InstallRequest *request = g_new0 (InstallRequest, 1);
  ProcessorWait wait = { 0 };
  const ZipEntry entries[] = {
    ZIP_FILE ("META-INF/MANIFEST.MF",
              "Manifest-Version: 1.0\r\n"
              "Main-Class: example.Processor\r\n\r\n")
  };

  g_assert_cmpint (g_mkdir_with_parents (artifact_dir, 0700), ==, 0);
  zip_write (artifact, entries, G_N_ELEMENTS (entries));

  script = g_strdup_printf (
    "#!/bin/sh\n"
    "if test \"%s\" = success; then cp \"$2\" \"$4\"; else "
    "printf 'first detail\\nfinal processor failure\\n' >&2; exit 7; fi\n",
    succeed ? "success" : "failure");
  g_assert_true (g_file_set_contents (java, script, -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (chmod (java, 0700), ==, 0);

  json = g_strdup_printf (
    "{\"id\":\"forge-47.2.0-1.20.1\",\"type\":\"release\","
    "\"mainClass\":\"example.Main\",\"processors\":[{"
    "\"jar\":\"net.minecraftforge:forge:47.2.0\","
    "\"args\":[\"{PATCHED}\"]}]}" );
  parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  description = nuvashalauncher_version_description_new_from_json (
    "{\"id\":\"1.20.1\",\"type\":\"release\","
    "\"mainClass\":\"example.Main\"}", -1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (description);

  processors = g_ptr_array_new ();
  g_ptr_array_add (processors,
                   json_array_get_element (
                     json_node_get_array (json_object_get_member (
                       json_node_get_object (json_parser_get_root (parser)),
                       "processors")), 0));
  pending = g_ptr_array_new ();
  variables = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (variables, g_strdup ("PATCHED"), g_strdup (patched));

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.scope = nuvashalauncher_task_scope_new (NULL, NULL,
                                               processor_finished, &wait);
  request->task = g_object_ref (nuvashalauncher_task_scope_task (wait.scope));
  request->scope = wait.scope;
  request->loader = nuvashalauncher_LOADER_FORGE;
  request->stage = STAGE_RUN;
  request->index = 0;
  request->loader_version = g_strdup ("47.2.0");
  request->java = g_strdup (java);
  request->libraries_dir = g_strdup (libraries);
  request->patched_jar = g_strdup (patched);
  request->processors = g_steal_pointer (&processors);
  request->pending = g_steal_pointer (&pending);
  request->variables = g_steal_pointer (&variables);
  request->description = g_object_ref (description);

  stage_run (request);
  g_main_loop_run (wait.loop);

  g_assert_null (wait.scope);
  if (succeed)
    {
      g_assert_no_error (wait.error);
      g_assert_nonnull (wait.install);
      g_assert_cmpstr (wait.install->client_jar, ==, patched);
      {
        g_autofree char *dists =
          nuvashalauncher_loader_jar_manifest_attribute (patched, "Minecraft-Dists");

        g_assert_cmpstr (dists, ==, "client");
      }
      nuvashalauncher_loader_install_free (wait.install);
    }
  else
    {
      g_assert_null (wait.install);
      g_assert_error (wait.error, nuvashalauncher_LOADERS_ERROR,
                      nuvashalauncher_LOADERS_ERROR_INSTALLER);
      g_assert_nonnull (strstr (wait.error->message, "final processor failure"));
    }

  g_clear_error (&wait.error);
  g_main_loop_unref (wait.loop);
  remove_recursive (directory);
}

static void
test_processor_success (void)
{
  run_processor_fixture (TRUE);
  g_print ("ok - a processor receives its classpath and patches the client\n");
}

static void
test_processor_failure_reports_output (void)
{
  run_processor_fixture (FALSE);
  g_print ("ok - a failed processor reports its final output\n");
}

static void
run_profile_fixture (const char *profile, gboolean valid_profile)
{
  g_autofree char *directory = make_tmp_dir ();
  g_autofree char *installer = g_build_filename (directory, "installer.jar", NULL);
  g_autofree char *root = g_build_filename (directory, "loader", NULL);
  g_autofree char *libraries = g_build_filename (directory, "libraries", NULL);
  g_autofree char *data_dir = g_build_filename (directory, "data", NULL);
  g_autofree char *version = g_strdup (
    "{\"id\":\"forge-47.2.0-1.20.1\",\"type\":\"release\","
    "\"mainClass\":\"example.Main\",\"libraries\":[]}");
  const ZipEntry entries[] = {
    { "install_profile.json", profile, strlen (profile), AE_IFREG, NULL },
    { "version.json", version, strlen (version), AE_IFREG, NULL },
  };
  g_autoptr (GError) error = NULL;
  InstallRequest *request = g_new0 (InstallRequest, 1);
  ProcessorWait wait = { 0 };

  zip_write (installer, entries, G_N_ELEMENTS (entries));
  g_assert_cmpint (g_mkdir_with_parents (root, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (libraries, 0700), ==, 0);

  wait.loop = g_main_loop_new (NULL, FALSE);
  wait.scope = nuvashalauncher_task_scope_new (NULL, NULL,
                                               processor_finished, &wait);
  request->task = g_object_ref (nuvashalauncher_task_scope_task (wait.scope));
  request->scope = wait.scope;
  request->loader = nuvashalauncher_LOADER_FORGE;
  request->stage = STAGE_INSTALLER;
  request->loader_version = g_strdup ("47.2.0");
  request->game_version = g_strdup ("1.20.1");
  request->data_dir = g_strdup (data_dir);
  request->root = g_strdup (root);
  request->libraries_dir = g_strdup (libraries);
  request->installer = g_strdup (installer);
  request->pending = g_ptr_array_new_with_free_func (g_free);

  install_step_done (request);
  g_main_loop_run (wait.loop);

  g_assert_null (wait.scope);
  g_assert_null (wait.install);
  if (valid_profile)
    {
      g_assert_error (wait.error, nuvashalauncher_LOADERS_ERROR,
                      nuvashalauncher_LOADERS_ERROR_INSTALLER);
      g_assert_nonnull (strstr (wait.error->message,
                                "says nothing about {MISSING}"));
    }
  else
    {
      g_assert_nonnull (wait.error);
      g_assert_nonnull (strstr (wait.error->message, "Parse error"));
    }

  g_clear_error (&wait.error);
  g_main_loop_unref (wait.loop);
  remove_recursive (directory);
}

static void
test_profile_transformation_and_variable_failure (void)
{
  run_profile_fixture (
    "{\"processors\":[{\"sides\":[\"client\"],"
    "\"args\":[\"{MISSING}\"]}],\"json\":\"/version.json\"}", TRUE);
  g_print ("ok - loader profiles transform before processor variables resolve\n");
}

static void
test_malformed_install_profile_fails_cleanly (void)
{
  run_profile_fixture ("not json", FALSE);
  g_print ("ok - malformed loader profiles fail without continuing\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/loaders/processor-success", test_processor_success);
  g_test_add_func ("/loaders/processor-failure", test_processor_failure_reports_output);
  g_test_add_func ("/loaders/profile-transformation",
                   test_profile_transformation_and_variable_failure);
  g_test_add_func ("/loaders/profile-malformed",
                   test_malformed_install_profile_fails_cleanly);
  return g_test_run ();
}
