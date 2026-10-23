/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-main-window-private.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-i18n.h"
#include "nuvasha-ui-feedback.h"

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
  nuvasha_download_cancel (nuvasha_DOWNLOAD (user_data));
}

/* Format transfer progress for the download row. */
static char *
download_count_text (NuvashaDownload *download)
{
  guint64 total = nuvasha_download_get_total (download);
  guint64 done = nuvasha_download_get_done (download);

  if (total == 0)
    return g_strdup ("");

  if (nuvasha_download_get_unit (download) == nuvasha_DOWNLOAD_UNIT_BYTES)
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
refresh_downloads (NuvashaMainWindow *self)
{
  GPtrArray *jobs = nuvasha_downloads_get_all ();
  /* Include content-page selections in the header indicator. */
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  if (self->downloads.list == NULL)
    return;

  /* The rows of the downloads that are over. */
  g_hash_table_iter_init (&iter, self->downloads.rows);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      DownloadRow *row = value;
      gboolean present = FALSE;

      for (guint i = 0; i < jobs->len; i++)
        if (g_ptr_array_index (jobs, i) == key)
          present = TRUE;

      if (!present)
        {
          gtk_box_remove (GTK_BOX (self->downloads.list), GTK_WIDGET (row->box));
          g_hash_table_iter_remove (&iter);
        }
    }

  for (guint i = 0; i < jobs->len; i++)
    {
      NuvashaDownload *download = nuvasha_DOWNLOAD (g_ptr_array_index (jobs, i));
      DownloadRow *row = g_hash_table_lookup (self->downloads.rows, download);
      const char *phase = nuvasha_download_get_detail (download);
      g_autofree char *count = download_count_text (download);
      guint64 done = nuvasha_download_get_done (download);
      guint64 total = nuvasha_download_get_total (download);

      if (row == NULL)
        {
          g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
            "/com/github/nuvashalauncher/nuvasha/ui/nuvasha-download-row.ui");
          GtkBox *box = GTK_BOX (gtk_builder_get_object (builder, "box"));
          GtkLabel *title = GTK_LABEL (gtk_label_new (nuvasha_download_get_title (download)));
          GtkButton *cancel = GTK_BUTTON (gtk_builder_get_object (builder, "cancel"));
          row = g_new0 (DownloadRow, 1);
          row->box = box;
          row->phase = GTK_LABEL (gtk_builder_get_object (builder, "phase"));
          row->count = GTK_LABEL (gtk_builder_get_object (builder, "count"));
          row->bar = GTK_PROGRESS_BAR (gtk_builder_get_object (builder, "bar"));
          gtk_label_set_ellipsize (title, PANGO_ELLIPSIZE_END);
          gtk_box_prepend (box, GTK_WIDGET (title));
           nuvasha_ui_prepare_icon_button (GTK_WIDGET (cancel), _("Cancel"));
          g_signal_connect_object (cancel, "clicked",
                                   G_CALLBACK (on_download_cancel), download, 0);
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
on_downloads_changed (NuvashaDownloads *downloads G_GNUC_UNUSED, gpointer user_data)
{
  refresh_downloads (user_data);
}
