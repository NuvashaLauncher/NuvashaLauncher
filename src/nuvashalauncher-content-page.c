/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherContentPage shows installed content and the compatible Modrinth
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
#include "nuvashalauncher-settings.h"

#include <glib/gstdio.h>
#include <string.h>

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-files-private.h"
#include "nuvashalauncher-task.h"

G_DEFINE_FINAL_TYPE (NuvashaLauncherContentPage, nuvashalauncher_content_page, GTK_TYPE_BOX)

static void
nuvashalauncher_content_page_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  GTK_WIDGET_CLASS (nuvashalauncher_content_page_parent_class)->size_allocate (widget, width, height, baseline);

  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (widget);
  gboolean narrow = width > 0 && width < 720;

  if (self->bar == NULL)
    return;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->bar),
                                  narrow ? GTK_ORIENTATION_VERTICAL
                                         : GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_hexpand (GTK_WIDGET (self->bar_spacer), !narrow);
  gtk_widget_set_vexpand (GTK_WIDGET (self->bar_spacer), narrow);
}

void update_batch_next (NuvashaLauncherContentPage *self);
void plan_stopped (NuvashaLauncherContentPage *self);

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
                         nuvashalauncher_error_message (error));
      if (request->page != NULL)
        show_error (request->page, message, error);
    }

  folder_open_request_free (request);
}

void
nuvashalauncher_content_page_on_open_folder_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);
  g_autofree char *uri = NULL;

  /* The folder may not exist yet on a fresh instance: asking the desktop
     to open a path that is not there only confuses it. Create it first,
     empty, ready for files. */
  g_autoptr (GError) create_error = NULL;
  if (!nuvashalauncher_content_ensure_folder (self->folder, &create_error))
    {
      g_autofree char *message =
        g_strdup_printf (_("Cannot create “%s”: %s"), self->folder,
                           nuvashalauncher_error_message (create_error));

      show_error (self, message, create_error);
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

char *
content_page_base_directory (NuvashaLauncherContentPage *self)
{
  char *directory = NULL;

  if (self->base_directory != NULL)
    return g_strdup (self->base_directory);

  g_object_get (self->instance, "directory", &directory, NULL);
  return directory;
}

NuvashaLauncherContentPage *
nuvashalauncher_content_page_new (NuvashaLauncherInstance *instance, NuvashaLauncherContentType type)
{
  return nuvashalauncher_content_page_new_with_base (instance, type, NULL);
}

NuvashaLauncherContentPage *
nuvashalauncher_content_page_new_with_base (NuvashaLauncherInstance *instance,
                                            NuvashaLauncherContentType type,
                                            const char *base_directory)
{
  NuvashaLauncherContentPage *self = NULL;
  g_autofree char *directory = NULL;
  const char *loader = NULL;
  NuvashaLauncherLoader value = NUVASHALAUNCHER_LOADER_VANILLA;

  g_return_val_if_fail (NUVASHALAUNCHER_IS_INSTANCE (instance), NULL);

  self = g_object_new (NUVASHALAUNCHER_TYPE_CONTENT_PAGE, NULL);
  self->instance = g_object_ref (instance);
  self->type = type;
  self->minecraft_version = instance_string (instance, "minecraft-version");
  self->browse.model = nuvashalauncher_content_browser_model_new ();
  self->installed.scan = g_cancellable_new ();

  if (base_directory != NULL && *base_directory != '\0')
    {
      self->base_directory = g_strdup (base_directory);
      self->folder = nuvashalauncher_content_folder_path (base_directory, type);
    }
  else
    {
      g_object_get (instance, "directory", &directory, NULL);
      if (directory != NULL)
        self->folder = nuvashalauncher_content_folder_path (directory, type);
    }

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* Mods require a non-vanilla loader. */
  g_object_get (instance, "loader", &value, NULL);

  if (nuvashalauncher_content_type_needs_loader (type))
    {
      if (value == NUVASHALAUNCHER_LOADER_VANILLA)
        {
          AdwStatusPage *status = ADW_STATUS_PAGE (adw_status_page_new ());

          gtk_widget_set_hexpand (GTK_WIDGET (status), TRUE);
          gtk_widget_set_vexpand (GTK_WIDGET (status), TRUE);
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

  self->installed.identifying = FALSE;

  self->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);

  nuvashalauncher_content_page_build_installed (self);
  nuvashalauncher_content_page_build_browse (self);

  g_signal_connect_object (self->stack, "notify::visible-child-name",
                           G_CALLBACK (nuvashalauncher_content_page_on_stack_switched),
                           self, 0);

  nuvashalauncher_content_page_build_bar (self);
  nuvashalauncher_content_page_move_actions_into_bar (self, FALSE);

  return self;
}

static void
nuvashalauncher_content_page_dispose (GObject *object)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (object);

  if (self->header_bar != NULL)
    nuvashalauncher_content_page_detach_header (self, self->header_bar);

  /* The widget tree may be freed while the page is still answering a
   * request of its own: nothing of it is touched afterwards. */
  self->toasts = NULL;
  self->banner = NULL;
  self->stack = NULL;
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
  watch_widget (&self->installed.update_popover, NULL);
  self->installed.group = NULL;
  self->installed.scroller = NULL;
  self->installed.status = NULL;
  self->installed.loading = NULL;
  self->installed.update_content = NULL;
  self->installed.update_spinner = NULL;
  self->installed.update_phase = NULL;
  self->installed.update_count = NULL;
  self->installed.update_bar = NULL;
  self->installed.update_cancel = NULL;
   self->installed.search_bar = NULL;
   self->installed.search_entry = NULL;
   self->installed.search_button = NULL;
   self->installed.search_spacer = NULL;
  self->installed.update_all = NULL;
  self->browse.results_group = NULL;
  self->browse.results_scroller = NULL;
  self->browse.results_status = NULL;
  self->browse.results_loading = NULL;
  self->browse.search_entry = NULL;
  self->browse.search_bar = NULL;
   self->browse.filter_box = NULL;
   self->browse.filter_popover = NULL;
  self->browse.loading_more = NULL;
  self->browse.retry_row = NULL;
  self->browse.retry_label = NULL;
  g_clear_object (&self->header_search_clamp);
  g_clear_object (&self->installed.search_bar);
  self->installed.search_entry = NULL;
  g_clear_object (&self->installed.search_button);
  g_clear_object (&self->browse.search_bar);
  self->browse.search_entry = NULL;

  if (self->browse.model->debounce_source != 0)
    {
      /* Through the model, which is where this source is registered: a
         scope left holding the id would take down whatever source is
         given the same id next. */
      nuvashalauncher_task_scope_remove_source (self->browse.model->scope,
                                    self->browse.model->debounce_source);
      self->browse.model->debounce_source = 0;
    }

  g_clear_handle_id (&self->installed.index_flush_source, g_source_remove);
  g_cancellable_cancel (self->installed.scan);
  g_clear_object (&self->installed.scan);

  /* A batch of updates that was still being applied is not applied any
     more: the plans and fetches hold the page weakly and stop. */
  self->install_model->updating = FALSE;

  if (self->header_bar != NULL)
    nuvashalauncher_content_page_detach_header (self, self->header_bar);
  g_clear_object (&self->header_search_clamp);
  g_clear_object (&self->switcher);
  g_clear_object (&self->installed.actions);

  g_clear_object (&self->instance);
  if (self->installed.rows_list != NULL)
    {
      g_list_free (self->installed.rows_list);
      self->installed.rows_list = NULL;
    }
  if (self->browse.result_rows != NULL)
    {
      g_list_free (self->browse.result_rows);
      self->browse.result_rows = NULL;
    }

  G_OBJECT_CLASS (nuvashalauncher_content_page_parent_class)->dispose (object);
}

static void
nuvashalauncher_content_page_finalize (GObject *object)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (object);

  g_clear_pointer (&self->installed.files, g_ptr_array_unref);
  g_clear_pointer (&self->installed.rows, g_hash_table_unref);
  g_clear_pointer (&self->browse.selected_projects, g_hash_table_unref);
  nuvashalauncher_content_browser_model_free (self->browse.model);
  self->browse.model = NULL;
  nuvashalauncher_content_install_model_free (self->install_model);
  self->install_model = NULL;
  g_free (self->minecraft_version);
  g_free (self->loader);
  g_free (self->base_directory);
  g_free (self->folder);

  G_OBJECT_CLASS (nuvashalauncher_content_page_parent_class)->finalize (object);
}

static void
nuvashalauncher_content_page_class_init (NuvashaLauncherContentPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = nuvashalauncher_content_page_dispose;
  object_class->finalize = nuvashalauncher_content_page_finalize;
  widget_class->size_allocate = nuvashalauncher_content_page_size_allocate;
}

static void
nuvashalauncher_content_page_init (NuvashaLauncherContentPage *self)
{
  self->install_model = nuvashalauncher_content_install_model_new ();
}

PageWatch *
page_watch_new (NuvashaLauncherContentPage *self)
{
  PageWatch *watch = g_new0 (PageWatch, 1);

  watch->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &watch->page);
  return watch;
}

void
page_watch_free (PageWatch *watch)
{
  if (watch == NULL)
    return;

  if (watch->operation != NULL)
    nuvashalauncher_task_scope_unref (watch->operation);

  if (watch->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (watch->page),
                                  (gpointer *) &watch->page);

  g_free (watch);
}

/* Points @field at @widget and watches it: when the widget is destroyed
 * the field becomes NULL before anything can touch it. Used for the
 * widgets of the page of a project, which the page owns. */
void
watch_widget (gpointer field, GtkWidget *widget)
{
  GtkWidget **target = field;

  if (*target != NULL)
    g_object_remove_weak_pointer (G_OBJECT (*target), field);

  *target = widget;

  if (widget != NULL)
    g_object_add_weak_pointer (G_OBJECT (widget), field);
}

void
show_toast (NuvashaLauncherContentPage *self, const char *message)
{
  nuvashalauncher_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toasts), message);
}

GtkWidget *
plain_label (const char *text)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (text != NULL ? text : ""));

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);

  return GTK_WIDGET (label);
}

void
on_banner_dismiss (AdwBanner *banner G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherContentPage *self = NUVASHALAUNCHER_CONTENT_PAGE (user_data);

  if (self->banner != NULL)
    adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
}
