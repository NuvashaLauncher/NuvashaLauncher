/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaDownloadModpackDialog: browse Modrinth and install a modpack.
 *
 * The browser is a split view: the list of packs on the left, the pack
 * that is opened on the right, and the list alone until one is. The
 * pages for installation, errors, and optional files cover the dialog.
 * Minecraft and loader versions come from the pack metadata. */

#include "nuvasha-download-modpack-dialog-private.h"
#include "nuvasha-icon-load-private.h"
#include "nuvasha-content-format.h"

#include "nuvasha-dialogs.h"
#include "nuvasha-downloads.h"
#include "nuvasha-download.h"
#include "nuvasha-operation.h"
#include "nuvasha-ui-feedback.h"
#include "nuvasha-i18n.h"
#include "nuvasha-icons.h"
#include "nuvasha-markdown.h"
#include "nuvasha-modpack.h"
#include "nuvasha-modrinth.h"
#include "nuvasha-task.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* One page of results. */
#define RESULTS_PER_PAGE 20

/* How long the words of a search entry rest before they are asked for,
 * so that typing is not one request per letter. */
#define SEARCH_DELAY_MS 500


G_DEFINE_FINAL_TYPE (NuvashaDownloadModpackDialog, nuvasha_download_modpack_dialog,
                     ADW_TYPE_DIALOG)

static void build_import_page (NuvashaDownloadModpackDialog *self);

/* Store a weak pointer to @widget in @field. */
void
details_watch (gpointer field, GtkWidget *widget)
{
  GtkWidget **target = field;

  if (*target != NULL)
    g_object_remove_weak_pointer (G_OBJECT (*target), field);

  *target = widget;

  if (widget != NULL)
    g_object_add_weak_pointer (G_OBJECT (widget), field);
}

/* Clear weak widget pointers before rebuilding the dialog. */
static void
dialog_forget_widgets (NuvashaDownloadModpackDialog *self)
{
  self->split = NULL;

  details_watch (&self->details_page, NULL);
  details_watch (&self->dropdown, NULL);
  details_watch (&self->download_button, NULL);
  details_watch (&self->body, NULL);
  details_watch (&self->meta, NULL);
  details_watch (&self->count, NULL);
  details_watch (&self->details_error, NULL);
  details_watch (&self->name_entry, NULL);
}

static void
dialog_dispose (GObject *object)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (object);

  if (self->search != NULL)
    g_cancellable_cancel (self->search);

  if (self->fetch != NULL)
    g_cancellable_cancel (self->fetch);

  if (self->scope != NULL)
    {
      nuvasha_operation_cancel (self->scope);
      /* The scope is also a GTask. Complete it after cancellation so a
       * local import does not leave an unreturned task behind when the
       * dialog is disposed. */
      nuvasha_task_scope_return_error_if_cancelled (self->scope);
    }

  dialog_forget_widgets (self);

  /* The pack download is the manager's; only the weak pointer home is
     ours to give back before the field goes away. */
  if (self->pack_download != NULL)
    g_object_remove_weak_pointer (G_OBJECT (self->pack_download),
                                  (gpointer *) &self->pack_download);

  g_clear_object (&self->manager);
  g_clear_object (&self->search);
  g_clear_object (&self->fetch);
  nuvasha_operation_unref (self->scope);
  self->scope = NULL;
  g_clear_pointer (&self->project, nuvasha_modrinth_project_free);
  g_clear_pointer (&self->version, nuvasha_modrinth_version_free);
  g_clear_pointer (&self->modpack, nuvasha_modpack_free);
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  g_clear_pointer (&self->details, nuvasha_modrinth_details_free);
  g_free (self->details_problem);
  g_free (self->versions_problem);
  g_clear_pointer (&self->rows, g_ptr_array_unref);
  g_clear_pointer (&self->hits, g_ptr_array_unref);
  g_clear_pointer (&self->opt_switch_rows, g_ptr_array_unref);
  g_clear_pointer (&self->opt_files, g_ptr_array_unref);
  g_clear_pointer (&self->seen_ids, g_hash_table_unref);
   remove_pack_file (self);
   /* Imported archives belong to the caller and are not deleted, but the
    * dialog still owns the path string it stored. */
   g_clear_pointer (&self->pack_path, g_free);
   g_free (self->directory);
  g_free (self->instance_name);
  self->instance_name = NULL;
  g_free (self->icon_path);
  self->icon_path = NULL;

  G_OBJECT_CLASS (nuvasha_download_modpack_dialog_parent_class)->dispose (object);
}

static void
nuvasha_download_modpack_dialog_class_init (NuvashaDownloadModpackDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  (void) klass;

  object_class->dispose = dialog_dispose;
}

static void
on_dialog_closed (AdwDialog *dialog G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);
  if (self->import_idle != 0)
    nuvasha_operation_remove (self->scope, self->import_idle);
  self->import_idle = 0;
  self->closed = TRUE;

  /* Closing does not stop the pack download: it is the manager's, it
     shows in the header, and the install that follows it runs without
     the dialog. Only the optional-files choice, which needs the dialog,
     is skipped for a pack that arrives after it is gone. */
}

static void
nuvasha_download_modpack_dialog_init (NuvashaDownloadModpackDialog *self)
{
  self->search = g_cancellable_new ();
  self->fetch = g_cancellable_new ();
  self->scope = nuvasha_operation_new (NULL, self->search, NULL, NULL);
  self->rows = NULL;
  self->hits = NULL;
  self->versions = NULL;
  self->seen_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          NULL);

  g_signal_connect (self, "closed", G_CALLBACK (on_dialog_closed), self);

  adw_dialog_set_title (ADW_DIALOG (self), _("Download Modpack"));
  nuvasha_dialog_set_standard_size (ADW_DIALOG (self));
  adw_dialog_set_content_width (ADW_DIALOG (self),
                                nuvasha_DIALOG_WIDE_CONTENT_WIDTH);

  self->nav = ADW_NAVIGATION_VIEW (adw_navigation_view_new ());
  /* The installing page appears in place instead of sliding in: the
     movement is not information, and the pages of this dialog are few
     and shallow enough that an instant change reads cleanly. */
  adw_navigation_view_set_animate_transitions (
    ADW_NAVIGATION_VIEW (self->nav), FALSE);
  adw_dialog_set_child (ADW_DIALOG (self), GTK_WIDGET (self->nav));

}

NuvashaDownloadModpackDialog *
nuvasha_download_modpack_dialog_new (NuvashaInstanceManager *manager)
{
  NuvashaDownloadModpackDialog *self =
    g_object_new (nuvasha_TYPE_DOWNLOAD_MODPACK_DIALOG, NULL);

  self->manager = g_object_ref (manager);
  build_browse_page (self, TRUE);

  return self;
}

static void
build_import_page (NuvashaDownloadModpackDialog *self)
{
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  AdwSpinner *spinner = ADW_SPINNER (adw_spinner_new ());
  GtkLabel *label = GTK_LABEL (gtk_label_new (_("Importing modpack…")));
  AdwNavigationPage *page = ADW_NAVIGATION_PAGE (
    adw_navigation_page_new (GTK_WIDGET (box), _("Import Modpack")));

  gtk_widget_set_halign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (box), GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (spinner));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (label));
  adw_navigation_view_add (ADW_NAVIGATION_VIEW (self->nav),
                           ADW_NAVIGATION_PAGE (page));
}

static gboolean
start_import_idle (gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);
  nuvasha_operation_forget_source (self->scope, self->import_idle);
  self->import_idle = 0;

  /* Reuse the same validation, optional-file selection and install flow as
   * a downloaded pack. The selected file is never deleted on failure. */
  nuvasha_operation_release (
    nuvasha_modpack_new_from_zip_async (self->pack_path, on_pack_parsed,
                                    g_object_ref (self)),
    self->imported ? self->fetch
                   : nuvasha_download_get_cancellable (self->pack_download));
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

NuvashaDownloadModpackDialog *
nuvasha_download_modpack_dialog_new_for_file (NuvashaInstanceManager *manager,
                                          const char *path)
{
  NuvashaDownloadModpackDialog *self =
    g_object_new (nuvasha_TYPE_DOWNLOAD_MODPACK_DIALOG, NULL);

  self->manager = g_object_ref (manager);
  build_browse_page (self, FALSE);

  self->pack_path = g_strdup (path);
  self->imported = TRUE;
  {
    g_autofree char *name = g_path_get_basename (path);

    self->pack_download = nuvasha_downloads_add (nuvasha_DOWNLOAD_MODPACK, name);
  }
  nuvasha_download_set_detail (self->pack_download, _("Reading modpack"));
  g_object_add_weak_pointer (G_OBJECT (self->pack_download),
                             (gpointer *) &self->pack_download);
  build_import_page (self);
  self->import_idle = g_idle_add (start_import_idle, g_object_ref (self));
  nuvasha_operation_track (self->scope, self->import_idle);
  return self;
}





/* ---------------------------------------------------------- *
 * The other pages
 * ---------------------------------------------------------- */

GtkWidget *
build_error_page (NuvashaDownloadModpackDialog *self,
                  const char *what,
                  const char *message,
                  GCallback retry,
                  GCallback cancel)
{
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  GtkWidget *box = NULL;
  AdwStatusPage *status = NULL;
  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkButton *retry_btn = GTK_BUTTON (gtk_button_new_with_label (_("Retry")));
  GtkButton *back_btn = GTK_BUTTON (gtk_button_new_with_label (_("Back")));
  GtkWidget *page = NULL;

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (header));

  status = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                 "dialog-error-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), what);
  {
    g_autofree char *escaped =
      g_markup_escape_text (message != NULL ? message : "", -1);

    adw_status_page_set_description (ADW_STATUS_PAGE (status), escaped);
  }

  gtk_widget_add_css_class (GTK_WIDGET (retry_btn), "suggested-action");
  g_signal_connect (retry_btn, "clicked", retry, self);
  g_signal_connect (back_btn, "clicked", cancel, self);
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (retry_btn));
  gtk_box_append (GTK_BOX (buttons), GTK_WIDGET (back_btn));
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_CENTER);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (status));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (buttons));
  gtk_widget_set_vexpand (GTK_WIDGET (status), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (status), GTK_ALIGN_CENTER);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (box));
  page = GTK_WIDGET (adw_navigation_page_new (GTK_WIDGET (toolbar),
                                               _("Could not install")));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (page), "error");

  return page;
}

void
push_error (NuvashaDownloadModpackDialog *self,
            const char *what,
            const char *message,
            GCallback retry)
{
  GtkWidget *page = build_error_page (self, what, message, retry,
                                      G_CALLBACK (on_error_back));

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                            ADW_NAVIGATION_PAGE (page));
}

/* Handler buttons patched around the pages. */

/* The pack is shown in the half beside the browser rather than as a page,
   so going back to it means taking the page on top off. */
void
dialog_details_show_again (NuvashaDownloadModpackDialog *self)
{
  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (self->nav), "browse");
}

void
on_error_back (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);

  dialog_details_show_again (self);
}

void
on_error_retry_fetch (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);

  self->fail_is_install = FALSE;

  /* Back onto the installing page the failed download left below the
   * error, or onto the details page when there was never one. */
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (self->nav));
  dialog_start_features (self);
}

void
on_error_retry_install (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);

  /* Back onto the installing page of the attempt that failed. */
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (self->nav));
  start_install (self);
}

/* The Download button under the versions: it installs the version the
   drop-down shows, and the dialog it was in has done its work. */
void
on_modpack_download_clicked (GtkButton *button G_GNUC_UNUSED,
                             gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);
  guint selected = 0;

  if (self->dropdown == NULL || self->versions == NULL)
    return;

  selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->dropdown));

  if (selected == GTK_INVALID_LIST_POSITION || selected >= self->versions->len)
    return;

  g_free (self->instance_name);
  self->instance_name = g_strdup (self->project->title);

  if (self->version != NULL)
    nuvasha_modrinth_version_free (self->version);

  self->version = nuvasha_modrinth_version_copy (
      g_ptr_array_index (self->versions, selected));

  /* The pack download and the install that follows are the manager's:
     the dialog has nothing left to show, so it closes and lets both run
     in the background with the header showing what is on its way. The
     flag is set first, so a fast (cached) pack can never race the close
     and put the optional page back up. */
  if (dialog_start_features (self))
    {
      self->closed = TRUE;
      adw_dialog_close (ADW_DIALOG (self));
    }
}
