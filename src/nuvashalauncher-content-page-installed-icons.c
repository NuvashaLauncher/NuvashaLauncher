/* Installed-content local icon loading. */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-content-files-private.h"
#include "nuvashalauncher-operation.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-i18n.h"

typedef struct {
  nuvashalauncherContentPage *page;
  GtkWidget *row;
} LocalIconLoad;

static void
local_icon_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data)
{
  LocalIconLoad *load = user_data;
  nuvashalauncherContentFile *file = g_object_get_data (G_OBJECT (load->row), "nuvashalauncher-file");
  g_autoptr (GError) error = NULL;
  g_autofree char *path =
    nuvashalauncher_content_load_local_icon_finish (result, &error);

  g_object_set_data (G_OBJECT (load->row), "nuvashalauncher-local-icon-loading", NULL);

  if (path != NULL && file != NULL)
    {
      g_free (file->local_icon_path);
      file->local_icon_path = g_steal_pointer (&path);

      if (load->page->installed.scroller != NULL)
        {
          GtkWidget *icon = GTK_WIDGET (
            g_object_get_data (G_OBJECT (load->row), "nuvashalauncher-icon"));
          if (icon != NULL)
            nuvashalauncher_icon_widget_set_file (icon, file->local_icon_path);
        }
      persist_identified_file (load->page, file);
    }

  g_object_unref (load->row);
  g_object_unref (load->page);
  g_free (load);
}

static void
load_row_icon (nuvashalauncherContentPage *self, GtkWidget *row)
{
  nuvashalauncherContentFile *file = g_object_get_data (G_OBJECT (row), "nuvashalauncher-file");
  GtkWidget *icon =
    GTK_WIDGET (g_object_get_data (G_OBJECT (row), "nuvashalauncher-icon"));
  GtkWidget *scroller = GTK_WIDGET (self->installed.scroller);
  graphene_rect_t bounds;

  if (file == NULL || icon == NULL ||
      (file->local_icon_path == NULL && file->local_icon_member == NULL) ||
      g_object_get_data (G_OBJECT (row), "nuvashalauncher-icon-loaded") != NULL ||
      !gtk_widget_compute_bounds (row, scroller, &bounds))
    return;

  if (bounds.origin.y + bounds.size.height < 0 ||
      bounds.origin.y > gtk_widget_get_height (scroller))
    return;

  if (file->local_icon_path != NULL &&
      !g_file_test (file->local_icon_path, G_FILE_TEST_IS_REGULAR))
    g_clear_pointer (&file->local_icon_path, g_free);

  if (file->local_icon_path == NULL &&
      g_object_get_data (G_OBJECT (row), "nuvashalauncher-local-icon-loading") == NULL)
    {
      LocalIconLoad *load = g_new0 (LocalIconLoad, 1);

      load->page = g_object_ref (self);
      load->row = g_object_ref (row);
      g_object_set_data (G_OBJECT (row), "nuvashalauncher-local-icon-loading",
                         GINT_TO_POINTER (1));

      nuvashalauncher_operation_release (
        nuvashalauncher_content_load_local_icon_async (file, local_icon_ready, load),
        self->installed.scan);
    }

  if (file->local_icon_path != NULL &&
      nuvashalauncher_icon_widget_set_file (icon, file->local_icon_path))
    g_object_set_data (G_OBJECT (row), "nuvashalauncher-icon-loaded", GINT_TO_POINTER (1));
}

void
load_visible_icons (nuvashalauncherContentPage *self)
{
  for (GList *item = self->installed.rows_list; item != NULL; item = item->next)
    load_row_icon (self, item->data);
}

void
nuvashalauncher_content_page_on_installed_scroll_changed (
  GtkAdjustment *adjustment G_GNUC_UNUSED, gpointer user_data)
{
  load_visible_icons (user_data);
}
