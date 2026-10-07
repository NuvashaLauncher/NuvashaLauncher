/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlJavaRow: the first entry names the runtime the default stands for,
 * and the trailing "Add Java Runtime" entry asks for a file instead of a
 * path.
 *
 * Naming the first entry replaces it in the string list, which makes the
 * combo row emit its selection again and calls the caller's handler. A
 * handler that names the entry (which is what the instance page does)
 * used to call back into itself without end and overflow the stack. This
 * checks the calls stay bounded.
 *
 * Detection scans the machine for Java runtimes; that is local and needs
 * no network. Needs a display to build the row, and skips itself when
 * there is none. */

#include "aml-java-row.h"

#include <adwaita.h>
#include <gtk/gtk.h>

static AmlJavaRow *watched_row = NULL;
static guint selected_calls = 0;

/* A runtime the label can be built from without a real executable. */
static AmlJava fake_runtime = {
  .executable = (char *) "/nonexistent/bin/java",
  .vendor = (char *) "Fake",
  .major_version = 99,
};

static void
on_selected (GObject    *object G_GNUC_UNUSED,
             GParamSpec *pspec G_GNUC_UNUSED,
             gpointer    user_data G_GNUC_UNUSED)
{
  selected_calls++;

  /* A regression recurses; fail while the stack can still hold it. */
  g_assert_cmpuint (selected_calls, <, 10);

  aml_java_row_set_default_entry_for (watched_row, &fake_runtime);
}

static guint add_requests = 0;

static void
on_add_requested (AmlJavaRow *row G_GNUC_UNUSED,
                  gpointer    user_data G_GNUC_UNUSED)
{
  add_requests++;
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
  AmlJavaRow *row = NULL;
  GtkStringList *model = NULL;
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
      return EXIT_SUCCESS;
    }

  row = aml_java_row_new ();
  g_object_ref_sink (row);
  aml_java_row_set_add_entry_enabled (row, TRUE);

  /* The row has to be ready for the first entry to be replaceable. */
  aml_java_row_load (row, NULL);

  for (int i = 0; i < 500 && !aml_java_row_get_ready (row); i++)
    pump (10);

  g_assert_true (aml_java_row_get_ready (row));

  model = GTK_STRING_LIST (adw_combo_row_get_model (ADW_COMBO_ROW (row)));
  n = g_list_model_get_n_items (G_LIST_MODEL (model));
  g_assert_cmpuint (n, >=, 1);
  g_assert_cmpstr (gtk_string_list_get_string (model, n - 1), ==,
                   "Add Java Runtime");

  /* Picking the add entry asks for a file and goes back to the first
   * entry instead of resting on it. */
  g_signal_connect (row, "add-requested", G_CALLBACK (on_add_requested), NULL);
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), n - 1);
  g_assert_cmpuint (add_requests, ==, 1);
  g_assert_cmpuint (adw_combo_row_get_selected (ADW_COMBO_ROW (row)), ==, 0);

  /* Naming the first entry must not call the handler forever. */
  watched_row = row;
  selected_calls = 0;
  g_signal_connect (row, "notify::selected", G_CALLBACK (on_selected), NULL);
  aml_java_row_set_default_entry_for (row, &fake_runtime);
  pump (100);

  g_assert_cmpstr (gtk_string_list_get_string (model, 0), ==, "Fake 99");
  g_assert_cmpuint (selected_calls, <, 10);

  g_object_unref (row);

  g_print ("ok - naming the default Java does not recurse\n");

  return EXIT_SUCCESS;
}
