/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-main-window-private.h"
#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-i18n.h"

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
          GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 4));
          GtkLabel *title = GTK_LABEL (gtk_label_new (nuvasha_download_get_title (download)));
          GtkBox *sub = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
          GtkBox *line_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
          GtkButton *cancel = GTK_BUTTON (gtk_button_new_from_icon_name (
            "window-close-symbolic"));

          gtk_widget_set_halign (GTK_WIDGET (title), GTK_ALIGN_START);
          gtk_widget_add_css_class (GTK_WIDGET (title), "heading");
          gtk_widget_set_hexpand (GTK_WIDGET (title), TRUE);
          gtk_label_set_ellipsize (GTK_LABEL (GTK_WIDGET (title)), PANGO_ELLIPSIZE_END);
          gtk_label_set_xalign (GTK_LABEL (GTK_WIDGET (title)), 0.0f);

          row = g_new0 (DownloadRow, 1);
           row->box = GTK_BOX (box);
           row->phase = GTK_LABEL (gtk_label_new (NULL));
           row->count = GTK_LABEL (gtk_label_new (NULL));
           row->bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());

          /* The phase ("Downloading compressed modpack") on the left and
             the amount ("312.0/500.0 MB") on the right, under the title. */
          gtk_widget_set_halign (GTK_WIDGET (row->phase), GTK_ALIGN_START);
          gtk_widget_set_hexpand (GTK_WIDGET (row->phase), TRUE);
          gtk_label_set_xalign (GTK_LABEL (row->phase), 0.0f);
          gtk_widget_add_css_class (GTK_WIDGET (row->phase), "dim-label");

          gtk_widget_set_halign (GTK_WIDGET (row->count), GTK_ALIGN_END);
          gtk_widget_set_valign (GTK_WIDGET (row->count), GTK_ALIGN_CENTER);
          gtk_widget_add_css_class (GTK_WIDGET (row->count), "dim-label");
          gtk_label_set_xalign (GTK_LABEL (row->count), 1.0f);

          gtk_widget_set_margin_start (GTK_WIDGET (box), 12);
          gtk_widget_set_margin_end (GTK_WIDGET (box), 12);
          gtk_widget_set_margin_top (GTK_WIDGET (box), 8);
          gtk_widget_set_margin_bottom (GTK_WIDGET (box), 8);
          gtk_widget_set_size_request (GTK_WIDGET (box), 360, -1);

          /* The bar takes the room and the × sits right of it. */
          gtk_widget_set_hexpand (GTK_WIDGET (row->bar), TRUE);
          gtk_widget_set_valign (GTK_WIDGET (row->bar), GTK_ALIGN_CENTER);
          gtk_widget_add_css_class (GTK_WIDGET (cancel), "flat");
          gtk_widget_set_valign (GTK_WIDGET (cancel), GTK_ALIGN_CENTER);
          gtk_widget_set_tooltip_text (GTK_WIDGET (cancel), _("Cancel"));
          gtk_accessible_update_property (GTK_ACCESSIBLE (GTK_WIDGET (cancel)),
                                          GTK_ACCESSIBLE_PROPERTY_LABEL,
                                          "Cancel", -1);

          g_signal_connect (cancel, "clicked", G_CALLBACK (on_download_cancel),
                            download);

          gtk_box_append (GTK_BOX (GTK_WIDGET (sub)), GTK_WIDGET (row->phase));
          gtk_box_append (GTK_BOX (GTK_WIDGET (sub)), GTK_WIDGET (row->count));
          gtk_box_append (GTK_BOX (GTK_WIDGET (line_box)), GTK_WIDGET (row->bar));
          gtk_box_append (GTK_BOX (GTK_WIDGET (line_box)), GTK_WIDGET (cancel));

          gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (title));
          gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (sub));
          gtk_box_append (GTK_BOX (GTK_WIDGET (box)), GTK_WIDGET (line_box));
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
