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
  g_autoptr (nuvashalauncherInstanceManager) source_manager = manager_new (source_data);
  g_autoptr (nuvashalauncherInstanceManager) target_manager = manager_new (target_data);
  g_autoptr (nuvashalauncherInstance) source = NULL;
  g_autoptr (nuvashalauncherModpack) pack = NULL;
  g_autoptr (nuvashalauncherInstance) imported = NULL;
  g_autofree char *imported_dir = NULL;
  g_autofree char *imported_icon = NULL;
  g_autofree char *icon_bytes = NULL;
  g_autofree char *override_bytes = NULL;
  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;

  source = nuvashalauncher_instance_manager_create (source_manager, "Round Trip",
                                        source_dir, "1.20.1", nuvashalauncher_LOADER_FABRIC,
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
  g_assert_cmpint (pack->loader, ==, nuvashalauncher_LOADER_FABRIC);

  /* The manifest loader is what this round trip verifies. Avoid asking the
   * live Fabric service during tests; the actual files, icon and overrides
   * are independent of the loader installer lookup. */
  pack->loader = nuvashalauncher_LOADER_VANILLA;
  g_clear_pointer (&pack->loader_version, g_free);

  imported = install_now (pack, target_manager, "Round Trip", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (imported);
  g_object_get (imported, "loader", &loader, "directory", &imported_dir,
                "icon", &imported_icon, NULL);
  g_assert_cmpint (loader, ==, nuvashalauncher_LOADER_VANILLA);
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
test_export_preserves_managed_datapack (void)
{
  g_autofree char *root = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (root, "data", NULL);
  g_autofree char *source_dir = g_build_filename (root, "source", NULL);
  g_autofree char *datapack_dir = g_build_filename (source_dir, "datapacks",
                                                    NULL);
  g_autofree char *datapack = g_build_filename (source_dir, "datapacks",
                                                "example.zip", NULL);
  g_autofree char *archive = g_build_filename (root, "managed.mrpack", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherInstanceManager) manager = manager_new (data_dir);
  g_autoptr (nuvashalauncherInstance) instance = NULL;
  g_autoptr (nuvashalauncherModpack) exported = NULL;
  nuvashalauncherContentIndexEntry entry = { 0 };
  GStatBuf stat_buf;

  instance = nuvashalauncher_instance_manager_create (manager, "Managed Pack",
                                               source_dir, "1.20.1",
                                               nuvashalauncher_LOADER_FABRIC, "0.15.11",
                                               &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);
  g_assert_cmpint (g_mkdir_with_parents (datapack_dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (datapack, "datapack bytes", -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_stat (datapack, &stat_buf), ==, 0);

  entry.filename = "example.zip";
  entry.size = stat_buf.st_size;
  entry.mtime_usec = (gint64) stat_buf.st_mtim.tv_sec * G_USEC_PER_SEC +
                     stat_buf.st_mtim.tv_nsec / 1000;
  entry.hash = "0123456789abcdef0123456789abcdef01234567";
  entry.hash512 = "0123456789abcdef0123456789abcdef";
  entry.download_url = "https://cdn.modrinth.com/data/pack/v1/example.zip";
  entry.env_client = "required";
  entry.env_server = "required";
  entry.provider = "Modrinth";
  entry.project_id = "pack";
  entry.version_id = "v1";

  g_assert_true (index_update_now (source_dir, nuvashalauncher_CONTENT_DATAPACK,
                                   &entry, &error));
  g_assert_no_error (error);
  g_object_set (instance,
                "modpack-name", "Managed Pack Original",
                "modpack-summary", "kept summary",
                "modpack-version-id", "original-version",
                NULL);
  g_assert_true (nuvashalauncher_instance_manager_save (manager, &error));
  g_assert_no_error (error);

  g_assert_true (export_now (instance, archive, &error));
  g_assert_no_error (error);
  exported = nuvashalauncher_modpack_new_from_zip (archive, &error);
  g_assert_no_error (error);
  g_assert_nonnull (exported);
  g_assert_cmpstr (exported->name, ==, "Managed Pack Original");
  g_assert_cmpstr (exported->summary, ==, "kept summary");
  g_assert_cmpstr (exported->version_id, ==, "original-version");
  g_assert_cmpuint (exported->files->len, ==, 1);
  {
    nuvashalauncherModpackFile *file = g_ptr_array_index (exported->files, 0);

    g_assert_cmpstr (file->path, ==, "datapacks/example.zip");
    g_assert_cmpstr (file->sha1, ==,
                     "0123456789abcdef0123456789abcdef01234567");
    g_assert_cmpint (file->env_client, ==, nuvashalauncher_MODPACK_ENV_REQUIRED);
    g_assert_cmpint (file->env_server, ==, nuvashalauncher_MODPACK_ENV_REQUIRED);
    g_assert_cmpstr (g_ptr_array_index (file->downloads, 0), ==,
                     "https://cdn.modrinth.com/data/pack/v1/example.zip");
  }

  remove_recursive (root);
  g_print ("ok - export preserves managed datapacks and metadata\n");
}

static void
test_dialog_imports_and_installs (void)
{
  g_autofree char *dir = make_tmp_dir ();
  g_autofree char *data_dir = g_build_filename (dir, "data", NULL);
  g_autofree char *zip_path = g_build_filename (dir, "pack.mrpack", NULL);
  g_autofree char *index_json =
    pack_index ("Dialog Pack", "", "\"minecraft\":\"1.20.1\"");
  g_autoptr (nuvashalauncherInstanceManager) manager = NULL;
  g_autoptr (nuvashalauncherDownloadModpackDialog) dialog = NULL;
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/modpack/export-import-round-trip", test_export_import_round_trip);
  g_test_add_func ("/modpack/export-managed-datapack", test_export_preserves_managed_datapack);
#if !defined(__SANITIZE_ADDRESS__)
  if (gtk_init_check ())
    {
      g_test_add_func ("/modpack/dialog-import", test_dialog_imports_and_installs);
    }
#endif
  return g_test_run ();
}
