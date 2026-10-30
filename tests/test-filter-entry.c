/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "nuvashalauncher-filter-entry.h"

static void
check_filter_entry (GtkEntry *entry)
{
  GtkWidget *window = gtk_window_new ();
  GtkPopover *popover = GTK_POPOVER (gtk_popover_new ());
  GdkRectangle primary, secondary;
  gint64 deadline;

  nuvashalauncher_filter_entry_set_popover (
    nuvashalauncher_FILTER_ENTRY (entry), popover, "Filters");
  gtk_popover_set_child (popover, gtk_label_new ("Filter options"));
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (entry));
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 60);
  gtk_widget_set_opacity (window, 0);
  gtk_window_present (GTK_WINDOW (window));
  gtk_editable_set_text (GTK_EDITABLE (entry), "sodium");

  deadline = g_get_monotonic_time () + G_TIME_SPAN_SECOND * 3;
  do
    {
      while (g_main_context_iteration (NULL, FALSE)) {}
      gtk_entry_get_icon_area (entry, GTK_ENTRY_ICON_PRIMARY, &primary);
      gtk_entry_get_icon_area (entry, GTK_ENTRY_ICON_SECONDARY, &secondary);
      if (primary.width > 0 && secondary.width > 0)
        break;
      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);

  g_assert_cmpstr (gtk_entry_get_icon_name (entry, GTK_ENTRY_ICON_PRIMARY),
                   ==, "view-more-symbolic");
  g_assert_cmpstr (gtk_entry_get_icon_name (entry, GTK_ENTRY_ICON_SECONDARY),
                   ==, "edit-clear-symbolic");
  g_assert_cmpint (primary.width, >, 0);
  g_assert_cmpint (secondary.width, >, 0);
  g_assert_cmpint (primary.x + primary.width, <=, secondary.x);

  /* There is no overlay or magnifying-glass icon. GTK lays out both
   * native entry icons, with text between them, even in a narrow field. */
  gtk_widget_allocate (GTK_WIDGET (entry), 160, 40, -1, NULL);
  gtk_entry_get_icon_area (entry, GTK_ENTRY_ICON_PRIMARY, &primary);
  gtk_entry_get_icon_area (entry, GTK_ENTRY_ICON_SECONDARY, &secondary);
  g_assert_cmpint (primary.x + primary.width, <=, secondary.x);

  g_signal_emit_by_name (entry, "icon-press", GTK_ENTRY_ICON_PRIMARY);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (popover)));
  gtk_popover_popdown (popover);
  g_signal_emit_by_name (entry, "icon-press", GTK_ENTRY_ICON_SECONDARY);
  g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (entry)), ==, "");
  g_assert_null (gtk_entry_get_icon_name (entry, GTK_ENTRY_ICON_SECONDARY));
  g_assert_cmpstr (gtk_entry_get_icon_name (entry, GTK_ENTRY_ICON_PRIMARY),
                   ==, "view-more-symbolic");

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_content_filter_entry (void)
{
  check_filter_entry (GTK_ENTRY (g_object_new (nuvashalauncher_TYPE_FILTER_ENTRY, NULL)));
}

static void
test_modpack_filter_entry (void)
{
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
    "/com/github/nuvashalauncher/nuvashalauncher/ui/nuvashalauncher-modpack-browser-toolbar.ui");
  GtkEntry *entry = GTK_ENTRY (gtk_builder_get_object (builder, "search-entry"));
  GtkBox *parent = GTK_BOX (gtk_widget_get_parent (GTK_WIDGET (entry)));

  g_assert_true (nuvashalauncher_IS_FILTER_ENTRY (entry));
  g_object_ref (entry);
  gtk_box_remove (parent, GTK_WIDGET (entry));
  check_filter_entry (entry);
  g_object_unref (entry);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    return 77;
  g_type_ensure (nuvashalauncher_TYPE_FILTER_ENTRY);
  g_test_add_func ("/filter-entry/content", test_content_filter_entry);
  g_test_add_func ("/filter-entry/modpack", test_modpack_filter_entry);
  return g_test_run ();
}
