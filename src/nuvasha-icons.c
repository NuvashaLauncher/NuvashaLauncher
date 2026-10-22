/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-icons.h"
#include "nuvasha-icon-load-private.h"

#include "nuvasha-download.h"
#include "nuvasha-downloads.h"
#include "nuvasha-task-private.h"
#include "nuvasha-fs.h"

#include <errno.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <string.h>

void
nuvasha_icon_load_free (NuvashaIconLoad *load)
{
  g_clear_object (&load->icon);
  if (load->operation != NULL)
    nuvasha_operation_unref (load->operation);
  g_free (load);
}

void
nuvasha_icon_load_apply (NuvashaIconLoad *load, GAsyncResult *result)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = nuvasha_icon_load_finish (result, &error);

  if (path != NULL && !nuvasha_icon_widget_set_file (load->icon, path))
    g_debug ("the icon for \"%s\" could not be decoded",
             (const char *) g_object_get_data (G_OBJECT (load->icon),
                                               "nuvasha-icon-url"));
}

void
nuvasha_icon_load_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                     gpointer user_data)
{
  NuvashaIconLoad *load = user_data;
  nuvasha_icon_load_apply (load, result);
  nuvasha_icon_load_free (load);
}

NuvashaIconLoad *
nuvasha_icon_load_start (GtkWidget *icon, const char *url)
{
  NuvashaIconLoad *load = g_new0 (NuvashaIconLoad, 1);

  g_object_set_data_full (G_OBJECT (icon), "nuvasha-icon-url", g_strdup (url),
                          g_free);
  load->icon = g_object_ref (icon);
  load->operation = nuvasha_icon_load_async (url, nuvasha_icon_load_ready, load);
  return load;
}

/* How many icons are fetched at the same time.
 *
 * Icons go through the same download module as everything else, which
 * runs a handful of transfers at once and queues the rest. A list can ask
 * for twenty icons in one go, and twenty transfers of a few kilobytes
 * each would fill that queue and make the search that fills the list wait
 * behind artwork. A couple at a time is plenty for a list a user
 * scrolls. */
#define ICONS_AT_ONCE 2

char *
nuvasha_icon_cache_directory (void)
{
  return g_build_filename (g_get_user_cache_dir (), "nuvasha", "icons",
                           "external", NULL);
}

/* The name a URL is cached under. All cached icons are normalized to PNG at
 * display size, regardless of the source format or URL extension. */
static char *
cache_path_for (const char *url)
{
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1,
                                                         url, -1);
  g_autofree char *directory = nuvasha_icon_cache_directory ();
  g_autofree char *leaf = g_strdup_printf ("%s.png", hash);

  return g_build_filename (directory, leaf, NULL);
}

typedef struct
{
  GTask *task;

  /* Borrowed from the operation the caller owns, except while the load
     waits in the queue: the caller's reference is what keeps it alive. */
  NuvashaTaskScope *scope;
  char *url;
  char *path;
} IconLoad;

static void icon_pump (void);

/* The loads that are waiting for their turn, and how many are running. */
static GQueue pending = G_QUEUE_INIT;
static guint active = 0;

static gboolean
save_icon_bytes (const gchar *buffer, gsize count, GError **error,
                 gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);
  gsize written = 0;

  while (written < count)
    {
      ssize_t result = write (fd, buffer + written, count - written);

      if (result < 0 && errno == EINTR)
        continue;

      if (result <= 0)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "cannot write scaled icon: %s", g_strerror (errno));
          return FALSE;
        }

      written += (gsize) result;
    }

  return TRUE;
}

static void
icon_load_free (IconLoad *load)
{
  g_clear_object (&load->task);
  g_free (load->url);
  g_free (load->path);
  g_free (load);
}

static void
icon_downloaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  IconLoad *load = user_data;
  g_autoptr (GError) error = NULL;

  active--;

  if (!nuvasha_download_file_finish (result, &error))
    {
      g_debug ("the icon \"%s\" could not be fetched: %s", load->url,
               error != NULL ? error->message : "unknown error");
      nuvasha_task_scope_return_error (load->scope, g_steal_pointer (&error));
    }
  else if (!nuvasha_icon_scale_file (load->path))
    nuvasha_task_scope_return_error (
      load->scope,
      g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "the downloaded icon could not be decoded"));
  else
    nuvasha_task_scope_return_pointer (load->scope, g_strdup (load->path), g_free);

  icon_load_free (load);

  icon_pump ();
}

/* Starts a queued load when there is room for it. */
static void
icon_pump (void)
{
  while (active < ICONS_AT_ONCE && !g_queue_is_empty (&pending))
    {
      IconLoad *load = g_queue_pop_head (&pending);
      GTask *task = load->task;

      /* A load whose row went away before its turn has nothing to draw
         into; its task is answered as cancelled and it is dropped. The
         one reference the task has is the one held here, so it is given
         back once, by icon_load_free(), and not by hand first. */
      if (g_cancellable_is_cancelled (g_task_get_cancellable (task)))
        {
          nuvasha_task_scope_return_error (
            load->scope,
            g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "the icon is no longer wanted"));
          icon_load_free (load);
          continue;
        }

      active++;
      nuvasha_task_scope_release_when_done (
        nuvasha_download_file_async (nuvasha_downloads_get_service (), load->url, load->path, NULL,
                                 G_CHECKSUM_SHA1, NULL, icon_downloaded,
                                 load),
        nuvasha_task_scope_get_cancellable (load->scope));
    }
}

NuvashaTaskScope *
nuvasha_icon_load_async (const char *url, GAsyncReadyCallback callback,
                     gpointer user_data)
{
  g_autoptr (NuvashaTaskScope) scope =
    nuvasha_task_scope_new (NULL, NULL, callback, user_data);
  GTask *task = nuvasha_task_scope_task (scope);
  g_autofree char *directory = NULL;
  IconLoad *load = NULL;

  g_task_set_check_cancellable (task, FALSE);

  if (url == NULL || *url == '\0')
    {
      /* A project with no icon is not an error: there is simply nothing
         to put in the row. */
      nuvasha_task_scope_return_pointer_deferred (scope, NULL, NULL);
      return g_steal_pointer (&scope);
    }

  directory = nuvasha_icon_cache_directory ();

  load = g_new0 (IconLoad, 1);
  load->task = g_object_ref (task);

  /* Borrowed: the caller holds the operation until the callback, and the
     load is answered before it goes away. */
  load->scope = scope;
  load->url = g_strdup (url);
  load->path = cache_path_for (url);

  /* A file already in the cache is the answer: no request is made for
     something that is already on disk. */
  if (g_file_test (load->path, G_FILE_TEST_IS_REGULAR))
    {
      if (nuvasha_icon_scale_file (load->path))
        {
          nuvasha_task_scope_return_pointer_deferred (scope, g_strdup (load->path),
                                                  g_free);
          icon_load_free (load);
          return g_steal_pointer (&scope);
        }
      nuvasha_fs_unlink (load->path, NULL);
    }

  if (!nuvasha_fs_make_directory (directory, 0755, NULL))
    {
      nuvasha_task_scope_return_error_deferred (
        scope,
        g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                     "cannot create \"%s\": %s", directory,
                     g_strerror (errno)));
      icon_load_free (load);
      return g_steal_pointer (&scope);
    }

  g_queue_push_tail (&pending, load);
  icon_pump ();

  return g_steal_pointer (&scope);
}

char *
nuvasha_icon_load_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (nuvasha_task_result_is_valid (result, "nuvasha-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The widget
 * ---------------------------------------------------------- */

GtkWidget *
nuvasha_icon_widget_new (const char *placeholder_icon_name)
{
  GtkStack *stack = GTK_STACK (gtk_stack_new ());
  GtkImage *placeholder = GTK_IMAGE (gtk_image_new_from_icon_name (
      placeholder_icon_name != NULL ? placeholder_icon_name
                                    : "image-x-generic-symbolic"));

  gtk_image_set_pixel_size (GTK_IMAGE (placeholder), 32);
  gtk_widget_set_size_request (GTK_WIDGET (placeholder), nuvasha_ICON_SIZE, nuvasha_ICON_SIZE);
  gtk_widget_set_halign (GTK_WIDGET (placeholder), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (placeholder), GTK_ALIGN_CENTER);

  /* The same square for every row, whatever is in it. */
  gtk_widget_set_size_request (GTK_WIDGET (stack), nuvasha_ICON_SIZE, nuvasha_ICON_SIZE);

  gtk_stack_add_named (GTK_STACK (stack), GTK_WIDGET (placeholder), "placeholder");
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "placeholder");

  return GTK_WIDGET (stack);
}

gboolean
nuvasha_icon_scale_file (const char *path)
{
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  g_autofree char *directory = NULL;
  g_autofree char *name = NULL;
  g_autofree char *temporary_name = NULL;
  g_autoptr (GError) error = NULL;
  int parent_fd = -1;
  int fd = -1;
  gboolean saved = FALSE;

  if (path == NULL)
    return FALSE;

  pixbuf = gdk_pixbuf_new_from_file_at_scale (path, nuvasha_ICON_SIZE,
                                              nuvasha_ICON_SIZE, TRUE, NULL);
  if (pixbuf == NULL)
    return FALSE;

  directory = g_path_get_dirname (path);
  name = g_path_get_basename (path);
  fd = nuvasha_fs_open_temporary_file_nofollow (directory, &temporary_name, NULL,
                                            &parent_fd, &error);
  if (fd < 0)
    return FALSE;

  saved = gdk_pixbuf_save_to_callback (pixbuf, save_icon_bytes,
                                       GINT_TO_POINTER (fd), "png", &error,
                                       NULL);
  if (saved && fsync (fd) != 0)
    {
      g_set_error (&error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot flush scaled icon: %s", g_strerror (errno));
      saved = FALSE;
    }

  if (close (fd) != 0)
    saved = FALSE;
  fd = -1;

  if (saved)
    saved = nuvasha_fs_rename_at (parent_fd, temporary_name, name, &error);

  if (!saved)
    nuvasha_fs_unlink_at (parent_fd, temporary_name, NULL);

  close (parent_fd);
  return saved;
}

gboolean
nuvasha_icon_widget_set_file (GtkWidget *widget, const char *path)
{
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  GtkWidget *icon = NULL;
  GtkWidget *old_icon = NULL;

  if (widget == NULL || !GTK_IS_STACK (widget) || path == NULL)
    return FALSE;

  /* Decode at display size rather than retaining a full-size source texture
     for a 64-pixel row. Large modpack icons can otherwise consume megabytes
     each even though the UI never displays those extra pixels. */
  pixbuf = gdk_pixbuf_new_from_file_at_scale (path, nuvasha_ICON_SIZE,
                                              nuvasha_ICON_SIZE, TRUE, NULL);
  if (pixbuf != NULL)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      texture = gdk_texture_new_for_pixbuf (pixbuf);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }

  if (texture == NULL)
    {
      g_debug ("the icon \"%s\" could not be decoded", path);
      return FALSE;
    }

  /* An image of the square and no more: a picture reports the size of
     its source as its natural size, so a 512 by 512 icon made its row
     taller than the one next to it. The pixel size pins the icon to the
     square whatever the source holds, and it is contained in it, never
     stretched. */
  icon = gtk_image_new_from_paintable (GDK_PAINTABLE (texture));
  gtk_image_set_pixel_size (GTK_IMAGE (icon), nuvasha_ICON_SIZE);
  gtk_widget_set_size_request (icon, nuvasha_ICON_SIZE, nuvasha_ICON_SIZE);

  old_icon = gtk_stack_get_child_by_name (GTK_STACK (widget), "icon");
  if (old_icon != NULL)
    gtk_stack_remove (GTK_STACK (widget), old_icon);
  gtk_stack_add_named (GTK_STACK (widget), icon, "icon");
  gtk_stack_set_visible_child_name (GTK_STACK (widget), "icon");

  return TRUE;
}
