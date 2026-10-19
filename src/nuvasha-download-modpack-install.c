/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-download-modpack-dialog-private.h"

#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-i18n.h"
#include "nuvasha-operation.h"
#include "nuvasha-modpack.h"
#include "nuvasha-fs.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static void on_pack_downloaded (GObject *, GAsyncResult *, gpointer);
static GtkWidget *build_optional_page (NuvashaDownloadModpackDialog *, GPtrArray *);
static void on_optional_cancel (GtkButton *, gpointer);
static void on_optional_install (GtkButton *, gpointer);
void start_install (NuvashaDownloadModpackDialog *);
void remove_pack_file (NuvashaDownloadModpackDialog *);

/* ---------------------------------------------------------- *
 * The pack fetched and the pack installed
 * ---------------------------------------------------------- */

/* References held by a background install so callbacks remain valid after
 * the dialog closes. */
typedef struct {
  NuvashaDownloadModpackDialog *dialog;
  NuvashaDownload *download;
  NuvashaTaskScope *operation;
} InstallJob;

static void
install_job_free (InstallJob *job)
{
  g_clear_object (&job->dialog);
  g_clear_object (&job->download);
  if (job->operation != NULL)
    nuvasha_operation_unref (job->operation);
  g_free (job);
}

static void
on_install_progress (guint done,
                     guint total,
  const char *path,
                     gpointer user_data)
{
  InstallJob *job = user_data;

  /* The install's "n of m files" appears in the header; a NULL
   * path marks the transition from downloading to extraction. */
  if (path == NULL)
    nuvasha_download_set_detail (job->download, _("Extracting files"));
  nuvasha_download_set_progress (job->download, done, total);
}

/* Remove downloaded archives after extraction, but never remove imports. */
void
remove_pack_file (NuvashaDownloadModpackDialog *self)
{
  if (!self->imported && self->pack_path != NULL)
    {
      nuvasha_modpack_remove_archive (self->pack_path, NULL);
      g_clear_pointer (&self->pack_path, g_free);
    }
}

static void
on_install_done (GObject *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer user_data)
{
  InstallJob *job = user_data;
  NuvashaDownloadModpackDialog *self = job->dialog;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaInstance) instance = NULL;

  instance = nuvasha_modpack_install_finish (result, &error);

  /* The entry leaves the header's dropdown whether it worked or not, and
     the compressed pack it came from goes with it. */
  nuvasha_download_finish (job->download);
  nuvasha_operation_unref (job->operation);
  job->operation = NULL;
  remove_pack_file (self);

  if (instance == NULL)
    {
      /* The error page only makes sense while the dialog is up; a closed
         dialog that failed in the background just leaves the header. */
      if (!self->closed &&
          !g_error_matches (error, nuvasha_MODPACK_ERROR,
                            nuvasha_MODPACK_ERROR_CANCELLED) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          self->fail_is_install = TRUE;
          push_error (self, "Could not install the modpack", error->message,
                      G_CALLBACK (on_error_retry_install));
        }
      else if (!g_error_matches (error, nuvasha_MODPACK_ERROR,
                                 nuvasha_MODPACK_ERROR_CANCELLED) &&
               !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("the modpack could not be installed: %s", error->message);

      install_job_free (job);
      return;
    }

  /* The instance owns its icon. Never leave its property pointing into the
     shared cache, because cache cleanup is allowed to remove that tree. */
  if (nuvasha_modpack_icon_is_regular (self->icon_path))
    {
      if (!nuvasha_modpack_install_icon (self->manager, instance, self->icon_path,
                                     &error))
        g_warning ("the icon of the new instance could not be copied: %s",
                   error != NULL ? error->message : "unknown error");
    }

  install_job_free (job);
}

void
start_install (NuvashaDownloadModpackDialog *self)
{
  g_autoptr (GPtrArray) chosen = NULL;
  const char *name = NULL;
  const char *directory = NULL;

  g_return_if_fail (self->modpack != NULL);

  name = self->instance_name;

  if (name == NULL || *name == '\0')
    name = self->modpack->name;

  if (name == NULL || *name == '\0')
    name = self->project != NULL && self->project->title != NULL
           ? self->project->title
           : "modpack";

  directory = self->directory; /* NULL means: the manager's default */

  /* the user's selections for the optional files. A closed dialog has
     none left to read (its switches are gone), so what the pack requires
     is what is installed. */
  chosen = g_ptr_array_new ();

  if (!self->closed && self->opt_switch_rows != NULL)
    {
      for (guint i = 0; i < self->opt_switch_rows->len; i++)
        {
          AdwSwitchRow *row = ADW_SWITCH_ROW (
            g_ptr_array_index (self->opt_switch_rows, i));

          if (adw_switch_row_get_active (row))
            {
              NuvashaModpackFile *file = g_ptr_array_index (self->opt_files, i);

              g_ptr_array_add (chosen, file);
            }
        }
    }

  {
    InstallJob *job = g_new0 (InstallJob, 1);
    NuvashaDownload *download = self->pack_download;

    /* The same header entry carries on from the compressed pack into the
       extract: it is one operation with two phases. */
    if (download == NULL)
      download = nuvasha_downloads_add (nuvasha_DOWNLOAD_MODPACK, name);

    nuvasha_download_set_detail (
      download, self->modpack->files->len > 0 ? _("Downloading files")
                                              : _("Extracting files"));

    job->dialog = g_object_ref (self);
    job->download = g_object_ref (download);

    job->operation = nuvasha_modpack_install_async (
      self->modpack, self->manager, name, directory, chosen,
      on_install_progress, on_install_done, job);

    /* The × on the header entry is what the user reaches for to stop this,
       so the install follows that entry's cancellable. */
    nuvasha_operation_follow (job->operation,
                                       nuvasha_download_get_cancellable (download));
  }
}

static GtkWidget *
build_optional_page (NuvashaDownloadModpackDialog *self,
                     GPtrArray *optional_files)
{
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  AdwPreferencesGroup *heading = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkBox *buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  GtkButton *cancel = GTK_BUTTON (gtk_button_new_with_label (_("Cancel")));
  GtkButton *install = GTK_BUTTON (gtk_button_new_with_label (_("Install")));
  GtkWidget *page = NULL;

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)), GTK_WIDGET (header));

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)),
                                   _("Optional files"));
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (heading),
    _("The pack lists these files but says they are not needed. Nothing of "
      "them is installed unless you ask. Default is off"));

  g_clear_pointer (&self->opt_switch_rows, g_ptr_array_unref);
  self->opt_switch_rows = g_ptr_array_new ();
  g_clear_pointer (&self->opt_files, g_ptr_array_unref);
  self->opt_files = g_ptr_array_new_with_free_func (NULL);

  for (guint i = 0; i < optional_files->len; i++)
    {
      NuvashaModpackFile *file = g_ptr_array_index (optional_files, i);
      AdwSwitchRow *row = ADW_SWITCH_ROW (adw_switch_row_new ());

      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (GTK_WIDGET (row)), file->path);
      adw_switch_row_set_active (ADW_SWITCH_ROW (GTK_WIDGET (row)), FALSE);
      gtk_widget_add_css_class (GTK_WIDGET (row), "dim-label");

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)), GTK_WIDGET (row));
      g_ptr_array_add (self->opt_switch_rows, row);
      g_ptr_array_add (self->opt_files, file);
    }

  gtk_widget_add_css_class (GTK_WIDGET (install), "suggested-action");
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (cancel));
  gtk_box_append (GTK_BOX (GTK_WIDGET (buttons)), GTK_WIDGET (install));
  gtk_widget_set_margin_start (GTK_WIDGET (buttons), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (buttons), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (buttons), 12);
  gtk_widget_set_halign (GTK_WIDGET (buttons), GTK_ALIGN_END);

  g_signal_connect (cancel, "clicked", G_CALLBACK (on_optional_cancel),
                    self);
  g_signal_connect (install, "clicked", G_CALLBACK (on_optional_install),
                    self);

  AdwPreferencesPage *page_holder =
    ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  adw_preferences_page_add (page_holder, ADW_PREFERENCES_GROUP (GTK_WIDGET (heading)));

  GtkBox *boxy = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));

  gtk_box_append (GTK_BOX (GTK_WIDGET (boxy)), GTK_WIDGET (page_holder));
  gtk_box_append (GTK_BOX (GTK_WIDGET (boxy)), GTK_WIDGET (buttons));

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (GTK_WIDGET (toolbar)), GTK_WIDGET (boxy));
  page = GTK_WIDGET (adw_navigation_page_new (GTK_WIDGET (toolbar), "Choose Files"));
  adw_navigation_page_set_tag (ADW_NAVIGATION_PAGE (GTK_WIDGET (page)), "optional");

  return page;
}

static void
on_optional_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);

  dialog_details_show_again (self);
}

static void
on_optional_install (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);

  /* Pop ourselves off so the installing page is showing when
   * the install clocks in with the downloading files. */
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (self->nav));

  start_install (self);
}


/* The name the pack gives itself is what the instance follows. The name
 * field shows the Modrinth project title until the pack is read; a field
 * still holding that title, or holding nothing, is replaced by the pack's
 * own name. A name the user typed is kept exactly as typed. */
static void
adopt_pack_name (NuvashaDownloadModpackDialog *self)
{
  const char *pack_name = self->modpack != NULL ? self->modpack->name : NULL;
  const char *typed = NULL;

  if (pack_name == NULL || *pack_name == '\0')
    return;

  if (self->name_entry != NULL)
    {
      typed = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));

      if (typed != NULL && *typed != '\0' &&
          g_strcmp0 (typed, self->project != NULL ? self->project->title : NULL)
            != 0)
        return; /* the user named it: that is what it is called */

      gtk_editable_set_text (GTK_EDITABLE (self->name_entry), pack_name);
    }

  if (self->instance_name == NULL || *self->instance_name == '\0' ||
      g_strcmp0 (self->instance_name,
                 self->project != NULL ? self->project->title : NULL) == 0)
    {
      g_free (self->instance_name);
      self->instance_name = g_strdup (pack_name);
    }
}

/* Fetch the .mrpack, parse it, and decide if the user has options. */

/* What the pack file weighs as it arrives, e.g. "312.4/500.0 MB". The
 * manager entry formats it; the dialog only hands the numbers over. */
static void
on_pack_progress (goffset bytes_done, goffset total, gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);

  if (self->pack_download != NULL)
    nuvasha_download_set_bytes (self->pack_download, (guint64) bytes_done,
                            total > 0 ? (guint64) total : 0);
}

static void
on_pack_downloaded (GObject *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);
  g_autoptr (GError) error = NULL;

  if (!nuvasha_download_file_finish (result, &error))
    {
      if (!g_error_matches (error, nuvasha_DOWNLOAD_ERROR,
                            nuvasha_DOWNLOAD_ERROR_CANCELLED))
        {
          /* A pack that failed after the dialog was closed has nowhere
             to say so; the reason goes to the journal and the entry
             leaves the header. */
          if (!self->closed)
            {
              self->fail_is_install = FALSE;
               push_error (self, _("Pack could not be fetched"), error->message,
                          G_CALLBACK (on_error_retry_fetch));
            }
          else
            g_warning ("the modpack \"%s\" could not be fetched: %s",
                       self->pack_path != NULL ? self->pack_path : "?",
                       error->message);
        }

      if (self->pack_download != NULL)
        nuvasha_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  nuvasha_operation_release (
    nuvasha_modpack_new_from_zip_async (self->pack_path, on_pack_parsed,
                                    user_data),
    self->pack_download != NULL
      ? nuvasha_download_get_cancellable (self->pack_download) : self->fetch);
}

void
on_pack_parsed (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                gpointer user_data)
{
  NuvashaDownloadModpackDialog *self = nuvasha_DOWNLOAD_MODPACK_DIALOG (user_data);
  g_autoptr (GError) error = NULL;

  g_clear_pointer (&self->modpack, nuvasha_modpack_free);
  self->modpack = nuvasha_modpack_new_from_zip_finish (result, &error);

  if (self->modpack == NULL)
    {
      if (!self->closed)
        {
          self->fail_is_install = FALSE;
           push_error (self, _("Invalid pack"), error->message,
                      G_CALLBACK (on_error_retry_fetch));
        }
      else
        g_warning ("the fetched modpack is not a pack: %s", error->message);

      if (self->pack_download != NULL)
        nuvasha_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  if (!nuvasha_modpack_resolve (self->modpack, &error))
    {
      if (!self->closed)
        {
          self->fail_is_install = FALSE;
           push_error (self, _("The pack does not define its install"),
                      error->message, G_CALLBACK (on_error_retry_fetch));
        }
      else
        g_warning ("the fetched modpack does not define its install: %s",
                   error->message);

      if (self->pack_download != NULL)
        nuvasha_download_finish (self->pack_download);
      remove_pack_file (self);

      operation_done (user_data);
      return;
    }

  /* The pack is known now, so the instance takes its name. A closed
     dialog has no name field left; the name is kept all the same. */
  adopt_pack_name (self);

  {
    g_autoptr (GPtrArray) optional =
      nuvasha_modpack_optional_client_files (self->modpack);

    /* Only a dialog that is still there can ask which optional files to
       take; a pack that arrived after it was closed installs what the
       pack requires. */
    /* A local import is intentionally headless: the dialog is never
     * presented, so there is no optional-file page for the user to answer.
     * Install the required files and overrides directly, leaving optional
     * entries out just as the closed-dialog path does. */
    if (!self->imported && !self->closed && optional->len > 0)
      {
        GtkWidget *page = build_optional_page (self, optional);

        adw_navigation_view_push (ADW_NAVIGATION_VIEW (self->nav),
                                  ADW_NAVIGATION_PAGE (page));
        operation_done (user_data);
        return;
      }
  }

  /* start_install holds its own reference from here on. Take it before
     this callback gives back the one it holds, so a dialog that is
     already closed is not freed under the install it just started. */
  start_install (self);
  operation_done (user_data);
}

gboolean
dialog_start_features (NuvashaDownloadModpackDialog *self)
{
  NuvashaModrinthFile *file = NULL;

  if (self->version == NULL || self->version->files == NULL)
    return FALSE;

  for (guint i = 0; i < self->version->files->len; i++)
    {
      NuvashaModrinthFile *candidate = g_ptr_array_index (self->version->files, i);

      if (candidate->filename != NULL &&
          g_str_has_suffix (candidate->filename, ".mrpack"))
        {
          file = candidate;
          break;
        }
    }

  if (file == NULL)
    {
      self->fail_is_install = FALSE;
      push_error (self, "This version has no pack file",
                  "It carries files that nuvasha does not recognize.",
                  G_CALLBACK (on_error_retry_fetch));
      return FALSE;
    }

  /* The pack itself is a download like any other: it goes into the
     header's dropdown and the spinner, and its × cancels it. */
  if (self->pack_download != NULL)
    {
      nuvasha_download_finish (self->pack_download);
      self->pack_download = NULL;
    }

  {
    const char *name = self->instance_name;

    if (name == NULL || *name == '\0')
      name = self->project != NULL ? self->project->title : NULL;

    if (name == NULL || *name == '\0')
      name = "modpack";

    self->pack_download = nuvasha_downloads_add (nuvasha_DOWNLOAD_MODPACK, name);
  }

  nuvasha_download_set_detail (self->pack_download, _("Downloading Modpack"));
  g_object_add_weak_pointer (G_OBJECT (self->pack_download),
                             (gpointer *) &self->pack_download);

  {
    g_autofree char *template = g_build_filename (
      g_get_tmp_dir (), ".nuvasha-pack-XXXXXX", NULL);
    int fd = g_mkstemp (template);

    if (fd < 0)
      {
        g_autoptr (GError) error = NULL;

        g_set_error (&error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "cannot create a temporary modpack file: %s",
                     g_strerror (errno));
        push_error (self, _("Pack could not be fetched"),
                    error->message, G_CALLBACK (on_error_retry_fetch));
        return FALSE;
      }

    close (fd);
    nuvasha_fs_unlink (template, NULL);
    g_free (self->pack_path);
    self->pack_path = g_steal_pointer (&template);

    nuvasha_operation_release (
      nuvasha_download_file_async (nuvasha_downloads_get_service (), file->url, self->pack_path, file->sha1,
                               G_CHECKSUM_SHA1, on_pack_progress,
                               on_pack_downloaded, g_object_ref (self)),
      nuvasha_download_get_cancellable (self->pack_download));
  }

  return TRUE;
}
