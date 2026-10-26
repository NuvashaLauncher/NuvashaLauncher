/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherJavaRow: the first entry names the runtime the automatic choice
 * stands for and that runtime is left out of the list below, so it is
 * shown once.
 *
 * Naming the first entry fills the list again, which makes the combo row
 * emit its selection and calls the caller's handler. A handler that sets
 * the requirement (which is what the instance page does) used to call
 * back into itself without end and overflow the stack. This checks the
 * calls stay bounded and that the automatic runtime is not repeated.
 *
 * Detection scans the machine for Java runtimes; that is local and needs
 * no network. Needs a display to build the row, and skips itself when
 * there is none. */

#include "nuvashalauncher-java-row.h"

#include <adwaita.h>
#include <gtk/gtk.h>

static nuvashalauncherJavaRow *watched_row = NULL;
static guint selected_calls = 0;

static void
on_selected (GObject    *object G_GNUC_UNUSED,
             GParamSpec *pspec G_GNUC_UNUSED,
             gpointer    user_data G_GNUC_UNUSED)
{
  selected_calls++;

  /* A regression recurses; fail while the stack can still hold it. */
  g_assert_cmpuint (selected_calls, <, 10);

  nuvashalauncher_java_row_set_required_major (watched_row, 21);
}

static guint
count_name (GtkStringList *model, const char *name)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (model));
  guint count = 0;

  for (guint i = 0; i < n; i++)
    {
      if (g_strcmp0 (gtk_string_list_get_string (model, i), name) == 0)
        count++;
    }

  return count;
}

static void
pump (int millis)
{
  gint64 deadline = g_get_monotonic_time () + millis * 1000;

  while (g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);
}

int
main (void)
{
  nuvashalauncherJavaRow *row = NULL;
  GtkStringList *model = NULL;
  GPtrArray *detected = NULL;
  guint n = 0;

  g_print ("# Java row\n");

#ifdef __SANITIZE_ADDRESS__
  /* The combo row wraps its model in a GtkListView, whose process-global
     allocations LeakSanitizer reports as leaks of this test. The test
     runs under make test; the sanitizer run skips it rather than failing
     on the toolkit's own memory. */
  g_print ("skipped - the sanitizer run does not attribute Gtk's own "
           "allocations\n");
  return EXIT_SUCCESS;
#endif

  if (!gtk_init_check ())
    {
      g_print ("skipped - no display, cannot build the row\n");
      return 77;
    }

  row = nuvashalauncher_java_row_new ();
  g_object_ref_sink (row);

  /* The row has to be ready for the first entry to be named. */
  nuvashalauncher_java_row_load (row, NULL);

  for (int i = 0; i < 500 && !nuvashalauncher_java_row_get_ready (row); i++)
    pump (10);

  g_assert_true (nuvashalauncher_java_row_get_ready (row));

  model = GTK_STRING_LIST (adw_combo_row_get_model (ADW_COMBO_ROW (row)));
  detected = nuvashalauncher_java_row_get_detected (row);

  if (detected->len == 0)
    {
      g_object_unref (row);
      g_print ("skipped - no Java runtime was detected\n");
      return 77;
    }

  n = g_list_model_get_n_items (G_LIST_MODEL (model));
  g_assert_cmpuint (n, >=, 1);

  /* The runtime the automatic choice resolves to is named in the first
   * entry and not repeated below. A machine with more than one runtime
   * whose names collide would show it twice for a reason of its own, so
   * this is checked where the answer is unambiguous. */
  if (detected->len == 1)
    {
      nuvashalauncherJava *only = g_ptr_array_index (detected, 0);

      /* Asking for what that one runtime offers makes it the automatic
       * choice. */
      if (only->major_version > 0)
        {
          nuvashalauncher_java_row_set_required_major (row, only->major_version);
          pump (20);
          g_assert_cmpuint (count_name (model,
                                        gtk_string_list_get_string (model, 0)),
                            ==, 1);
        }
    }

  /* A missing exact runtime still selects a detected runtime visibly, while
   * the row reports that the instance remains on automatic selection. */
  nuvashalauncher_java_row_set_required_major (row, G_MAXINT);
  pump (20);
  model = GTK_STRING_LIST (adw_combo_row_get_model (ADW_COMBO_ROW (row)));
  g_assert_cmpuint (adw_combo_row_get_selected (ADW_COMBO_ROW (row)),
                    !=, GTK_INVALID_LIST_POSITION);
  g_assert_cmpstr (gtk_string_list_get_string (model, 0), !=, "");
  g_assert_cmpstr (nuvashalauncher_java_row_get_selected_path (row), ==, "");

  /* Setting the requirement must not call the handler forever. */
  watched_row = row;
  selected_calls = 0;
  g_signal_connect (row, "notify::selected", G_CALLBACK (on_selected), NULL);
  nuvashalauncher_java_row_set_required_major (row, 21);
  pump (100);

  g_assert_cmpuint (selected_calls, <, 10);

  g_object_unref (row);

  g_print ("ok - the automatic runtime is named once and does not recurse\n");

  return EXIT_SUCCESS;
}
