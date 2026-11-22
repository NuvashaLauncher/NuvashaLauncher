#include "test-modpack-common.h"

static void
test_export_import_round_trip (void)
{
  g_autofree char *root = make_tmp_dir ();
  g_autofree char *source_data = g_build_filename (root, "source-data", NULL);
  g_autofree char *target_data = g_build_filename (root, "target-data", NULL);
  g_autofree char *source_dir = g_build_filename (root, "source-instance", NULL);
  g_autofree char *icon_source = g_build_filename (root, "source-icon.png", NULL);
  g_autofree char *archive = g_build_filename (root, "round-trip.mrpack", NULL);
  g_autofree char *override = g_build_filename (source_dir, "config", "test.txt", NULL);
  g_autofree char *log = g_build_filename (source_dir, "logs", "latest.log", NULL);
  g_autofree char *index =
    g_build_filename (source_dir, "mods", ".index", "index.json", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherInstanceManager) source_manager = manager_new (source_data);
  g_autoptr (NuvashaLauncherInstanceManager) target_manager = manager_new (target_data);
  g_autoptr (NuvashaLauncherInstance) source = NULL;
  g_autoptr (NuvashaLauncherModpack) pack = NULL;
  g_autoptr (NuvashaLauncherInstance) imported = NULL;
  g_autofree char *imported_dir = NULL;
  g_autofree char *imported_icon = NULL;
  g_autofree char *icon_bytes = NULL;
  g_autofree char *override_bytes = NULL;
  NuvashaLauncherLoader loader = NUVASHALAUNCHER_LOADER_VANILLA;

  source = nuvashalauncher_instance_manager_create (source_manager, "Round Trip",
                                        source_dir, "1.20.1", NUVASHALAUNCHER_LOADER_FABRIC,
                                        "0.15.11", &error);
  g_assert_no_error (error);
  g_assert_nonnull (source);
  g_assert_true (g_file_set_contents (icon_source, "icon bytes", -1, &error));
  g_assert_no_error (error);
  {
    g_autofree char *override_dir = g_path_get_dirname (override);

    g_assert_cmpint (g_mkdir_with_parents (override_dir, 0755), ==, 0);
  }
  g_assert_true (g_file_set_contents (override, "override bytes", -1, &error));
  g_assert_no_error (error);
  {
    g_autofree char *log_dir = g_path_get_dirname (log);

    g_assert_cmpint (g_mkdir_with_parents (log_dir, 0755), ==, 0);
  }
  g_assert_true (g_file_set_contents (log, "transient bytes", -1, &error));
  g_assert_no_error (error);
  {
    g_autofree char *index_dir = g_path_get_dirname (index);

    g_assert_cmpint (g_mkdir_with_parents (index_dir, 0755), ==, 0);
  }
  g_assert_true (g_file_set_contents (index, "internal index", -1, &error));
  g_assert_no_error (error);
  g_object_set (source, "icon", icon_source, NULL);
  g_assert_true (nuvashalauncher_instance_manager_save (source_manager, &error));
  g_assert_no_error (error);

  g_assert_true (export_now (source, archive, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (archive, G_FILE_TEST_IS_REGULAR));

  {
    struct archive *reader = archive_read_new ();
    struct archive_entry *entry = NULL;
    guint icon_entries = 0;

    g_assert_cmpint (archive_read_support_format_zip (reader), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_read_open_filename (reader, archive, 10240), ==,
                     ARCHIVE_OK);
    while (archive_read_next_header (reader, &entry) == ARCHIVE_OK)
      if (g_strcmp0 (archive_entry_pathname (entry), "overrides/icon.png") ==
          0)
        icon_entries++;
    g_assert_cmpint (archive_read_free (reader), ==, ARCHIVE_OK);
    g_assert_cmpuint (icon_entries, ==, 1);
  }

  pack = nuvashalauncher_modpack_new_from_zip (archive, &error);
  g_assert_no_error (error);
  g_assert_nonnull (pack);
  g_assert_true (nuvashalauncher_modpack_resolve (pack, &error));
  g_assert_no_error (error);
  g_assert_cmpint (pack->loader, ==, NUVASHALAUNCHER_LOADER_FABRIC);

  /* The manifest loader is what this round trip verifies. Avoid asking the
   * live Fabric service during tests; the actual files, icon and overrides
   * are independent of the loader installer lookup. */
  pack->loader = NUVASHALAUNCHER_LOADER_VANILLA;
  g_clear_pointer (&pack->loader_version, g_free);

  imported = install_now (pack, target_manager, "Round Trip", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (imported);
  g_object_get (imported, "loader", &loader, "directory", &imported_dir,
                "icon", &imported_icon, NULL);
  g_assert_cmpint (loader, ==, NUVASHALAUNCHER_LOADER_VANILLA);
  g_assert_nonnull (imported_icon);
  icon_bytes = read_file (imported_icon);
  g_assert_cmpstr (icon_bytes, ==, "icon bytes");

  {
    g_autofree char *imported_override =
      g_build_filename (imported_dir, "config", "test.txt", NULL);
    override_bytes = read_file (imported_override);
  }
  g_assert_cmpstr (override_bytes, ==, "override bytes");
  {
    g_autofree char *imported_log =
      g_build_filename (imported_dir, "logs", "latest.log", NULL);
    g_autofree char *imported_index =
      g_build_filename (imported_dir, "mods", ".index", "index.json", NULL);

    g_assert_false (g_file_test (imported_log, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (imported_index, G_FILE_TEST_EXISTS));
  }

  remove_recursive (root);
  g_print ("ok - export and import preserve loader, icon and overrides\n");
}

static void
test_dialog_imports_and_installs (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *index_json =
    pack_index ("Dialog Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autoptr (NuvashaLauncherInstanceManager) manager = NULL;
  g_autoptr (NuvashaLauncherDownloadModpackDialog) dialog = NULL;
  g_autoptr (GtkWindow) window = NULL;
  GtkButton *anchor;
  ZipEntry entries[] = {
    { "modrinth.index.json", index_json, strlen (index_json), AE_IFREG,
      NULL },
    ZIP_FILE ("overrides/config/options.txt", "from overrides"),
    ZIP_FILE ("overrides/icon.png", "icon bytes"),
  };
  gboolean finished = FALSE;
  gint64 deadline = 0;

  zip_write (zip_path, entries, G_N_ELEMENTS (entries));
  manager = manager_new (data_dir);

  dialog = nuvashalauncher_download_modpack_dialog_new_for_file (manager, zip_path);
  g_assert_nonnull (dialog);
  /* The constructor returns a floating dialog, as GTK widget constructors
   * do. Keep the reference owned by this test rather than relying on an
   * unrelated in-flight browser request to keep the dialog alive. */
  g_object_ref_sink (dialog);
  window = GTK_WINDOW (g_object_ref_sink (gtk_window_new ()));
  anchor = GTK_BUTTON (gtk_button_new ());
  gtk_window_set_child (window, GTK_WIDGET (anchor));
  gtk_widget_set_opacity (GTK_WIDGET (window), 0.0);
  gtk_widget_set_focusable (GTK_WIDGET (window), FALSE);
  gtk_window_present (window);
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (anchor));

  /* The import runs on the main loop: the pack is read, the install goes
     on, and both release the operations they started. */
  deadline = g_get_monotonic_time () + 30 * G_TIME_SPAN_SECOND;

  while (!finished && g_get_monotonic_time () < deadline)
    {
      if (manager_count (manager) > 0)
        finished = TRUE;

      g_main_context_iteration (NULL, FALSE);
    }

  g_assert_true (finished);
  g_assert_cmpuint (manager_count (manager), ==, 1);

  g_assert_true (adw_dialog_close (ADW_DIALOG (dialog)));
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);
  gtk_window_destroy (window);
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);
  remove_recursive (dir);
  g_print ("ok - the dialog reads a pack and installs it in one go\n");
}

static void
write_nested_file (const char *base, const char *rel, const char *content)
{
  g_autofree char *path = g_build_filename (base, rel, NULL);
  g_autofree char *parent = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);
  g_assert_true (g_file_set_contents (path, content, -1, &error));
  g_assert_no_error (error);
}

static void
test_export_atomic_failure_and_exclusions (void)
{
  g_autofree char *root = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (root, "data", NULL);
  g_autofree char *inst_dir = g_build_filename (root, "inst", NULL);
  g_autofree char *bad_dest = g_build_filename (root, "missing-dir", "out.mrpack", NULL);
  g_autofree char *good_dest = g_build_filename (root, "out.mrpack", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherInstanceManager) manager = manager_new (data_dir);
  g_autoptr (NuvashaLauncherInstance) inst = NULL;

  inst = nuvashalauncher_instance_manager_create (
    manager, "ExportCheck", inst_dir, "1.21.1",
    NUVASHALAUNCHER_LOADER_VANILLA, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  write_nested_file (inst_dir, "mods/keep.jar", "mod-data");
  write_nested_file (inst_dir, "screenshots/shot.png", "png-data");
  write_nested_file (inst_dir, "crash-reports/crash-1.txt", "crash-data");
  write_nested_file (inst_dir, "logs/latest.log", "log-data");
  write_nested_file (inst_dir, "cache/tmp.bin", "cache-data");

  /* Exporting to a nonexistent parent directory must fail cleanly without leaving artifacts. */
  g_assert_false (export_now (inst, bad_dest, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (g_file_test (bad_dest, G_FILE_TEST_EXISTS));

  /* Valid export preserves mods/ overrides and excludes transient directories. */
  g_assert_true (export_now (inst, good_dest, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (good_dest, G_FILE_TEST_IS_REGULAR));

  {
    struct archive *reader = archive_read_new ();
    struct archive_entry *entry = NULL;
    gboolean saw_mod = FALSE;
    gboolean saw_excluded = FALSE;

    g_assert_cmpint (archive_read_support_format_zip (reader), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_read_open_filename (reader, good_dest, 10240), ==, ARCHIVE_OK);
    while (archive_read_next_header (reader, &entry) == ARCHIVE_OK)
      {
        const char *name = archive_entry_pathname (entry);
        if (g_strcmp0 (name, "overrides/mods/keep.jar") == 0)
          saw_mod = TRUE;
        if (strstr (name, "screenshots/") != NULL ||
            strstr (name, "crash-reports/") != NULL ||
            strstr (name, "logs/") != NULL ||
            strstr (name, "cache/") != NULL)
          saw_excluded = TRUE;
      }
    g_assert_cmpint (archive_read_free (reader), ==, ARCHIVE_OK);
    g_assert_true (saw_mod);
    g_assert_false (saw_excluded);
  }

  remove_recursive (root);
  g_print ("ok - export failure is atomic and excludes transient directories\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/modpack/export-import-round-trip", test_export_import_round_trip);
  g_test_add_func ("/modpack/export-exclusions-and-failure",
                   test_export_atomic_failure_and_exclusions);
  if (gtk_init_check ())
    {
      g_test_add_func ("/modpack/dialog-import", test_dialog_imports_and_installs);
    }
  return g_test_run ();
}
