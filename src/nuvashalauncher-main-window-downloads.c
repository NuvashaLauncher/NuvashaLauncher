/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-main-window-private.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-ui-feedback.h"

/* One dropdown entry: the box GTK owns plus the widgets that change as
 * the download does. The box is a child of the downloads list, so it is
 * destroyed with it; only the struct is ours. */
typedef struct {
  GtkBox *box;
  GtkLabel *phase;          /* current operation */
  GtkLabel *count;          /* progress */
  GtkProgressBar *bar;
} DownloadRow;

void
download_row_free (gpointer data)
{
  g_free (data);
}

static void
on_download_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncher_download_cancel (NUVASHALAUNCHER_DOWNLOAD (user_data));
}

/* Format transfer progress for the download row. */
static char *
download_count_text (NuvashaLauncherDownload *download)
{
  guint64 total = nuvashalauncher_download_get_total (download);
  guint64 done = nuvashalauncher_download_get_done (download);

  if (total == 0)
    return g_strdup ("");

  if (nuvashalauncher_download_get_unit (download) == NUVASHALAUNCHER_DOWNLOAD_UNIT_BYTES)
    {
      gboolean gigabytes = total >= (G_GUINT64_CONSTANT (1) << 30);
      double divisor = gigabytes ? 1024.0 * 1024.0 * 1024.0
                                 : 1024.0 * 1024.0;

      return g_strdup_printf ("%.1f/%.1f %s", (double) done / divisor,
                              (double) total / divisor,
                              gigabytes ? "GB" : "MB");
    }

  return g_strdup_printf ("%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT, done,
                          total);
}

void
refresh_downloads (NuvashaLauncherMainWindow *self)
{
  GPtrArray *jobs = nuvashalauncher_downloads_get_all ();
  /* Include content-page selections in the header indicator. */
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  if (self->downloads.list == NULL)
    return;

  if (g_hash_table_size (self->downloads.rows) > 0)
    {
      g_autoptr (GHashTable) active_jobs = g_hash_table_new (g_direct_hash, g_direct_equal);

      for (guint i = 0; i < jobs->len; i++)
        g_hash_table_add (active_jobs, g_ptr_array_index (jobs, i));

      /* The rows of the downloads that are over. */
      g_hash_table_iter_init (&iter, self->downloads.rows);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          DownloadRow *row = value;

          if (!g_hash_table_contains (active_jobs, key))
            {
              gtk_box_remove (GTK_BOX (self->downloads.list), GTK_WIDGET (row->box));
              g_hash_table_iter_remove (&iter);
            }
        }
    }

  for (guint i = 0; i < jobs->len; i++)
    {
      NuvashaLauncherDownload *download = NUVASHALAUNCHER_DOWNLOAD (g_ptr_array_index (jobs, i));
      DownloadRow *row = g_hash_table_lookup (self->downloads.rows, download);
      const char *phase = nuvashalauncher_download_get_detail (download);
      g_autofree char *count = download_count_text (download);
      guint64 done = nuvashalauncher_download_get_done (download);
      guint64 total = nuvashalauncher_download_get_total (download);

      if (row == NULL)
        {
          GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 4));
          GtkLabel *title = GTK_LABEL (gtk_label_new (nuvashalauncher_download_get_title (download)));
          GtkBox *status_row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
          GtkLabel *phase_label = GTK_LABEL (gtk_label_new (""));
          GtkLabel *count_label = GTK_LABEL (gtk_label_new (""));
          GtkBox *progress_row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
          GtkProgressBar *bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
          GtkButton *cancel = GTK_BUTTON (gtk_button_new_from_icon_name ("window-close-symbolic"));

          gtk_widget_add_css_class (GTK_WIDGET (box), "nuvashalauncher-download-row");

          gtk_label_set_xalign (title, 0.0f);
          gtk_widget_set_hexpand (GTK_WIDGET (title), TRUE);
          gtk_label_set_ellipsize (title, PANGO_ELLIPSIZE_END);
          gtk_label_set_single_line_mode (title, TRUE);
          gtk_widget_add_css_class (GTK_WIDGET (title), "heading");

          gtk_label_set_xalign (phase_label, 0.0f);
          gtk_widget_set_hexpand (GTK_WIDGET (phase_label), TRUE);
          gtk_label_set_ellipsize (phase_label, PANGO_ELLIPSIZE_END);
          gtk_label_set_single_line_mode (phase_label, TRUE);
          gtk_widget_add_css_class (GTK_WIDGET (phase_label), "dim-label");

          gtk_label_set_xalign (count_label, 1.0f);
          gtk_widget_add_css_class (GTK_WIDGET (count_label), "dim-label");

          gtk_box_append (status_row, GTK_WIDGET (phase_label));
          gtk_box_append (status_row, GTK_WIDGET (count_label));

          gtk_widget_set_hexpand (GTK_WIDGET (bar), TRUE);
          gtk_widget_set_valign (GTK_WIDGET (bar), GTK_ALIGN_CENTER);

          gtk_widget_set_valign (GTK_WIDGET (cancel), GTK_ALIGN_CENTER);
          nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (cancel), _("Cancel"));
          g_signal_connect_object (cancel, "clicked",
                                   G_CALLBACK (on_download_cancel), download, 0);

          gtk_box_append (progress_row, GTK_WIDGET (bar));
          gtk_box_append (progress_row, GTK_WIDGET (cancel));

          gtk_box_append (box, GTK_WIDGET (title));
          gtk_box_append (box, GTK_WIDGET (status_row));
          gtk_box_append (box, GTK_WIDGET (progress_row));

          row = g_new0 (DownloadRow, 1);
          row->box = box;
          row->phase = phase_label;
          row->count = count_label;
          row->bar = bar;

          gtk_box_append (GTK_BOX (self->downloads.list), GTK_WIDGET (box));

          g_hash_table_insert (self->downloads.rows, download, row);
        }

      gtk_label_set_text (GTK_LABEL (row->phase),
                          phase != NULL ? phase : "");
      gtk_label_set_text (GTK_LABEL (row->count), count);

      if (total > 0)
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (row->bar),
                                       (double) done / (double) total);
      else
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (row->bar));
    }

  /* The button is the page of the header: the spinner that appears while
     anything is downloading and opens a dropdown of what is, and the
     number of what the content pages have picked on the icon beside it.
     Either of the two is a reason for it to be there at all. */
  if (jobs->len > 0)
    {
      gtk_widget_set_visible (GTK_WIDGET (self->downloads.button), TRUE);
    }
  else
    {
      GtkPopover *popover = gtk_menu_button_get_popover (
        GTK_MENU_BUTTON (self->downloads.button));

      /* The last download is over: a dropdown with nothing left in it
         should not stay open over the window. */
      if (popover != NULL)
        gtk_popover_popdown (popover);

       gtk_widget_set_visible (GTK_WIDGET (self->downloads.button), FALSE);
    }

  /* Browser selections are shown by the browser's own button, not in this
     menu. This header button is only for transfers that are running. */
  {
    gtk_widget_set_visible (GTK_WIDGET (self->downloads.spinner), jobs->len > 0);
    gtk_widget_set_visible (GTK_WIDGET (self->downloads.icon), jobs->len == 0);
  }
}

void
on_downloads_changed (NuvashaLauncherDownloads *downloads G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherMainWindow *self = user_data;

  refresh_downloads (self);
  nuvashalauncher_main_window_maybe_quit_background (self);
}
