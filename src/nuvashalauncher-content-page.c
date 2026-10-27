/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherContentPage shows installed content and the compatible Modrinth
 * catalog for one instance. The content type supplies the folder, loader
 * filter, file extensions, title, and icon.
 *
 * Async callbacks hold the page weakly: a closed page must be safe to
 * destroy while a scan, search, or version lookup is still running. */

#include "nuvashalauncher-content-page.h"
#include "nuvashalauncher-instance-page-private.h"
#include "nuvashalauncher-content-index.h"
#include "nuvashalauncher-content-format.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-markdown.h"
#include "nuvashalauncher-settings.h"

#include <glib/gstdio.h>
#include <string.h>

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-files-private.h"
#include "nuvashalauncher-operation.h"

G_DEFINE_FINAL_TYPE (nuvashalauncherContentPage, nuvashalauncher_content_page, GTK_TYPE_BOX)

static void
on_content_page_width_changed (GObject *object,
                                GParamSpec *pspec G_GNUC_UNUSED,
                                gpointer user_data G_GNUC_UNUSED)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (object);
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  gboolean narrow = width > 0 && width < 720;

  if (self->bar == NULL)
    return;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->bar),
                                  narrow ? GTK_ORIENTATION_VERTICAL
                                         : GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self->bar_spacer), !narrow);
  gtk_widget_set_vexpand (GTK_WIDGET (self->bar_spacer), narrow);
}

void nuvashalauncher_content_page_run_search (nuvashalauncherContentPage *self, gboolean append);
void update_batch_next (nuvashalauncherContentPage *self);
void start_plan (nuvashalauncherContentPage *self, nuvashalauncherModrinthVersion *version,
                        const char *title, const char *replaces,
                        gboolean disabled);
void batch_next (nuvashalauncherContentPage *self);

void plan_stopped (nuvashalauncherContentPage *self);

static void
folder_open_request_free (gpointer data)
{
  PageWatch *request = data;
  page_watch_free (request);
}

static void
open_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  PageWatch *request = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_default_for_uri_finish (result, &error))
    {
      g_autofree char *message =
        g_strdup_printf (_("The folder could not be opened: %s"),
                         error->message);
      if (request->page != NULL)
        show_toast (request->page, message);
    }

  folder_open_request_free (request);
}

void
nuvashalauncher_content_page_on_open_folder_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);
  g_autofree char *uri = NULL;

  /* The folder may not exist yet on a fresh instance: asking the desktop
     to open a path that is not there only confuses it. Create it first,
     empty, ready for files. */
  g_autoptr (GError) create_error = NULL;
  if (!nuvashalauncher_content_ensure_folder (self->folder, &create_error))
    {
      g_autofree char *message =
        g_strdup_printf (_("Cannot create \"%s\": %s"), self->folder,
                          create_error->message);

      show_toast (self, message);
      return;
    }

  uri = g_filename_to_uri (self->folder, NULL, NULL);

  if (uri == NULL)
    return;

  /* Open the folder through the desktop, without invoking a shell. */
  {
    PageWatch *request = page_watch_new (self);
    g_app_info_launch_default_for_uri_async (uri, NULL, NULL, open_finished,
                                             request);
  }
}

nuvashalauncherContentPage *
nuvashalauncher_content_page_new (nuvashalauncherInstance *instance, nuvashalauncherContentType type)
{
  nuvashalauncherContentPage *self = NULL;
  g_autofree char *directory = NULL;
  const char *loader = NULL;
  nuvashalauncherLoader value = nuvashalauncher_LOADER_VANILLA;

  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE (instance), NULL);

  self = g_object_new (nuvashalauncher_TYPE_CONTENT_PAGE, NULL);
  self->instance = g_object_ref (instance);
  self->type = type;
  self->minecraft_version = instance_string (instance, "minecraft-version");
  self->browse.model = nuvashalauncher_content_browser_model_new ();
  self->installed.scan = g_cancellable_new ();

  g_object_get (instance, "directory", &directory, NULL);

  if (directory != NULL)
    self->folder = nuvashalauncher_content_folder_path (directory, type);

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* Mods require a non-vanilla loader. */
  g_object_get (instance, "loader", &value, NULL);

  if (nuvashalauncher_content_type_needs_loader (type))
    {
      if (value == nuvashalauncher_LOADER_VANILLA)
        {
          AdwStatusPage *status = ADW_STATUS_PAGE (adw_status_page_new ());

          adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                         nuvashalauncher_content_type_icon (type));
          adw_status_page_set_title (ADW_STATUS_PAGE (status),
                                      _("No mod loader"));
          adw_status_page_set_description (
              ADW_STATUS_PAGE (status),
              _("Mods need a mod loader. Pick Fabric, Quilt, Forge or NeoForge "
              "in the settings of this instance first"));
          gtk_box_append (GTK_BOX (self), GTK_WIDGET (status));

          return self;
        }
    }

  /* Modrinth expects lowercase loader names. */
  g_object_get (instance, "loader", &value, NULL);
  loader = nuvashalauncher_loader_to_string (value);
  self->loader = loader != NULL ? g_strdup (loader) : NULL;

  self->project.details_ready = FALSE;
  self->project.versions_ready = FALSE;
  self->project.details = NULL;
  self->project.details_versions = NULL;
  self->project.details_problem = NULL;
  self->project.versions_problem = NULL;
  self->project.current_project_title = NULL;
  self->project.current_project_id = NULL;
  self->project.current_project_downloads = 0;
  self->project.details_author = NULL;
  self->project.details_body = NULL;
  self->project.details_dropdown = NULL;
  self->project.details_download = NULL;
  self->installed.identifying = FALSE;
  self->project.details_check = NULL;
  self->project.details_page = NULL;
  self->project.split = NULL;
  self->install_model->batch = NULL;
  self->install_model->picking = FALSE;
  self->install_model->pick_index = 0;
  self->pick_button = NULL;
  self->pick_badge = NULL;

  self->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);

  nuvashalauncher_content_page_build_installed (self);
  nuvashalauncher_content_page_build_browse (self);

  g_signal_connect_object (self->stack, "notify::visible-child-name",
                           G_CALLBACK (nuvashalauncher_content_page_on_stack_switched),
                           self, 0);

  nuvashalauncher_content_page_build_bar (self);
  nuvashalauncher_content_page_move_actions_into_bar (self, FALSE);
  g_signal_connect_object (self, "notify::width",
                           G_CALLBACK (on_content_page_width_changed), self, 0);

  return self;
}

static void
nuvashalauncher_content_page_dispose (GObject *object)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (object);

  /* The widget tree may be freed while the page is still answering a
   * request of its own: nothing of it is touched afterwards. */
  self->toasts = NULL;
  self->banner = NULL;
  self->stack = NULL;
  self->project.split = NULL;
  self->bar = NULL;
  self->bar_spacer = NULL;
  self->switcher = NULL;
  self->switcher_installed = NULL;
  self->switcher_browse = NULL;
  /* Release the action rows retained by the page. */
  g_clear_object (&self->installed.actions);
  g_clear_object (&self->browse_actions);
  if (self->installed.update_popover != NULL &&
      gtk_widget_get_parent (GTK_WIDGET (self->installed.update_popover)) != NULL)
    gtk_widget_unparent (GTK_WIDGET (self->installed.update_popover));
  self->installed.group = NULL;
  self->installed.scroller = NULL;
  self->installed.status = NULL;
  self->installed.loading = NULL;
  self->installed.update_content = NULL;
  self->installed.update_spinner = NULL;
  self->installed.update_popover = NULL;
  self->installed.update_phase = NULL;
  self->installed.update_count = NULL;
  self->installed.update_bar = NULL;
  self->installed.update_cancel = NULL;
  self->installed.search_bar = NULL;
  self->installed.search_entry = NULL;
  self->installed.update_all = NULL;
  self->browse.results_group = NULL;
  self->browse.results_scroller = NULL;
  self->browse.results_status = NULL;
  self->browse.search_entry = NULL;
  self->browse.search_bar = NULL;
  self->browse.filter_box = NULL;
  self->browse.loading_more = NULL;
  self->browse.retry_row = NULL;
  self->browse.retry_label = NULL;
  self->browse.spinner = NULL;
  self->pick_button = NULL;
  self->pick_badge = NULL;

  /* A batch that was still being installed is not applied any more: the
     plans and fetches hold the page weakly and stop. */
  g_clear_pointer (&self->install_model->batch, g_ptr_array_unref);
  self->install_model->picking = FALSE;
  self->install_model->pick_index = 0;

  if (self->browse.model->debounce_source != 0)
    {
      /* Through the model, which is where this source is registered: a
         scope left holding the id would take down whatever source is
         given the same id next. */
      nuvashalauncher_operation_remove (self->browse.model->scope,
                                    self->browse.model->debounce_source);
      self->browse.model->debounce_source = 0;
    }

  g_cancellable_cancel (self->installed.scan);
  g_clear_object (&self->installed.scan);

  /* The page of a project and the widgets in it are owned by the
     page; the fields only watch them, so they are dropped and not
     unrefed. */
  watch_widget (&self->project.details_body, NULL);
  watch_widget (&self->project.details_check, NULL);
  watch_widget (&self->project.details_dropdown, NULL);
  watch_widget (&self->project.details_download, NULL);
  watch_widget (&self->project.details_page, NULL);

  /* A batch of updates that was still being applied is not applied any
     more: the plans and fetches hold the page weakly and stop. */
  self->install_model->updating = FALSE;

  G_OBJECT_CLASS (nuvashalauncher_content_page_parent_class)->dispose (object);
}

static void
nuvashalauncher_content_page_finalize (GObject *object)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (object);

  g_clear_object (&self->instance);
  g_clear_pointer (&self->installed.files, g_ptr_array_unref);
  g_clear_pointer (&self->installed.rows, g_hash_table_unref);
  g_clear_pointer (&self->installed.selected_paths, g_hash_table_unref);
  nuvashalauncher_content_browser_model_free (self->browse.model);
  self->browse.model = NULL;
  g_list_free (self->installed.rows_list);
  g_list_free (self->browse.result_rows);
  nuvashalauncher_content_install_model_free (self->install_model);
  self->install_model = NULL;
  g_free (self->minecraft_version);
  g_free (self->loader);
  g_free (self->folder);
  g_free (self->project.current_project_title);
  g_free (self->project.current_project_id);
  g_free (self->project.details_author);
  g_clear_pointer (&self->project.details, nuvashalauncher_modrinth_details_free);
  g_clear_pointer (&self->project.details_versions, g_ptr_array_unref);
  g_free (self->project.details_problem);
  g_free (self->project.versions_problem);

  G_OBJECT_CLASS (nuvashalauncher_content_page_parent_class)->finalize (object);
}

static void
nuvashalauncher_content_page_class_init (nuvashalauncherContentPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_content_page_dispose;
  object_class->finalize = nuvashalauncher_content_page_finalize;
}

static void
nuvashalauncher_content_page_init (nuvashalauncherContentPage *self)
{
  /* Keep the selected-file list and required loader state. Everything else
     is built in nuvashalauncher_content_page_new(), which knows the instance and the
     kind of content. */
  self->install_model =
    nuvashalauncher_content_install_model_new ((GDestroyNotify) pick_free);
  self->installed.selected_paths = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                          g_free, NULL);
}
