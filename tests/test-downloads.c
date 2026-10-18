/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for comfy-downloads: the process wide list of downloads that are
 * running, which the header of the main window shows.
 *
 * Nothing here needs a display and nothing touches the network: the
 * manager is asked about the entries themselves. A test binary is a
 * process of its own, so the singleton starts empty. */

#include "comfy-downloads.h"

#include <glib.h>
#include <stdlib.h>

/* Every change the manager reports, counted through its signal. */
static guint changed = 0;

static void
on_changed (ComfyDownloads *downloads G_GNUC_UNUSED, gpointer user_data)
{
  guint *count = user_data;

  (*count)++;
}

static void
test_add_and_finish (void)
{
  ComfyDownloads *downloads = comfy_downloads_get_default ();
  ComfyDownload *one = NULL;
  ComfyDownload *two = NULL;

  g_signal_connect (downloads, "changed", G_CALLBACK (on_changed), &changed);

  g_assert_cmpuint (comfy_downloads_count (), ==, 0);
  changed = 0;

  one = comfy_downloads_add (comfy_DOWNLOAD_MODPACK, "A pack");
  g_assert_nonnull (one);
  g_assert_cmpuint (comfy_downloads_count (), ==, 1);
  g_assert_cmpuint (changed, ==, 1);
  g_assert_cmpstr (comfy_download_get_title (one), ==, "A pack");
  g_assert_cmpint (comfy_download_get_kind (one), ==, comfy_DOWNLOAD_MODPACK);
  g_assert_nonnull (comfy_download_get_cancellable (one));

  two = comfy_downloads_add (comfy_DOWNLOAD_MOD, "A mod");
  g_assert_cmpuint (comfy_downloads_count (), ==, 2);

  comfy_download_finish (one);
  g_assert_cmpuint (comfy_downloads_count (), ==, 1);

  comfy_download_finish (two);
  g_assert_cmpuint (comfy_downloads_count (), ==, 0);

  g_print ("ok - an entry is added, counted and finished\n");
}

static void
test_progress_and_detail (void)
{
  ComfyDownload *download = comfy_downloads_add (comfy_DOWNLOAD_SHADER, "Shaders");
  guint changes = 0;

  changed = 0;

  g_assert_cmpuint ((guint) comfy_download_get_done (download), ==, 0);
  g_assert_cmpuint ((guint) comfy_download_get_total (download), ==, 0);
  g_assert_cmpint (comfy_download_get_unit (download), ==,
                   comfy_DOWNLOAD_UNIT_COUNT);
  g_assert_null (comfy_download_get_detail (download));

  comfy_download_set_detail (download, "0 of 4 files");
  g_assert_cmpstr (comfy_download_get_detail (download), ==, "0 of 4 files");

  comfy_download_set_progress (download, 2, 4);
  g_assert_cmpuint ((guint) comfy_download_get_done (download), ==, 2);
  g_assert_cmpuint ((guint) comfy_download_get_total (download), ==, 4);
  g_assert_cmpint (comfy_download_get_unit (download), ==,
                   comfy_DOWNLOAD_UNIT_COUNT);

  /* Setting the same values again changes nothing and says nothing. */
  changes = changed;
  comfy_download_set_progress (download, 2, 4);
  comfy_download_set_detail (download, "0 of 4 files");
  g_assert_cmpuint (changed, ==, changes);

  /* A single transfer is counted in bytes and says so. */
  comfy_download_set_bytes (download, 300, 500);
  g_assert_cmpint (comfy_download_get_unit (download), ==,
                   comfy_DOWNLOAD_UNIT_BYTES);
  g_assert_cmpuint ((guint) comfy_download_get_done (download), ==, 300);
  g_assert_cmpuint ((guint) comfy_download_get_total (download), ==, 500);
  g_assert_cmpuint (changed, >, changes);

  comfy_download_finish (download);
  g_print ("ok - the detail and the progress are kept and only change once\n");
}

static void
test_cancel (void)
{
  ComfyDownload *download = comfy_downloads_add (comfy_DOWNLOAD_RESOURCE_PACK, "Pack");
  GCancellable *cancellable = comfy_download_get_cancellable (download);

  g_assert_false (g_cancellable_is_cancelled (cancellable));

  comfy_download_cancel (download);
  g_assert_true (g_cancellable_is_cancelled (cancellable));

  comfy_download_finish (download);
  g_print ("ok - cancelling the entry cancels its cancellable\n");
}

static void
test_all_lists_every_entry (void)
{
  GPtrArray *all;
  ComfyDownload *one = comfy_downloads_add (comfy_DOWNLOAD_UPDATE, "One");
  ComfyDownload *two = comfy_downloads_add (comfy_DOWNLOAD_UPDATE, "Two");

  all = comfy_downloads_get_all ();
  g_assert_cmpuint (all->len, ==, 2);
  g_assert_true (g_ptr_array_index (all, 0) == one);
  g_assert_true (g_ptr_array_index (all, 1) == two);

  comfy_download_finish (one);
  comfy_download_finish (two);
  g_assert_cmpuint (comfy_downloads_count (), ==, 0);

  g_print ("ok - the list holds every running download in order\n");
}

/* What the content pages have picked, which the header counts on the
 * icon of its downloads button: what one page adds and another takes
 * away, and a count that never falls below what is really picked. */
static void
test_picked_count (void)
{
  g_assert_cmpuint (comfy_downloads_picked (), ==, 0);

  comfy_downloads_picked_add (3);
  g_assert_cmpuint (comfy_downloads_picked (), ==, 3);

  /* Two content pages at once: one has four files on its list and the
     other one, so what the header shows is the sum. */
  comfy_downloads_picked_add (1);
  g_assert_cmpuint (comfy_downloads_picked (), ==, 4);

  comfy_downloads_picked_add (-1);
  g_assert_cmpuint (comfy_downloads_picked (), ==, 3);

  comfy_downloads_picked_add (-3);
  g_assert_cmpuint (comfy_downloads_picked (), ==, 0);

  /* A page that takes back more than it said has is not a number the
     header would draw. */
  comfy_downloads_picked_add (-2);
  g_assert_cmpuint (comfy_downloads_picked (), ==, 0);

  /* Nothing is running and nothing is picked: no entry, no button. */
  g_assert_cmpuint (comfy_downloads_count (), ==, 0);

  g_print ("ok - the picked count is what the open pages have on\n");
}

int
main (void)
{
  g_print ("# Download tests\n");

  test_add_and_finish ();
  test_progress_and_detail ();
  test_cancel ();
  test_all_lists_every_entry ();
  test_picked_count ();

  /* The singleton holds the one reference it was created with; giving it
     back keeps the sanitizer run from calling the manager a leak. */
  g_object_unref (comfy_downloads_get_default ());

  return EXIT_SUCCESS;
}
