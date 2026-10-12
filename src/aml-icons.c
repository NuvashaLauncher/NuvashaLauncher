/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-icons.h"

#include "aml-download.h"
#include "aml-task-private.h"
#include "aml-fs.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

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
aml_icon_cache_directory (void)
{
  return g_build_filename (g_get_user_cache_dir (), "AML", "icons", NULL);
}

/* The name a url is cached under: its SHA1, with the extension the url
 * ends in so a file can be recognised for what it is. A url with no
 * extension is cached as a .png, which is what Modrinth serves. */
static char *
cache_path_for (const char *url)
{
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1,
                                                         url, -1);
  g_autofree char *directory = aml_icon_cache_directory ();
  g_autofree char *leaf = NULL;
  const char *dot = strrchr (url, '.');
  const char *extension = ".png";

  if (dot != NULL && strlen (dot) <= 5)
    extension = dot;

  /* The query, if any, is not part of the file name. */
  if (strchr (extension, '?') != NULL || strchr (extension, '/') != NULL)
    extension = ".png";

  leaf = g_strdup_printf ("%s%s", hash, extension);

  return g_build_filename (directory, leaf, NULL);
}

typedef struct
{
  GTask *task;
  AmlTaskScope *scope;
  char *url;
  char *path;
} IconLoad;

static void icon_pump (void);

/* The loads that are waiting for their turn, and how many are running. */
static GQueue pending = G_QUEUE_INIT;
static guint active = 0;

static void
icon_load_free (IconLoad *load)
{
  g_clear_object (&load->task);
  aml_task_scope_unref (load->scope);
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

  if (!aml_download_file_finish (result, &error))
    {
      g_debug ("the icon \"%s\" could not be fetched: %s", load->url,
               error != NULL ? error->message : "unknown error");
      aml_task_scope_return_error (load->scope, g_steal_pointer (&error));
    }
  else
    aml_task_scope_return_pointer (load->scope, g_strdup (load->path), g_free);

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
          aml_task_scope_return_error (
            load->scope,
            g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "the icon is no longer wanted"));
          icon_load_free (load);
          continue;
        }

      active++;
       aml_download_file_async (load->url, load->path, NULL, G_CHECKSUM_SHA1,
                                aml_task_scope_get_cancellable (load->scope), NULL,
                               icon_downloaded, load);
    }
}

void
aml_icon_load_async (const char *url, GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback,
                                       user_data);
  g_autofree char *directory = NULL;
  IconLoad *load = NULL;

  g_task_set_check_cancellable (task, FALSE);

  if (url == NULL || *url == '\0')
    {
      /* A project with no icon is not an error: there is simply nothing
         to put in the row. */
      g_task_return_pointer (task, NULL, NULL);
      return;
    }

  directory = aml_icon_cache_directory ();

  load = g_new0 (IconLoad, 1);
  load->task = g_steal_pointer (&task);
  load->scope = aml_task_scope_from_task (load->task);
  load->url = g_strdup (url);
  load->path = cache_path_for (url);

  /* A file already in the cache is the answer: no request is made for
     something that is already on disk. */
  if (g_file_test (load->path, G_FILE_TEST_IS_REGULAR))
    {
      aml_task_scope_return_pointer (load->scope, g_strdup (load->path), g_free);
      icon_load_free (load);
      return;
    }

  if (!aml_fs_make_directory (directory, 0755, NULL))
    {
      aml_task_scope_return_error (
        load->scope,
        g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                     "cannot create \"%s\": %s", directory,
                     g_strerror (errno)));
      icon_load_free (load);
      return;
    }

  g_queue_push_tail (&pending, load);
  icon_pump ();
}

char *
aml_icon_load_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * The widget
 * ---------------------------------------------------------- */

GtkWidget *
aml_icon_widget_new (const char *placeholder_icon_name)
{
  GtkStack *stack = GTK_STACK (gtk_stack_new ());
  GtkImage *placeholder = GTK_IMAGE (gtk_image_new_from_icon_name (
      placeholder_icon_name != NULL ? placeholder_icon_name
                                    : "image-x-generic-symbolic"));

  gtk_image_set_pixel_size (GTK_IMAGE (GTK_WIDGET (placeholder)), 32);
  gtk_widget_set_size_request (GTK_WIDGET (placeholder), AML_ICON_SIZE, AML_ICON_SIZE);
  gtk_widget_set_halign (GTK_WIDGET (placeholder), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (placeholder), GTK_ALIGN_CENTER);

  /* The same square for every row, whatever is in it. */
  gtk_widget_set_size_request (GTK_WIDGET (stack), AML_ICON_SIZE, AML_ICON_SIZE);

  gtk_stack_add_named (GTK_STACK (GTK_WIDGET (stack)), GTK_WIDGET (placeholder), "placeholder");
  gtk_stack_set_visible_child_name (GTK_STACK (GTK_WIDGET (stack)), "placeholder");

  return GTK_WIDGET (stack);
}

gboolean
aml_icon_widget_set_file (GtkWidget *widget, const char *path)
{
  g_autoptr (GFile) file = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  GtkWidget *icon = NULL;
  GtkWidget *old_icon = NULL;

  if (widget == NULL || !GTK_IS_STACK (widget) || path == NULL)
    return FALSE;

  /* Decoding the bytes here, once, does two things: it proves that this
     machine can read the format Modrinth served (usually WebP), and it
     hands the icon a texture that is ready to draw instead of asking it
     to read the file a second time. A file that does not decode leaves
     the placeholder in place rather than an empty square. */
  file = g_file_new_for_path (path);
  texture = gdk_texture_new_from_file (file, NULL);

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
  gtk_image_set_pixel_size (GTK_IMAGE (icon), AML_ICON_SIZE);
  gtk_widget_set_size_request (icon, AML_ICON_SIZE, AML_ICON_SIZE);

  old_icon = gtk_stack_get_child_by_name (GTK_STACK (widget), "icon");
  if (old_icon != NULL)
    gtk_stack_remove (GTK_STACK (widget), old_icon);
  gtk_stack_add_named (GTK_STACK (widget), icon, "icon");
  gtk_stack_set_visible_child_name (GTK_STACK (widget), "icon");

  return TRUE;
}
