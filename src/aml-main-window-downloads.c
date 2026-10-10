/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-main-window-private.h"
#include "aml-download.h"
#include "aml-downloads.h"
#include "aml-i18n.h"

/* One dropdown entry: the box GTK owns plus the widgets that change as
 * the download does. The box is a child of the downloads list, so it is
 * destroyed with it; only the struct is ours. */
typedef struct {
  GtkWidget *box;
  GtkWidget *phase;  /* what it is doing, on the left */
  GtkWidget *count;  /* how far it is, on the right */
  GtkWidget *bar;
} DownloadRow;

void
download_row_free (gpointer data)
{
  g_free (data);
}

static void
on_download_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  aml_download_cancel (AML_DOWNLOAD (user_data));
}

/* What is on the right of the phase line: "312.0/500.0 MB" for a transfer
 * whose length the server gave, "12 of 48 files" for an install. */
static char *
download_count_text (AmlDownload *download)
{
  guint64 total = aml_download_get_total (download);
  guint64 done = aml_download_get_done (download);

  if (total == 0)
    return g_strdup ("");

  if (aml_download_get_unit (download) == AML_DOWNLOAD_UNIT_BYTES)
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
refresh_downloads (AmlMainWindow *self)
{
  GPtrArray *jobs = aml_downloads_get_all ();
  /* What the content pages have picked: what the icon carries a number
     of, and the reason the button is there when nothing is downloading. */
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  if (self->downloads_list == NULL)
    return;

  /* The rows of the downloads that are over. */
  g_hash_table_iter_init (&iter, self->download_rows);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      DownloadRow *row = value;
      gboolean present = FALSE;

      for (guint i = 0; i < jobs->len; i++)
        if (g_ptr_array_index (jobs, i) == key)
          present = TRUE;

      if (!present)
        {
          gtk_box_remove (GTK_BOX (self->downloads_list), row->box);
          g_hash_table_iter_remove (&iter);
        }
    }

  for (guint i = 0; i < jobs->len; i++)
    {
      AmlDownload *download = g_ptr_array_index (jobs, i);
      DownloadRow *row = g_hash_table_lookup (self->download_rows, download);
      const char *phase = aml_download_get_detail (download);
      g_autofree char *count = download_count_text (download);
      guint64 done = aml_download_get_done (download);
      guint64 total = aml_download_get_total (download);

      if (row == NULL)
        {
          GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
          GtkWidget *title = gtk_label_new (aml_download_get_title (download));
          GtkWidget *sub = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          GtkWidget *line_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          GtkWidget *cancel = gtk_button_new_from_icon_name (
            "window-close-symbolic");

          gtk_widget_set_halign (title, GTK_ALIGN_START);
          gtk_widget_add_css_class (title, "heading");
          gtk_widget_set_hexpand (title, TRUE);
          gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
          gtk_label_set_xalign (GTK_LABEL (title), 0.0f);

          row = g_new0 (DownloadRow, 1);
          row->box = box;
          row->phase = gtk_label_new (NULL);
          row->count = gtk_label_new (NULL);
          row->bar = gtk_progress_bar_new ();

          /* The phase ("Downloading compressed modpack") on the left and
             the amount ("312.0/500.0 MB") on the right, under the title. */
          gtk_widget_set_halign (row->phase, GTK_ALIGN_START);
          gtk_widget_set_hexpand (row->phase, TRUE);
          gtk_label_set_xalign (GTK_LABEL (row->phase), 0.0f);
          gtk_widget_add_css_class (row->phase, "dim-label");

          gtk_widget_set_halign (row->count, GTK_ALIGN_END);
          gtk_widget_set_valign (row->count, GTK_ALIGN_CENTER);
          gtk_widget_add_css_class (row->count, "dim-label");
          gtk_label_set_xalign (GTK_LABEL (row->count), 1.0f);

          gtk_widget_set_margin_start (box, 12);
          gtk_widget_set_margin_end (box, 12);
          gtk_widget_set_margin_top (box, 8);
          gtk_widget_set_margin_bottom (box, 8);
          gtk_widget_set_size_request (box, 360, -1);

          /* The bar takes the room and the × sits right of it. */
          gtk_widget_set_hexpand (row->bar, TRUE);
          gtk_widget_set_valign (row->bar, GTK_ALIGN_CENTER);
          gtk_widget_add_css_class (cancel, "flat");
          gtk_widget_set_valign (cancel, GTK_ALIGN_CENTER);
          gtk_widget_set_tooltip_text (cancel, _("Cancel"));
          gtk_accessible_update_property (GTK_ACCESSIBLE (cancel),
                                          GTK_ACCESSIBLE_PROPERTY_LABEL,
                                          "Cancel", -1);

          g_signal_connect (cancel, "clicked", G_CALLBACK (on_download_cancel),
                            download);

          gtk_box_append (GTK_BOX (sub), row->phase);
          gtk_box_append (GTK_BOX (sub), row->count);
          gtk_box_append (GTK_BOX (line_box), row->bar);
          gtk_box_append (GTK_BOX (line_box), cancel);

          gtk_box_append (GTK_BOX (box), title);
          gtk_box_append (GTK_BOX (box), sub);
          gtk_box_append (GTK_BOX (box), line_box);
          gtk_box_append (GTK_BOX (self->downloads_list), box);

          g_hash_table_insert (self->download_rows, download, row);
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
      gtk_widget_set_visible (self->downloads_button, TRUE);
    }
  else
    {
      GtkPopover *popover = gtk_menu_button_get_popover (
        GTK_MENU_BUTTON (self->downloads_button));

      /* The last download is over: a dropdown with nothing left in it
         should not stay open over the window. */
      if (popover != NULL)
        gtk_popover_popdown (popover);

       gtk_widget_set_visible (self->downloads_button, FALSE);
    }

  /* Browser selections are shown by the browser's own button, not in this
     menu. This header button is only for transfers that are running. */
  {
    gtk_widget_set_visible (self->downloads_spinner, jobs->len > 0);
    gtk_widget_set_visible (self->downloads_icon, jobs->len == 0);
  }
}

void
on_downloads_changed (AmlDownloads *downloads G_GNUC_UNUSED, gpointer user_data)
{
  refresh_downloads (user_data);
}
