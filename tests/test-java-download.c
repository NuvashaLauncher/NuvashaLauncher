/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for Eclipse Temurin discovery and installation (nuvashalauncher-java-download). */
#define _GNU_SOURCE

#include "nuvashalauncher-java-download.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-fs.h"

#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  SoupServer *server;
  char *base;
  char *good_tar_gz;
  gsize good_tar_gz_len;
  char *good_sha256;
  char *evil_tar_gz;
  gsize evil_tar_gz_len;
  char *evil_sha256;
  char *symlink_escape_tar_gz;
  gsize symlink_escape_tar_gz_len;
  char *symlink_escape_sha256;
} Fixture;

static Fixture fixture;

static gboolean
build_tar_gz_memory (const char *const *paths,
                     const char *const *contents,
                     const mode_t *modes,
                     const char *const *symlinks,
                     gsize count,
                     char **out_data,
                     gsize *out_len)
{
  struct archive *a = archive_write_new ();
  gsize capacity = 65536;
  char *buffer = g_malloc (capacity);
  size_t used = 0;

  archive_write_add_filter_gzip (a);
  archive_write_set_format_pax_restricted (a);
  g_assert_cmpint (archive_write_open_memory (a, buffer, capacity, &used), ==, ARCHIVE_OK);

  for (gsize i = 0; i < count; i++)
    {
      struct archive_entry *entry = archive_entry_new ();
      archive_entry_set_pathname (entry, paths[i]);
      archive_entry_set_perm (entry, modes[i] & 0777);

      if (symlinks != NULL && symlinks[i] != NULL)
        {
          archive_entry_set_filetype (entry, AE_IFLNK);
          archive_entry_set_symlink (entry, symlinks[i]);
          archive_entry_set_size (entry, 0);
          g_assert_cmpint (archive_write_header (a, entry), ==, ARCHIVE_OK);
        }
      else if (S_ISDIR (modes[i]))
        {
          archive_entry_set_filetype (entry, AE_IFDIR);
          archive_entry_set_size (entry, 0);
          g_assert_cmpint (archive_write_header (a, entry), ==, ARCHIVE_OK);
        }
      else
        {
          gsize len = contents[i] != NULL ? strlen (contents[i]) : 0;
          archive_entry_set_filetype (entry, AE_IFREG);
          archive_entry_set_size (entry, (la_int64_t) len);
          g_assert_cmpint (archive_write_header (a, entry), ==, ARCHIVE_OK);
          if (len > 0)
            g_assert_cmpint (archive_write_data (a, contents[i], len), ==, (la_ssize_t) len);
        }

      archive_entry_free (entry);
    }

  g_assert_cmpint (archive_write_close (a), ==, ARCHIVE_OK);
  archive_write_free (a);

  *out_data = buffer;
  *out_len = used;
  return TRUE;
}

static void
reply_with (SoupServerMessage *message, guint status,
            const char *content_type, const char *body, gsize length)
{
  soup_server_message_set_status (message, status, NULL);
  if (body != NULL)
    soup_server_message_set_response (message, content_type, SOUP_MEMORY_COPY,
                                      body, length);
}

static void
server_handler (SoupServer *server G_GNUC_UNUSED,
                SoupServerMessage *message,
                const char *path,
                GHashTable *query,
                gpointer user_data)
{
  Fixture *self = user_data;
  const char *host_arch = nuvashalauncher_java_download_host_arch ();

  if (g_str_equal (path, "/info/available_releases"))
    {
      const char *json =
        "{\"available_releases\":[8,17,21,24],"
        "\"available_lts_releases\":[8,17,21]}";
      reply_with (message, 200, "application/json", json, strlen (json));
      return;
    }

  if (g_str_equal (path, "/assets/latest/21/hotspot"))
    {
      const char *req_arch = query != NULL ? g_hash_table_lookup (query, "architecture") : NULL;
      g_assert_cmpstr (req_arch, ==, host_arch);

      g_autofree char *json = g_strdup_printf (
        "[{\"release_name\":\"jdk-21.0.5+11\","
        "\"version\":{\"major\":21,\"semver\":\"21.0.5+11\"},"
        "\"binary\":{\"os\":\"linux\",\"architecture\":\"%s\",\"image_type\":\"jdk\","
        "\"package\":{\"name\":\"OpenJDK21U-jdk_%s_linux_hotspot_21.0.5_11.tar.gz\","
        "\"link\":\"%s/files/temurin-21.tar.gz\","
        "\"checksum\":\"%s\",\"size\":%" G_GSIZE_FORMAT "}}}]",
        host_arch, host_arch, self->base, self->good_sha256, self->good_tar_gz_len);
      reply_with (message, 200, "application/json", json, strlen (json));
      return;
    }

  if (g_str_equal (path, "/assets/latest/17/hotspot"))
    {
      g_autofree char *json = g_strdup_printf (
        "[{\"release_name\":\"jdk-17.0.13+11\","
        "\"version\":{\"major\":17,\"semver\":\"17.0.13+11\"},"
        "\"binary\":{\"os\":\"linux\",\"architecture\":\"%s\",\"image_type\":\"jdk\","
        "\"package\":{\"name\":\"OpenJDK17U-jdk_%s_linux_hotspot_17.0.13_11.tar.gz\","
        "\"link\":\"%s/files/temurin-21.tar.gz\","
        "\"checksum\":\"%s\",\"size\":%" G_GSIZE_FORMAT "}}}]",
        host_arch, host_arch, self->base, self->good_sha256, self->good_tar_gz_len);
      reply_with (message, 200, "application/json", json, strlen (json));
      return;
    }

  if (g_str_equal (path, "/assets/latest/8/hotspot"))
    {
      /* Return a mismatched architecture to confirm host-arch filtering. */
      const char *other_arch = g_str_equal (host_arch, "x64") ? "aarch64" : "x64";
      g_autofree char *json = g_strdup_printf (
        "[{\"release_name\":\"jdk8u432-b06\","
        "\"version\":{\"major\":8,\"semver\":\"8.0.432+6\"},"
        "\"binary\":{\"os\":\"linux\",\"architecture\":\"%s\",\"image_type\":\"jdk\","
        "\"package\":{\"name\":\"OpenJDK8U.tar.gz\","
        "\"link\":\"%s/files/temurin-21.tar.gz\","
        "\"checksum\":\"%s\",\"size\":%" G_GSIZE_FORMAT "}}}]",
        other_arch, self->base, self->good_sha256, self->good_tar_gz_len);
      reply_with (message, 200, "application/json", json, strlen (json));
      return;
    }

  if (g_str_equal (path, "/assets/latest/24/hotspot"))
    {
      /* 404 for a version unavailable on this arch should be skipped cleanly. */
      reply_with (message, 404, "text/plain", "not found", 9);
      return;
    }

  if (g_str_equal (path, "/files/temurin-21.tar.gz"))
    {
      reply_with (message, 200, "application/gzip",
                  self->good_tar_gz, self->good_tar_gz_len);
      return;
    }

  if (g_str_equal (path, "/files/evil.tar.gz"))
    {
      reply_with (message, 200, "application/gzip",
                  self->evil_tar_gz, self->evil_tar_gz_len);
      return;
    }

  if (g_str_equal (path, "/files/symlink-escape.tar.gz"))
    {
      reply_with (message, 200, "application/gzip",
                  self->symlink_escape_tar_gz, self->symlink_escape_tar_gz_len);
      return;
    }

  reply_with (message, 404, NULL, NULL, 0);
}

static void
fixture_setup (void)
{
  g_autoptr (GError) error = NULL;
  GSList *uris;
  GUri *uri;

  {
    const char *const paths[] = {
      "jdk-21.0.5+11/",
      "jdk-21.0.5+11/bin/",
      "jdk-21.0.5+11/bin/java",
      "jdk-21.0.5+11/lib/",
      "jdk-21.0.5+11/lib/libawt_xawt.so",
      "jdk-21.0.5+11/lib/link-to-java"
    };
    const char *const contents[] = {
      NULL, NULL, "#!/bin/sh\necho java\n", NULL, "ELF", NULL
    };
    const mode_t modes[] = {
      S_IFDIR | 0755, S_IFDIR | 0755, S_IFREG | 0755,
      S_IFDIR | 0755, S_IFREG | 0644, S_IFLNK | 0777
    };
    const char *const symlinks[] = {
      NULL, NULL, NULL, NULL, NULL, "../bin/java"
    };

    build_tar_gz_memory (paths, contents, modes, symlinks, G_N_ELEMENTS (paths),
                         &fixture.good_tar_gz, &fixture.good_tar_gz_len);
    fixture.good_sha256 = g_compute_checksum_for_data (
      G_CHECKSUM_SHA256, (const guchar *) fixture.good_tar_gz, fixture.good_tar_gz_len);
  }

  {
    const char *const paths[] = {
      "jdk-21.0.5+11/",
      "jdk-21.0.5+11/../../escape.txt"
    };
    const char *const contents[] = { NULL, "pwn" };
    const mode_t modes[] = { S_IFDIR | 0755, S_IFREG | 0644 };

    build_tar_gz_memory (paths, contents, modes, NULL, G_N_ELEMENTS (paths),
                         &fixture.evil_tar_gz, &fixture.evil_tar_gz_len);
    fixture.evil_sha256 = g_compute_checksum_for_data (
      G_CHECKSUM_SHA256, (const guchar *) fixture.evil_tar_gz, fixture.evil_tar_gz_len);
  }

  {
    const char *const paths[] = {
      "jdk-21.0.5+11/",
      "jdk-21.0.5+11/bin/",
      "jdk-21.0.5+11/bin/java",
      "jdk-21.0.5+11/bin/badlink"
    };
    const char *const contents[] = { NULL, NULL, "#!/bin/sh\n", NULL };
    const mode_t modes[] = {
      S_IFDIR | 0755, S_IFDIR | 0755, S_IFREG | 0755, S_IFLNK | 0777
    };
    const char *const symlinks[] = { NULL, NULL, NULL, "../../etc/passwd" };

    build_tar_gz_memory (paths, contents, modes, symlinks, G_N_ELEMENTS (paths),
                         &fixture.symlink_escape_tar_gz,
                         &fixture.symlink_escape_tar_gz_len);
    fixture.symlink_escape_sha256 = g_compute_checksum_for_data (
      G_CHECKSUM_SHA256, (const guchar *) fixture.symlink_escape_tar_gz,
      fixture.symlink_escape_tar_gz_len);
  }

  fixture.server = soup_server_new (NULL, NULL);
  soup_server_add_handler (fixture.server, NULL, server_handler, &fixture, NULL);
  g_assert_true (soup_server_listen_local (fixture.server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error (error);

  uris = soup_server_get_uris (fixture.server);
  g_assert_nonnull (uris);
  uri = uris->data;
  fixture.base = g_strdup_printf ("http://127.0.0.1:%d", g_uri_get_port (uri));
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
}

static void
fixture_teardown (void)
{
  if (fixture.server != NULL)
    {
      soup_server_disconnect (fixture.server);
      g_clear_object (&fixture.server);
    }
  g_clear_pointer (&fixture.base, g_free);
  g_clear_pointer (&fixture.good_tar_gz, g_free);
  g_clear_pointer (&fixture.good_sha256, g_free);
  g_clear_pointer (&fixture.evil_tar_gz, g_free);
  g_clear_pointer (&fixture.evil_sha256, g_free);
  g_clear_pointer (&fixture.symlink_escape_tar_gz, g_free);
  g_clear_pointer (&fixture.symlink_escape_sha256, g_free);
}

typedef struct {
  GMainLoop *loop;
  GPtrArray *releases;
  char *installed_path;
  GError *error;
  gboolean saw_downloading;
  gboolean saw_extracting;
} AsyncCall;

static void
on_list_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;
  call->releases = nuvashalauncher_java_download_list_finish (result, &call->error);
  g_main_loop_quit (call->loop);
}

static void
on_install_progress (NuvashaLauncherJavaDownloadStage stage,
                     goffset bytes_done G_GNUC_UNUSED,
                     goffset bytes_total G_GNUC_UNUSED,
                     gpointer user_data)
{
  AsyncCall *call = user_data;
  if (stage == NUVASHALAUNCHER_JAVA_DOWNLOAD_STAGE_DOWNLOADING)
    call->saw_downloading = TRUE;
  else if (stage == NUVASHALAUNCHER_JAVA_DOWNLOAD_STAGE_EXTRACTING)
    call->saw_extracting = TRUE;
}

static void
on_install_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
  AsyncCall *call = user_data;
  call->installed_path = nuvashalauncher_java_download_install_finish (result, &call->error);
  g_main_loop_quit (call->loop);
}

static void
test_host_arch (void)
{
  const char *arch = nuvashalauncher_java_download_host_arch ();
  g_assert_nonnull (arch);
  g_assert_cmpstr (arch, !=, "");
}

static void
test_list_and_install_temurin (void)
{
  NuvashaLauncherDownloadService *service = nuvashalauncher_download_service_new ();
  g_autofree char *tmp_dir = g_dir_make_tmp ("nuvashalauncher-test-java-dl-XXXXXX", NULL);
  AsyncCall list_call = { .loop = g_main_loop_new (NULL, FALSE) };
  NuvashaLauncherTaskScope *scope;

  scope = nuvashalauncher_java_download_list_async (
    service, fixture.base, on_list_done, &list_call);
  g_main_loop_run (list_call.loop);
  nuvashalauncher_task_scope_unref (scope);
  g_main_loop_unref (list_call.loop);

  g_assert_no_error (list_call.error);
  g_assert_nonnull (list_call.releases);
  /* Only 21 and 17 matched host arch; 8 had wrong arch, 24 returned 404. */
  g_assert_cmpuint (list_call.releases->len, ==, 2);

  NuvashaLauncherJavaRelease *r0 = g_ptr_array_index (list_call.releases, 0);
  NuvashaLauncherJavaRelease *r1 = g_ptr_array_index (list_call.releases, 1);
  g_assert_cmpint (r0->major_version, ==, 21);
  g_assert_true (r0->lts);
  g_assert_cmpstr (r0->version_display, ==, "21.0.5+11");
  g_assert_cmpint (r1->major_version, ==, 17);
  g_assert_true (r1->lts);

  g_assert_false (nuvashalauncher_java_download_is_installed (r0, tmp_dir));

  AsyncCall install_call = { .loop = g_main_loop_new (NULL, FALSE) };
  scope = nuvashalauncher_java_download_install_async (
    service, r0, tmp_dir, on_install_progress, on_install_done, &install_call);
  g_main_loop_run (install_call.loop);
  nuvashalauncher_task_scope_unref (scope);
  g_main_loop_unref (install_call.loop);

  g_assert_no_error (install_call.error);
  g_assert_nonnull (install_call.installed_path);
  g_assert_true (install_call.saw_extracting);
  g_assert_true (g_file_test (install_call.installed_path, G_FILE_TEST_IS_EXECUTABLE));
  g_assert_true (nuvashalauncher_java_download_is_installed (r0, tmp_dir));

  g_free (install_call.installed_path);
  g_ptr_array_unref (list_call.releases);
  nuvashalauncher_fs_remove_tree (tmp_dir);
  nuvashalauncher_download_service_free (service);
}

static void
test_rejects_unsafe_archives (void)
{
  NuvashaLauncherDownloadService *service = nuvashalauncher_download_service_new ();
  g_autofree char *tmp_dir = g_dir_make_tmp ("nuvashalauncher-test-java-sec-XXXXXX", NULL);
  g_autofree char *evil_url = g_strdup_printf ("%s/files/evil.tar.gz", fixture.base);
  g_autofree char *sym_url = g_strdup_printf ("%s/files/symlink-escape.tar.gz", fixture.base);

  NuvashaLauncherJavaRelease evil = {
    .major_version = 21,
    .release_name = "jdk-21.0.5+11",
    .download_url = evil_url,
    .sha256 = fixture.evil_sha256,
    .size = fixture.evil_tar_gz_len
  };

  AsyncCall call1 = { .loop = g_main_loop_new (NULL, FALSE) };
  NuvashaLauncherTaskScope *scope = nuvashalauncher_java_download_install_async (
    service, &evil, tmp_dir, NULL, on_install_done, &call1);
  g_main_loop_run (call1.loop);
  nuvashalauncher_task_scope_unref (scope);
  g_main_loop_unref (call1.loop);

  g_assert_null (call1.installed_path);
  g_assert_error (call1.error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                  NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE);
  g_clear_error (&call1.error);

  NuvashaLauncherJavaRelease sym = {
    .major_version = 21,
    .release_name = "jdk-21.0.5+11",
    .download_url = sym_url,
    .sha256 = fixture.symlink_escape_sha256,
    .size = fixture.symlink_escape_tar_gz_len
  };

  AsyncCall call2 = { .loop = g_main_loop_new (NULL, FALSE) };
  scope = nuvashalauncher_java_download_install_async (
    service, &sym, tmp_dir, NULL, on_install_done, &call2);
  g_main_loop_run (call2.loop);
  nuvashalauncher_task_scope_unref (scope);
  g_main_loop_unref (call2.loop);

  g_assert_null (call2.installed_path);
  g_assert_error (call2.error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                  NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE);
  g_clear_error (&call2.error);

  nuvashalauncher_fs_remove_tree (tmp_dir);
  nuvashalauncher_download_service_free (service);
}

int
main (int argc, char **argv)
{
  int status;

  g_test_init (&argc, &argv, NULL);
  fixture_setup ();

  g_test_add_func ("/java-download/host-arch", test_host_arch);
  g_test_add_func ("/java-download/list-and-install", test_list_and_install_temurin);
  g_test_add_func ("/java-download/rejects-unsafe-archives", test_rejects_unsafe_archives);

  status = g_test_run ();
  fixture_teardown ();
  return status;
}
