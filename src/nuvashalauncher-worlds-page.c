/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "nuvashalauncher-worlds-page.h"
#include "nuvashalauncher-content-page.h"
#include "nuvashalauncher-dialogs.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-icons.h"
#include "nuvashalauncher-nbt.h"
#include "nuvashalauncher-ui-feedback.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char *folder_name;
  char *path;
  char *level_dat_path;
  char *icon_path;
  char *name;
  char *search_key;
  gint32 game_type;
  gint64 last_played;
  gint64 seed;
  gboolean has_seed;
} WorldEntry;

static void
world_entry_free (WorldEntry *w)
{
  if (w == NULL)
    return;
  g_free (w->folder_name);
  g_free (w->path);
  g_free (w->level_dat_path);
  g_free (w->icon_path);
  g_free (w->name);
  g_free (w->search_key);
  g_free (w);
}

struct _NuvashaLauncherWorldsPage {
  GtkBox parent_instance;

  NuvashaLauncherInstance *instance;
  NuvashaLauncher *launcher;
  char *saves_dir;

  AdwToastOverlay *toasts;
  AdwNavigationView *nav_view;
  NuvashaLauncherContentPage *datapacks_page;
  GtkWidget *back_button;
  GtkBox *bar;
  GtkWidget *add_button;
  GtkWidget *right_actions;
  AdwViewStack *stack;
  AdwPreferencesGroup *group;
  GtkFixed *drag_layer;
  GtkSearchEntry *search_bar;
  AdwHeaderBar *header_bar;
  AdwClamp *header_search_clamp;
  GPtrArray *rows;
  GPtrArray *worlds;
  GCancellable *reload_cancellable;
};

G_DEFINE_FINAL_TYPE (NuvashaLauncherWorldsPage, nuvashalauncher_worlds_page, GTK_TYPE_BOX)

static const char *
game_type_label (gint32 game_type)
{
  switch (game_type)
    {
    case 0:
      return _("Survival");
    case 1:
      return _("Creative");
    case 2:
      return _("Adventure");
    case 3:
      return _("Spectator");
    default:
      return _("Unknown");
    }
}

static gint
compare_worlds (gconstpointer a, gconstpointer b)
{
  const WorldEntry *wa = *(const WorldEntry *const *) a;
  const WorldEntry *wb = *(const WorldEntry *const *) b;

  if (wa->last_played != wb->last_played)
    return wa->last_played > wb->last_played ? -1 : 1;

  return g_utf8_collate (wa->name, wb->name);
}

static WorldEntry *
read_world_entry (const char *saves_dir, const char *folder_name)
{
  g_autofree char *world_path = g_build_filename (saves_dir, folder_name, NULL);
  g_autofree char *level_dat = g_build_filename (world_path, "level.dat", NULL);

  if (!g_file_test (world_path, G_FILE_TEST_IS_DIR) ||
      !g_file_test (level_dat, G_FILE_TEST_IS_REGULAR))
    return NULL;

  WorldEntry *w = g_new0 (WorldEntry, 1);
  w->folder_name = g_strdup (folder_name);
  w->path = g_steal_pointer (&world_path);
  w->level_dat_path = g_steal_pointer (&level_dat);
  w->game_type = -1;

  g_autofree char *icon = g_build_filename (w->path, "icon.png", NULL);
  if (g_file_test (icon, G_FILE_TEST_IS_REGULAR))
    w->icon_path = g_steal_pointer (&icon);

  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_parse_file (w->level_dat_path, NULL);
  NuvashaLauncherNbtTag *data =
    root != NULL ? nuvashalauncher_nbt_compound_get (root, "Data") : NULL;

  if (data != NULL)
    {
      const char *lvl = nuvashalauncher_nbt_compound_get_string (data, "LevelName");
      w->name = g_strdup (lvl != NULL && *lvl != '\0' ? lvl : folder_name);
      w->game_type = nuvashalauncher_nbt_compound_get_int (data, "GameType", -1);
      w->last_played = nuvashalauncher_nbt_compound_get_long (data, "LastPlayed", 0);

      NuvashaLauncherNbtTag *wgs =
        nuvashalauncher_nbt_compound_get (data, "WorldGenSettings");
      if (wgs != NULL && nuvashalauncher_nbt_compound_get (wgs, "seed") != NULL)
        {
          w->seed = nuvashalauncher_nbt_compound_get_long (wgs, "seed", 0);
          w->has_seed = TRUE;
        }
      else if (nuvashalauncher_nbt_compound_get (data, "RandomSeed") != NULL)
        {
          w->seed = nuvashalauncher_nbt_compound_get_long (data, "RandomSeed", 0);
          w->has_seed = TRUE;
        }
    }
  else
    {
      w->name = g_strdup (folder_name);
    }

  {
    g_autofree char *name_folded = w->name != NULL ? g_utf8_casefold (w->name, -1) : NULL;
    g_autofree char *folder_folded = w->folder_name != NULL ? g_utf8_casefold (w->folder_name, -1) : NULL;

    w->search_key = g_strconcat (name_folded != NULL ? name_folded : "",
                                 " ", folder_folded != NULL ? folder_folded : "",
                                 NULL);
  }

  return w;
}

static void
show_toast (NuvashaLauncherWorldsPage *self, const char *msg)
{
  nuvashalauncher_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toasts), msg);
}

static void
on_join_world_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  WorldEntry *w = g_object_get_data (G_OBJECT (button), "nuvashalauncher-world");

  if (self->launcher == NULL || w == NULL)
    return;

  nuvashalauncher_launcher_set_quick_play (self->launcher, w->folder_name, NULL);
  nuvashalauncher_launcher_start (self->launcher);
}

typedef struct {
  NuvashaLauncherWorldsPage *page;
  char *world_path;
  char *level_dat_path;
  GtkEntry *entry;
} RenameWorldData;

static void
rename_world_data_free (RenameWorldData *d)
{
  if (d == NULL)
    return;
  g_clear_object (&d->page);
  g_free (d->world_path);
  g_free (d->level_dat_path);
  g_free (d);
}

typedef struct {
  char *level_dat_path;
  char *new_name;
} RenameWorldOp;

static void
rename_world_op_free (RenameWorldOp *op)
{
  if (op == NULL)
    return;
  g_free (op->level_dat_path);
  g_free (op->new_name);
  g_free (op);
}

static void
rename_world_thread (GTask        *task,
                     gpointer      source_object G_GNUC_UNUSED,
                     gpointer      task_data,
                     GCancellable *cancellable G_GNUC_UNUSED)
{
  RenameWorldOp *op = task_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_parse_file (op->level_dat_path, &error);
  NuvashaLauncherNbtTag *data =
    root != NULL ? nuvashalauncher_nbt_compound_get (root, "Data") : NULL;

  if (data == NULL)
    {
      if (error == NULL)
        g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "World level.dat is missing Data tag");
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  nuvashalauncher_nbt_compound_set (
    data, nuvashalauncher_nbt_new_string ("LevelName", op->new_name));
  if (!nuvashalauncher_nbt_write_file (root, op->level_dat_path, TRUE, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
on_rename_world_done (GObject *source, GAsyncResult *result,
                      gpointer user_data G_GNUC_UNUSED)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (source);
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      if (error != NULL)
        nuvashalauncher_ui_error_toast (GTK_WIDGET (self),
                                        GTK_WIDGET (self->toasts), error);
      return;
    }

  nuvashalauncher_worlds_page_reload (self);
}

static void
on_rename_world_response (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  RenameWorldData *d = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "rename") != 0)
    {
      rename_world_data_free (d);
      return;
    }

  const char *new_name = gtk_editable_get_text (GTK_EDITABLE (d->entry));
  if (new_name != NULL && *new_name != '\0')
    {
      RenameWorldOp *op = g_new0 (RenameWorldOp, 1);
      g_autoptr (GTask) task = NULL;

      op->level_dat_path = g_steal_pointer (&d->level_dat_path);
      op->new_name = g_strdup (new_name);
      task = g_task_new (d->page, NULL, on_rename_world_done, NULL);
      g_task_set_task_data (task, op, (GDestroyNotify) rename_world_op_free);
      g_task_run_in_thread (task, rename_world_thread);
    }
  rename_world_data_free (d);
}

static void
on_rename_world_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  WorldEntry *w = g_object_get_data (G_OBJECT (button), "nuvashalauncher-world");
  GtkWidget *popover = g_object_get_data (G_OBJECT (button), "nuvashalauncher-popover");

  if (popover != NULL)
    gtk_popover_popdown (GTK_POPOVER (popover));
  if (w == NULL)
    return;

  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (_("Rename world"), NULL));
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  GtkWidget *entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), w->name);
  adw_alert_dialog_set_extra_child (alert, entry);
  adw_alert_dialog_add_responses (alert,
                                  "cancel", _("Cancel"),
                                  "rename", _("Rename"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (alert, "rename", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (alert, "rename");
  adw_alert_dialog_set_close_response (alert, "cancel");

  RenameWorldData *d = g_new0 (RenameWorldData, 1);
  d->page = g_object_ref (self);
  d->world_path = g_strdup (w->path);
  d->level_dat_path = g_strdup (w->level_dat_path);
  d->entry = GTK_ENTRY (entry);

  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_rename_world_response, d);
}

static gboolean
copy_dir_recursive (const char *src, const char *dst, GError **error)
{
  if (g_mkdir_with_parents (dst, 0755) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Could not create directory %s", dst);
      return FALSE;
    }

  g_autoptr (GDir) dir = g_dir_open (src, 0, error);
  if (dir == NULL)
    return FALSE;

  const char *name = NULL;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      if (g_strcmp0 (name, "session.lock") == 0)
        continue;
      g_autofree char *s = g_build_filename (src, name, NULL);
      g_autofree char *d = g_build_filename (dst, name, NULL);
      GStatBuf st;

      if (g_lstat (s, &st) != 0 || S_ISLNK (st.st_mode))
        continue;

      if (S_ISDIR (st.st_mode))
        {
          if (!copy_dir_recursive (s, d, error))
            return FALSE;
        }
      else if (S_ISREG (st.st_mode))
        {
          if (!nuvashalauncher_fs_try_clone_path (s, d, FALSE))
            {
              g_autoptr (GFile) gf_src = g_file_new_for_path (s);
              g_autoptr (GFile) gf_dst = g_file_new_for_path (d);
              if (!g_file_copy (gf_src, gf_dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error))
                return FALSE;
            }
        }
    }
  return TRUE;
}

static char *
unique_folder_in (const char *parent, const char *base_name)
{
  g_autofree char *candidate = g_build_filename (parent, base_name, NULL);
  if (!g_file_test (candidate, G_FILE_TEST_EXISTS))
    return g_strdup (base_name);

  for (int i = 1; i < 1000; i++)
    {
      g_autofree char *name = g_strdup_printf ("%s (%d)", base_name, i);
      g_autofree char *path = g_build_filename (parent, name, NULL);
      if (!g_file_test (path, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&name);
    }
  return g_strdup_printf ("%s_%ld", base_name, (long) g_get_real_time ());
}

typedef struct
{
  char *src_path;
  char *dest_path;
  char *copy_title;
} DuplicateWorldOp;

static void
duplicate_world_op_free (DuplicateWorldOp *op)
{
  g_free (op->src_path);
  g_free (op->dest_path);
  g_free (op->copy_title);
  g_free (op);
}

static void
duplicate_world_thread (GTask *task, gpointer source_object G_GNUC_UNUSED,
                        gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
  DuplicateWorldOp *op = task_data;
  g_autoptr (GError) error = NULL;

  if (!copy_dir_recursive (op->src_path, op->dest_path, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_autofree char *dest_level = g_build_filename (op->dest_path, "level.dat", NULL);
  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_parse_file (dest_level, NULL);
  NuvashaLauncherNbtTag *data =
    root != NULL ? nuvashalauncher_nbt_compound_get (root, "Data") : NULL;
  if (data != NULL)
    {
      nuvashalauncher_nbt_compound_set (
        data, nuvashalauncher_nbt_new_string ("LevelName", op->copy_title));
      nuvashalauncher_nbt_write_file (root, dest_level, TRUE, NULL);
    }

  g_task_return_boolean (task, TRUE);
}

static void
on_duplicate_world_done (GObject *source, GAsyncResult *result,
                         gpointer user_data G_GNUC_UNUSED)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (source);
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      nuvashalauncher_ui_error_toast (GTK_WIDGET (self), GTK_WIDGET (self->toasts), error);
      return;
    }

  nuvashalauncher_worlds_page_reload (self);
}

static void
on_duplicate_world_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  WorldEntry *w = g_object_get_data (G_OBJECT (button), "nuvashalauncher-world");
  GtkWidget *popover = g_object_get_data (G_OBJECT (button), "nuvashalauncher-popover");

  if (popover != NULL)
    gtk_popover_popdown (GTK_POPOVER (popover));
  if (w == NULL)
    return;

  g_autofree char *copy_base = g_strdup_printf ("%s Copy", w->folder_name);
  g_autofree char *dest_folder = unique_folder_in (self->saves_dir, copy_base);
  DuplicateWorldOp *op = g_new0 (DuplicateWorldOp, 1);
  g_autoptr (GTask) task = NULL;

  op->src_path = g_strdup (w->path);
  op->dest_path = g_build_filename (self->saves_dir, dest_folder, NULL);
  op->copy_title = g_strdup_printf ("%s (%s)", w->name, _("Copy"));

  task = g_task_new (self, NULL, on_duplicate_world_done, NULL);
  g_task_set_task_data (task, op, (GDestroyNotify) duplicate_world_op_free);
  g_task_run_in_thread (task, duplicate_world_thread);
}

static void
on_copy_seed_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  WorldEntry *w = g_object_get_data (G_OBJECT (button), "nuvashalauncher-world");
  GtkWidget *popover = g_object_get_data (G_OBJECT (button), "nuvashalauncher-popover");

  if (popover != NULL)
    gtk_popover_popdown (GTK_POPOVER (popover));
  if (w == NULL || !w->has_seed)
    return;

  g_autofree char *seed_str = g_strdup_printf ("%" G_GINT64_FORMAT, w->seed);
  GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
  gdk_clipboard_set_text (clipboard, seed_str);
  show_toast (self, _("Seed copied to clipboard"));
}

typedef struct {
  NuvashaLauncherWorldsPage *page;
  char *world_path;
} DeleteWorldData;

static void
delete_world_thread (GTask        *task,
                     gpointer      source_object G_GNUC_UNUSED,
                     gpointer      task_data,
                     GCancellable *cancellable G_GNUC_UNUSED)
{
  const char *world_path = task_data;

  nuvashalauncher_fs_remove_tree (world_path);
  g_task_return_boolean (task, TRUE);
}

static void
on_delete_world_done (GObject *source, GAsyncResult *result,
                      gpointer user_data G_GNUC_UNUSED)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (source);

  g_task_propagate_boolean (G_TASK (result), NULL);
  nuvashalauncher_worlds_page_reload (self);
}

static void
delete_world_data_free (DeleteWorldData *d)
{
  if (d == NULL)
    return;
  g_clear_object (&d->page);
  g_free (d->world_path);
  g_free (d);
}

static void
on_delete_world_response (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  DeleteWorldData *d = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_strcmp0 (response, "delete") == 0 && d->world_path != NULL)
    {
      g_autoptr (GTask) task =
        g_task_new (d->page, NULL, on_delete_world_done, NULL);

      g_task_set_task_data (task, g_steal_pointer (&d->world_path), g_free);
      g_task_run_in_thread (task, delete_world_thread);
    }
  delete_world_data_free (d);
}

static void
on_delete_world_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  WorldEntry *w = g_object_get_data (G_OBJECT (button), "nuvashalauncher-world");
  GtkWidget *popover = g_object_get_data (G_OBJECT (button), "nuvashalauncher-popover");

  if (popover != NULL)
    gtk_popover_popdown (GTK_POPOVER (popover));
  if (w == NULL)
    return;

  g_autofree char *body = g_strdup_printf (_("Delete world “%s”? This cannot be undone."), w->name);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (_("Delete world"), body));
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_responses (alert,
                                  "cancel", _("Cancel"),
                                  "delete", _("Delete"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (alert, "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");

  DeleteWorldData *d = g_new0 (DeleteWorldData, 1);
  d->page = g_object_ref (self);
  d->world_path = g_strdup (w->path);
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL,
                           on_delete_world_response, d);
}

static GtkWidget *
popover_menu_button (const char *label, WorldEntry *w, GtkWidget *popover,
                     GCallback cb, NuvashaLauncherWorldsPage *self)
{
  GtkWidget *btn = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (btn, "flat");
  gtk_widget_set_halign (btn, GTK_ALIGN_FILL);
  g_object_set_data (G_OBJECT (btn), "nuvashalauncher-world", w);
  g_object_set_data (G_OBJECT (btn), "nuvashalauncher-popover", popover);
  g_signal_connect_object (btn, "clicked", cb, self, 0);
  return btn;
}

static void rebuild_world_rows (NuvashaLauncherWorldsPage *self);

typedef struct {
  char *level_dat_path;
  gint64 last_played;
} WorldOrderUpdate;

static void
world_order_update_free (WorldOrderUpdate *u)
{
  if (u == NULL)
    return;
  g_free (u->level_dat_path);
  g_free (u);
}

static void
reorder_worlds_thread (GTask *task,
                       gpointer source_object G_GNUC_UNUSED,
                       gpointer task_data,
                       GCancellable *cancellable G_GNUC_UNUSED)
{
  GPtrArray *updates = task_data;

  for (guint i = 0; i < updates->len; i++)
    {
      WorldOrderUpdate *u = g_ptr_array_index (updates, i);
      g_autoptr (NuvashaLauncherNbtTag) root =
        nuvashalauncher_nbt_parse_file (u->level_dat_path, NULL);
      NuvashaLauncherNbtTag *data =
        root != NULL ? nuvashalauncher_nbt_compound_get (root, "Data") : NULL;

      if (data == NULL || data->type != NUVASHALAUNCHER_NBT_COMPOUND)
        continue;

      nuvashalauncher_nbt_compound_set (
        data, nuvashalauncher_nbt_new_long ("LastPlayed", u->last_played));
      nuvashalauncher_nbt_write_file (root, u->level_dat_path, TRUE, NULL);
    }

  g_task_return_boolean (task, TRUE);
}

static void
reorder_world (NuvashaLauncherWorldsPage *self, guint from_index, guint to_index)
{
  gpointer item;
  gint64 max_ts = g_get_real_time () / 1000;
  GPtrArray *updates;
  g_autoptr (GTask) task = NULL;

  if (self->worlds == NULL || self->group == NULL ||
      from_index == to_index ||
      from_index >= self->worlds->len ||
      to_index >= self->worlds->len)
    return;

  item = g_ptr_array_steal_index (self->worlds, from_index);
  g_ptr_array_insert (self->worlds, (gint) to_index, item);

  for (guint i = 0; i < self->worlds->len; i++)
    {
      WorldEntry *w = g_ptr_array_index (self->worlds, i);
      if (w->last_played > max_ts)
        max_ts = w->last_played;
    }

  updates = g_ptr_array_new_with_free_func ((GDestroyNotify) world_order_update_free);
  for (guint i = 0; i < self->worlds->len; i++)
    {
      WorldEntry *w = g_ptr_array_index (self->worlds, i);
      gint64 desired = max_ts - (gint64) i * 1000;
      if (w->last_played != desired)
        {
          WorldOrderUpdate *u = g_new0 (WorldOrderUpdate, 1);
          w->last_played = desired;
          u->level_dat_path = g_strdup (w->level_dat_path);
          u->last_played = desired;
          g_ptr_array_add (updates, u);
        }
    }

  rebuild_world_rows (self);

  if (updates->len > 0)
    {
      task = g_task_new (self, NULL, NULL, NULL);
      g_task_set_task_data (task, updates, (GDestroyNotify) g_ptr_array_unref);
      g_task_run_in_thread (task, reorder_worlds_thread);
    }
  else
    {
      g_ptr_array_unref (updates);
    }
}

typedef struct {
  NuvashaLauncherWorldsPage *page;
  guint from_index;
  guint target_index;
  double start_y;
  double ghost_origin_x;
  double ghost_origin_y;
  gboolean dragging;
  GtkWidget *highlighted_row;
  GtkWidget *ghost;
} WorldRowDrag;

static void
clear_world_drag_visuals (WorldRowDrag *drag)
{
  if (drag->highlighted_row != NULL)
    {
      gtk_widget_remove_css_class (drag->highlighted_row, "nuvashalauncher-drop-target");
      drag->highlighted_row = NULL;
    }

  if (drag->ghost != NULL)
    {
      if (drag->page != NULL && drag->page->drag_layer != NULL &&
          gtk_widget_get_parent (drag->ghost) == GTK_WIDGET (drag->page->drag_layer))
        gtk_fixed_remove (drag->page->drag_layer, drag->ghost);
      drag->ghost = NULL;
    }
}

static void
world_row_drag_free (gpointer data)
{
  WorldRowDrag *drag = data;
  if (drag == NULL)
    return;
  clear_world_drag_visuals (drag);
  g_free (drag);
}

static void
on_world_drag_begin (GtkGestureDrag *gesture,
                     double start_x G_GNUC_UNUSED,
                     double start_y,
                     gpointer user_data)
{
  WorldRowDrag *drag = user_data;
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

  drag->from_index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "nuvashalauncher-idx"));
  drag->target_index = drag->from_index;
  drag->start_y = start_y;
  drag->dragging = FALSE;
  clear_world_drag_visuals (drag);
}

static void
on_world_drag_update (GtkGestureDrag *gesture,
                      double offset_x G_GNUC_UNUSED,
                      double offset_y,
                      gpointer user_data)
{
  WorldRowDrag *drag = user_data;
  NuvashaLauncherWorldsPage *self = drag->page;
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  double cur_y;

  if (self == NULL || self->rows == NULL)
    return;

  if (!drag->dragging)
    {
      if (fabs (offset_y) < 8.0)
        return;
      drag->dragging = TRUE;

      if (self->drag_layer != NULL)
        {
          graphene_rect_t bounds;
          if (gtk_widget_compute_bounds (row, GTK_WIDGET (self->drag_layer), &bounds))
            {
              g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (row);
              GtkWidget *pic = gtk_picture_new_for_paintable (paintable);
              gtk_widget_set_size_request (pic,
                                           (int) bounds.size.width,
                                           (int) bounds.size.height);
              gtk_widget_set_can_target (pic, FALSE);
              gtk_widget_add_css_class (pic, "nuvashalauncher-drag-ghost");
              drag->ghost_origin_x = bounds.origin.x;
              drag->ghost_origin_y = bounds.origin.y;
              gtk_fixed_put (self->drag_layer, pic,
                             drag->ghost_origin_x,
                             drag->ghost_origin_y + offset_y);
              drag->ghost = pic;
            }
        }
    }
  else if (drag->ghost != NULL && self->drag_layer != NULL)
    {
      gtk_fixed_move (self->drag_layer, drag->ghost,
                      drag->ghost_origin_x,
                      drag->ghost_origin_y + offset_y);
    }

  cur_y = drag->start_y + offset_y;
  guint best = drag->from_index;
  GtkWidget *best_row = NULL;

  for (guint i = 0; i < self->rows->len; i++)
    {
      GtkWidget *cand = g_ptr_array_index (self->rows, i);
      graphene_rect_t bounds;

      if (!gtk_widget_get_visible (cand))
        continue;
      if (!gtk_widget_compute_bounds (cand, row, &bounds))
        continue;

      if (cur_y >= bounds.origin.y &&
          cur_y <= bounds.origin.y + bounds.size.height)
        {
          best = i;
          best_row = cand;
          break;
        }
    }

  drag->target_index = best;
  if (drag->highlighted_row != best_row)
    {
      if (drag->highlighted_row != NULL)
        {
          gtk_widget_remove_css_class (drag->highlighted_row, "nuvashalauncher-drop-target");
          drag->highlighted_row = NULL;
        }
      if (best_row != NULL && best_row != row)
        {
          gtk_widget_add_css_class (best_row, "nuvashalauncher-drop-target");
          drag->highlighted_row = best_row;
        }
    }
}

static void
on_world_drag_end (GtkGestureDrag *gesture G_GNUC_UNUSED,
                   double offset_x G_GNUC_UNUSED,
                   double offset_y G_GNUC_UNUSED,
                   gpointer user_data)
{
  WorldRowDrag *drag = user_data;
  guint from = drag->from_index;
  guint to = drag->target_index;
  gboolean was_dragging = drag->dragging;

  clear_world_drag_visuals (drag);
  drag->dragging = FALSE;

  if (was_dragging && from != to && drag->page != NULL)
    reorder_world (drag->page, from, to);
}

static void on_world_datapacks_clicked (GtkButton *button, gpointer user_data);

static GtkWidget *
build_world_row (NuvashaLauncherWorldsPage *self, WorldEntry *w, guint index)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_subtitle_lines (row, 2);
  adw_action_row_set_subtitle_selectable (row, FALSE);
  gtk_widget_add_css_class (GTK_WIDGET (row), "nuvashalauncher-content-file-row");
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), w->name);

  g_autoptr (GString) sub = g_string_new (game_type_label (w->game_type));
  if (w->last_played > 0)
    {
      g_autoptr (GDateTime) dt =
        g_date_time_new_from_unix_local (w->last_played / 1000);
      if (dt != NULL)
        {
          g_autofree char *ts = g_date_time_format (dt, "%Y-%m-%d %H:%M");
          g_string_append_printf (sub, "  ·  %s", ts);
        }
    }
  adw_action_row_set_subtitle (row, sub->str);

  GtkWidget *icon = nuvashalauncher_icon_widget_new ("applications-games-symbolic");
  if (w->icon_path != NULL)
    nuvashalauncher_icon_widget_set_file (icon, w->icon_path);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (row, icon);

  GtkWidget *join_btn = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
  gtk_widget_set_valign (join_btn, GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (join_btn, _("Play"));
  g_object_set_data (G_OBJECT (join_btn), "nuvashalauncher-world", w);
  g_signal_connect_object (join_btn, "clicked",
                           G_CALLBACK (on_join_world_clicked), self, 0);
  adw_action_row_add_suffix (row, join_btn);

  GtkWidget *menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn), "view-more-symbolic");
  gtk_widget_add_css_class (menu_btn, "flat");
  gtk_widget_set_valign (menu_btn, GTK_ALIGN_CENTER);

  GtkWidget *popover = gtk_popover_new ();
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 2));
  gtk_box_append (box, popover_menu_button (_("Rename"), w, popover,
                                            G_CALLBACK (on_rename_world_clicked), self));
  gtk_box_append (box, popover_menu_button (_("Duplicate"), w, popover,
                                            G_CALLBACK (on_duplicate_world_clicked), self));
  gtk_box_append (box, popover_menu_button (_("Datapacks"), w, popover,
                                            G_CALLBACK (on_world_datapacks_clicked), self));
  if (w->has_seed)
    gtk_box_append (box, popover_menu_button (_("Copy seed"), w, popover,
                                              G_CALLBACK (on_copy_seed_clicked), self));
  GtkWidget *del = popover_menu_button (_("Delete"), w, popover,
                                        G_CALLBACK (on_delete_world_clicked), self);
  gtk_widget_add_css_class (del, "destructive-action");
  gtk_box_append (box, del);
  gtk_popover_set_child (GTK_POPOVER (popover), GTK_WIDGET (box));
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (menu_btn), popover);
  adw_action_row_add_suffix (row, menu_btn);

  g_object_set_data (G_OBJECT (row), "nuvashalauncher-idx", GUINT_TO_POINTER (index));

  WorldRowDrag *drag_state = g_new0 (WorldRowDrag, 1);
  drag_state->page = self;
  g_object_set_data_full (G_OBJECT (row), "nuvashalauncher-drag-state",
                          drag_state, world_row_drag_free);

  GtkGesture *drag_gesture = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag_gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect (drag_gesture, "drag-begin", G_CALLBACK (on_world_drag_begin), drag_state);
  g_signal_connect (drag_gesture, "drag-update", G_CALLBACK (on_world_drag_update), drag_state);
  g_signal_connect (drag_gesture, "drag-end", G_CALLBACK (on_world_drag_end), drag_state);
  gtk_widget_add_controller (GTK_WIDGET (row), GTK_EVENT_CONTROLLER (drag_gesture));

  return GTK_WIDGET (row);
}

static void
filter_world_rows (NuvashaLauncherWorldsPage *self)
{
  const char *query = self->search_bar != NULL
    ? gtk_editable_get_text (GTK_EDITABLE (self->search_bar))
    : "";
  g_autofree char *needle = (query != NULL && *query != '\0')
    ? g_utf8_casefold (query, -1)
    : NULL;

  for (guint i = 0; i < self->rows->len && i < self->worlds->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (self->rows, i);
      WorldEntry *w = g_ptr_array_index (self->worlds, i);
      gboolean visible = needle == NULL || *needle == '\0' ||
                         (w->search_key != NULL &&
                          strstr (w->search_key, needle) != NULL);

      gtk_widget_set_visible (row, visible);
    }
}

static void
rebuild_world_rows (NuvashaLauncherWorldsPage *self)
{
  for (guint i = 0; i < self->rows->len; i++)
    adw_preferences_group_remove (self->group, g_ptr_array_index (self->rows, i));
  g_ptr_array_set_size (self->rows, 0);

  for (guint i = 0; i < self->worlds->len; i++)
    {
      WorldEntry *w = g_ptr_array_index (self->worlds, i);
      GtkWidget *row = build_world_row (self, w, i);
      g_ptr_array_add (self->rows, row);
      adw_preferences_group_add (self->group, row);
    }

  filter_world_rows (self);
  adw_view_stack_set_visible_child_name (
    self->stack, self->worlds->len > 0 ? "list" : "empty");
}

static void
on_world_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  filter_world_rows (NUVASHALAUNCHER_WORLDS_PAGE (user_data));
}

static gboolean
on_world_search_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
                     guint keyval,
                     guint keycode G_GNUC_UNUSED,
                     GdkModifierType state,
                     gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);

  if (self->datapacks_page != NULL)
    return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_f && (state & GDK_CONTROL_MASK))
    {
      nuvashalauncher_worlds_page_set_search_visible (self, TRUE);
      return GDK_EVENT_STOP;
    }
  if (keyval == GDK_KEY_Escape && nuvashalauncher_worlds_page_is_search_visible (self))
    {
      nuvashalauncher_worlds_page_set_search_visible (self, FALSE);
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

static void
worlds_reload_thread (GTask *task,
                      gpointer source_object G_GNUC_UNUSED,
                      gpointer task_data,
                      GCancellable *cancellable)
{
  const char *saves_dir = task_data;
  g_autoptr (GPtrArray) worlds =
    g_ptr_array_new_with_free_func ((GDestroyNotify) world_entry_free);

  if (saves_dir != NULL && g_file_test (saves_dir, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) dir = g_dir_open (saves_dir, 0, NULL);
      if (dir != NULL)
        {
          const char *name = NULL;
          while (!g_cancellable_is_cancelled (cancellable) &&
                 (name = g_dir_read_name (dir)) != NULL)
            {
              WorldEntry *w = read_world_entry (saves_dir, name);
              if (w != NULL)
                g_ptr_array_add (worlds, w);
            }
        }
    }

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "cancelled");
      return;
    }

  g_ptr_array_sort (worlds, compare_worlds);
  g_task_return_pointer (task, g_steal_pointer (&worlds),
                         (GDestroyNotify) g_ptr_array_unref);
}

static void
on_worlds_reload_done (GObject *source,
                       GAsyncResult *result,
                       gpointer user_data G_GNUC_UNUSED)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (source);
  g_autoptr (GPtrArray) worlds = g_task_propagate_pointer (G_TASK (result), NULL);

  if (self->reload_cancellable != g_task_get_cancellable (G_TASK (result)) ||
      g_cancellable_is_cancelled (self->reload_cancellable) ||
      worlds == NULL)
    return;

  g_clear_pointer (&self->worlds, g_ptr_array_unref);
  self->worlds = g_steal_pointer (&worlds);
  rebuild_world_rows (self);
}

void
nuvashalauncher_worlds_page_reload (NuvashaLauncherWorldsPage *self)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_WORLDS_PAGE (self));

  if (self->reload_cancellable != NULL)
    {
      g_cancellable_cancel (self->reload_cancellable);
      g_clear_object (&self->reload_cancellable);
    }

  if (self->saves_dir == NULL || !g_file_test (self->saves_dir, G_FILE_TEST_IS_DIR))
    {
      g_ptr_array_set_size (self->worlds, 0);
      rebuild_world_rows (self);
      return;
    }

  self->reload_cancellable = g_cancellable_new ();
  g_autoptr (GTask) task =
    g_task_new (self, self->reload_cancellable, on_worlds_reload_done, NULL);
  g_task_set_task_data (task, g_strdup (self->saves_dir), g_free);
  g_task_run_in_thread (task, worlds_reload_thread);
}

static gboolean
write_archive_entry_to_path (struct archive *a, const char *target, GError **error)
{
  int fd = open (target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   "Could not create world file \"%s\": %s",
                   target, g_strerror (errsv));
      return FALSE;
    }

  guint8 chunk[65536];
  la_ssize_t n = 0;
  while ((n = archive_read_data (a, chunk, sizeof chunk)) > 0)
    {
      gsize off = 0;
      while (off < (gsize) n)
        {
          ssize_t w = write (fd, chunk + off, (gsize) n - off);
          if (w < 0)
            {
              if (errno == EINTR)
                continue;
              int errsv = errno;
              close (fd);
              g_unlink (target);
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                           "Could not write world file \"%s\": %s",
                           target, g_strerror (errsv));
              return FALSE;
            }
          off += (gsize) w;
        }
    }

  if (n < 0)
    {
      close (fd);
      g_unlink (target);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           archive_error_string (a));
      return FALSE;
    }

  if (close (fd) != 0)
    {
      int errsv = errno;
      g_unlink (target);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   "Could not finalize world file \"%s\": %s",
                   target, g_strerror (errsv));
      return FALSE;
    }

  return TRUE;
}

static gboolean
extract_world_zip (const char *zip_path, const char *saves_dir, GError **error)
{
  struct archive *a = archive_read_new ();
  archive_read_support_format_all (a);
  archive_read_support_filter_all (a);

  if (archive_read_open_filename (a, zip_path, 10240) != ARCHIVE_OK)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           archive_error_string (a));
      archive_read_free (a);
      return FALSE;
    }

  /* Detect whether level.dat sits at root or inside a single top-level dir. */
  g_autofree char *prefix = NULL;
  gboolean found_root_level = FALSE;
  struct archive_entry *entry = NULL;

  while (archive_read_next_header (a, &entry) == ARCHIVE_OK)
    {
      const char *p = archive_entry_pathname (entry);
      if (p == NULL)
        continue;
      if (g_strcmp0 (p, "level.dat") == 0)
        {
          found_root_level = TRUE;
          break;
        }
      const char *slash = strchr (p, '/');
      if (slash != NULL && g_strcmp0 (slash + 1, "level.dat") == 0 && prefix == NULL)
        prefix = g_strndup (p, (gsize) (slash - p + 1));
    }
  archive_read_free (a);

  if (!found_root_level && prefix == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Archive does not contain a Minecraft world (level.dat missing)");
      return FALSE;
    }

  g_autofree char *base = g_path_get_basename (zip_path);
  char *dot = strrchr (base, '.');
  if (dot != NULL)
    *dot = '\0';
  g_autofree char *raw_name = prefix != NULL
    ? g_strndup (prefix, strlen (prefix) - 1)
    : g_strdup (base);
  g_autofree char *dest_folder = unique_folder_in (saves_dir, raw_name);
  g_autofree char *dest_dir = g_build_filename (saves_dir, dest_folder, NULL);

  if (g_mkdir_with_parents (dest_dir, 0755) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Could not create world directory");
      return FALSE;
    }

  a = archive_read_new ();
  archive_read_support_format_all (a);
  archive_read_support_filter_all (a);
  if (archive_read_open_filename (a, zip_path, 10240) != ARCHIVE_OK)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           archive_error_string (a));
      archive_read_free (a);
      return FALSE;
    }

  gboolean ok = TRUE;
  while (archive_read_next_header (a, &entry) == ARCHIVE_OK)
    {
      const char *p = archive_entry_pathname (entry);
      if (p == NULL)
        continue;
      if (prefix != NULL)
        {
          if (!g_str_has_prefix (p, prefix))
            continue;
          p += strlen (prefix);
        }
      while (*p == '/')
        p++;
      if (*p == '\0' || strstr (p, "..") != NULL)
        continue;

      g_autofree char *target = g_build_filename (dest_dir, p, NULL);
      if (archive_entry_filetype (entry) == AE_IFDIR)
        {
          g_mkdir_with_parents (target, 0755);
          continue;
        }
      if (archive_entry_filetype (entry) != AE_IFREG)
        continue;

      g_autofree char *parent = g_path_get_dirname (target);
      g_mkdir_with_parents (parent, 0755);

      if (!write_archive_entry_to_path (a, target, error))
        {
          ok = FALSE;
          break;
        }
    }

  archive_read_free (a);
  return ok;
}

typedef struct
{
  char *zip_path;
  char *saves_dir;
} ExtractWorldOp;

static void
extract_world_op_free (ExtractWorldOp *op)
{
  g_free (op->zip_path);
  g_free (op->saves_dir);
  g_free (op);
}

static void
extract_world_thread (GTask *task, gpointer source_object G_GNUC_UNUSED,
                      gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
  ExtractWorldOp *op = task_data;
  g_autoptr (GError) error = NULL;

  if (!extract_world_zip (op->zip_path, op->saves_dir, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
on_add_world_extracted (GObject *source, GAsyncResult *result,
                        gpointer user_data G_GNUC_UNUSED)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (source);
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      nuvashalauncher_ui_error_toast (GTK_WIDGET (self), GTK_WIDGET (self->toasts), error);
      return;
    }

  nuvashalauncher_worlds_page_reload (self);
}

static void
on_add_world_file_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file == NULL)
    return;

  char *path = g_file_get_path (file);
  if (path == NULL)
    return;

  ExtractWorldOp *op = g_new0 (ExtractWorldOp, 1);
  g_autoptr (GTask) task = NULL;

  op->zip_path = path;
  op->saves_dir = g_strdup (self->saves_dir);

  task = g_task_new (self, NULL, on_add_world_extracted, NULL);
  g_task_set_task_data (task, op, (GDestroyNotify) extract_world_op_free);
  g_task_run_in_thread (task, extract_world_thread);
}

static void
on_add_world_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  gtk_file_filter_set_name (filter, "World Archives (*.zip)");
  gtk_file_filter_add_suffix (filter, "zip");
  g_list_store_append (filters, filter);

  gtk_file_dialog_set_title (dialog, _("Add world"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));

  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  gtk_file_dialog_open (dialog, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, on_add_world_file_chosen, self);
}

static void
on_open_folder_finished (GObject      *source G_GNUC_UNUSED,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr (NuvashaLauncherWorldsPage) self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_default_for_uri_finish (result, &error))
    nuvashalauncher_ui_error_toast (GTK_WIDGET (self),
                                    GTK_WIDGET (self->toasts), error);
}

static void
on_open_folder_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  g_autoptr (GError) error = NULL;
  g_autofree char *uri = NULL;

  if (self->saves_dir == NULL)
    return;

  if (!nuvashalauncher_fs_make_directory (self->saves_dir, 0755, &error))
    {
      nuvashalauncher_ui_error_toast (GTK_WIDGET (self),
                                      GTK_WIDGET (self->toasts), error);
      return;
    }

  uri = g_filename_to_uri (self->saves_dir, NULL, NULL);
  if (uri != NULL)
    g_app_info_launch_default_for_uri_async (uri, NULL, NULL,
                                             on_open_folder_finished,
                                             g_object_ref (self));
}

static void
on_refresh_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncher_worlds_page_reload (NUVASHALAUNCHER_WORLDS_PAGE (user_data));
}

static void
nuvashalauncher_worlds_page_dispose (GObject *object)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (object);

  if (self->reload_cancellable != NULL)
    {
      g_cancellable_cancel (self->reload_cancellable);
      g_clear_object (&self->reload_cancellable);
    }

  if (self->header_bar != NULL)
    nuvashalauncher_worlds_page_detach_header (self, self->header_bar);
  self->datapacks_page = NULL;
  self->nav_view = NULL;
  g_clear_object (&self->back_button);
  g_clear_object (&self->header_search_clamp);
  g_clear_object (&self->search_bar);
  g_clear_object (&self->add_button);
  g_clear_object (&self->right_actions);

  g_clear_object (&self->instance);
  g_clear_object (&self->launcher);
  g_clear_pointer (&self->saves_dir, g_free);
  g_clear_pointer (&self->rows, g_ptr_array_unref);
  g_clear_pointer (&self->worlds, g_ptr_array_unref);

  G_OBJECT_CLASS (nuvashalauncher_worlds_page_parent_class)->dispose (object);
}

static void
nuvashalauncher_worlds_page_class_init (NuvashaLauncherWorldsPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = nuvashalauncher_worlds_page_dispose;
}

static void
nuvashalauncher_worlds_page_init (NuvashaLauncherWorldsPage *self)
{
  self->rows = g_ptr_array_new ();
  self->worlds = g_ptr_array_new_with_free_func ((GDestroyNotify) world_entry_free);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

static void worlds_attach_own_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header);
static void worlds_detach_own_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header);
static void worlds_attach_datapacks_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header);
static void worlds_detach_datapacks_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header);

static void
on_back_to_worlds_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);

  if (self->nav_view != NULL)
    adw_navigation_view_pop (self->nav_view);
}

static void
on_nav_view_popped (AdwNavigationView *nav_view G_GNUC_UNUSED,
                    AdwNavigationPage *page G_GNUC_UNUSED,
                    gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);

  if (self->datapacks_page == NULL)
    return;

  if (self->header_bar != NULL)
    worlds_detach_datapacks_header (self, self->header_bar);

  self->datapacks_page = NULL;

  if (self->header_bar != NULL)
    worlds_attach_own_header (self, self->header_bar);
}

static void
on_world_datapacks_clicked (GtkButton *button, gpointer user_data)
{
  NuvashaLauncherWorldsPage *self = NUVASHALAUNCHER_WORLDS_PAGE (user_data);
  WorldEntry *w = g_object_get_data (G_OBJECT (button), "nuvashalauncher-world");

  if (w == NULL || w->path == NULL || self->nav_view == NULL || self->datapacks_page != NULL)
    return;

  NuvashaLauncherContentPage *dp =
    nuvashalauncher_content_page_new_with_base (self->instance,
                                                NUVASHALAUNCHER_CONTENT_DATAPACK,
                                                w->path);
  if (dp == NULL)
    return;

  self->datapacks_page = dp;
  nuvashalauncher_content_page_reset_for_tab_switch (dp);

  AdwNavigationPage *dp_nav = adw_navigation_page_new (
    GTK_WIDGET (dp),
    w->name != NULL && *w->name != '\0' ? w->name : _("Datapacks"));
  adw_navigation_page_set_tag (dp_nav, "datapacks");

  if (self->header_bar != NULL)
    {
      worlds_detach_own_header (self, self->header_bar);
      worlds_attach_datapacks_header (self, self->header_bar);
    }

  adw_navigation_view_push (self->nav_view, dp_nav);
}

NuvashaLauncherWorldsPage *
nuvashalauncher_worlds_page_new (NuvashaLauncherInstance *instance,
                                 NuvashaLauncher *launcher)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_INSTANCE (instance), NULL);

  NuvashaLauncherWorldsPage *self = g_object_new (NUVASHALAUNCHER_TYPE_WORLDS_PAGE, NULL);
  g_autofree char *dir = NULL;

  self->instance = g_object_ref (instance);
  self->launcher = launcher != NULL ? g_object_ref (launcher) : NULL;
  g_object_get (instance, "directory", &dir, NULL);
  if (dir != NULL)
    self->saves_dir = g_build_filename (dir, "saves", NULL);

  GtkBox *inner = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_widget_set_vexpand (GTK_WIDGET (inner), TRUE);

  GtkBox *bar = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  gtk_widget_set_margin_top (GTK_WIDGET (bar), 8);
  gtk_widget_set_margin_bottom (GTK_WIDGET (bar), 8);
  gtk_widget_set_margin_start (GTK_WIDGET (bar), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (bar), 12);

  GtkWidget *add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_set_valign (add_btn, GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (add_btn, _("Add world…"));
  g_signal_connect_object (add_btn, "clicked", G_CALLBACK (on_add_world_clicked), self, 0);

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);

  GtkBox *right_actions = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  gtk_widget_set_valign (GTK_WIDGET (right_actions), GTK_ALIGN_CENTER);

  GtkWidget *folder_btn = gtk_button_new_from_icon_name ("folder-symbolic");
  nuvashalauncher_ui_prepare_icon_button (folder_btn, _("Open folder"));
  g_signal_connect_object (folder_btn, "clicked", G_CALLBACK (on_open_folder_clicked), self, 0);

  GtkWidget *refresh_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  nuvashalauncher_ui_prepare_icon_button (refresh_btn, _("Refresh"));
  g_signal_connect_object (refresh_btn, "clicked", G_CALLBACK (on_refresh_clicked), self, 0);

  gtk_box_append (right_actions, folder_btn);
  gtk_box_append (right_actions, refresh_btn);

  gtk_box_append (bar, add_btn);
  gtk_box_append (bar, spacer);
  gtk_box_append (bar, GTK_WIDGET (right_actions));
  gtk_box_append (inner, GTK_WIDGET (bar));
  self->bar = bar;
  self->add_button = g_object_ref_sink (add_btn);
  self->right_actions = g_object_ref_sink (GTK_WIDGET (right_actions));

  self->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);

  self->group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwClamp *clamp = ADW_CLAMP (adw_clamp_new ());
  gtk_widget_set_margin_top (GTK_WIDGET (clamp), 8);
  gtk_widget_set_margin_bottom (GTK_WIDGET (clamp), 16);
  gtk_widget_set_margin_start (GTK_WIDGET (clamp), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (clamp), 12);
  adw_clamp_set_child (clamp, GTK_WIDGET (self->group));

  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (scroller, GTK_WIDGET (clamp));

  AdwStatusPage *empty = ADW_STATUS_PAGE (adw_status_page_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (empty), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (empty), TRUE);
  adw_status_page_set_icon_name (empty, "applications-games-symbolic");
  adw_status_page_set_title (empty, _("No worlds"));

  GtkWidget *empty_add_btn = gtk_button_new_with_label (_("Add world…"));
  gtk_widget_add_css_class (empty_add_btn, "suggested-action");
  gtk_widget_add_css_class (empty_add_btn, "pill");
  gtk_widget_set_halign (empty_add_btn, GTK_ALIGN_CENTER);
  g_signal_connect_object (empty_add_btn, "clicked", G_CALLBACK (on_add_world_clicked), self, 0);
  adw_status_page_set_child (empty, empty_add_btn);

  GtkOverlay *drag_overlay = GTK_OVERLAY (gtk_overlay_new ());
  gtk_overlay_set_child (drag_overlay, GTK_WIDGET (scroller));
  self->drag_layer = GTK_FIXED (gtk_fixed_new ());
  gtk_widget_set_can_target (GTK_WIDGET (self->drag_layer), FALSE);
  gtk_overlay_add_overlay (drag_overlay, GTK_WIDGET (self->drag_layer));

  adw_view_stack_add_named (self->stack, GTK_WIDGET (drag_overlay), "list");
  adw_view_stack_add_named (self->stack, GTK_WIDGET (empty), "empty");
  gtk_box_append (inner, GTK_WIDGET (self->stack));

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, GTK_WIDGET (inner));

  self->nav_view = ADW_NAVIGATION_VIEW (adw_navigation_view_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->nav_view), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->nav_view), TRUE);
  {
    AdwNavigationPage *worlds_nav_page =
      adw_navigation_page_new (GTK_WIDGET (self->toasts), _("Worlds"));
    adw_navigation_page_set_tag (worlds_nav_page, "worlds");
    adw_navigation_view_add (self->nav_view, worlds_nav_page);
  }
  g_signal_connect_object (self->nav_view, "popped",
                           G_CALLBACK (on_nav_view_popped), self, 0);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->nav_view));

  GtkWidget *back_btn = gtk_button_new_from_icon_name ("go-previous-symbolic");
  gtk_widget_set_valign (back_btn, GTK_ALIGN_CENTER);
  nuvashalauncher_ui_prepare_icon_button (back_btn, _("Back"));
  g_signal_connect_object (back_btn, "clicked",
                           G_CALLBACK (on_back_to_worlds_clicked), self, 0);
  self->back_button = g_object_ref_sink (back_btn);

  self->search_bar = GTK_SEARCH_ENTRY (g_object_ref_sink (gtk_search_entry_new ()));
  gtk_search_entry_set_placeholder_text (self->search_bar, _("Search"));
  gtk_widget_set_hexpand (GTK_WIDGET (self->search_bar), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->search_bar), FALSE);
  g_signal_connect_object (self->search_bar, "changed",
                           G_CALLBACK (on_world_search_changed), self, 0);

  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  g_signal_connect_object (keys, "key-pressed",
                           G_CALLBACK (on_world_search_key), self, 0);

  nuvashalauncher_worlds_page_reload (self);
  return self;
}

void
nuvashalauncher_worlds_page_set_search_visible (NuvashaLauncherWorldsPage *self,
                                                gboolean visible)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_WORLDS_PAGE (self));
  if (self->datapacks_page != NULL)
    {
      nuvashalauncher_content_page_set_installed_search_visible (
        self->datapacks_page, visible);
      return;
    }
  if (self->search_bar == NULL)
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->search_bar), visible);
  if (self->header_search_clamp != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp), visible);
  if (visible)
    gtk_widget_grab_focus (GTK_WIDGET (self->search_bar));
  else
    {
      gtk_editable_set_text (GTK_EDITABLE (self->search_bar), "");
      gtk_widget_grab_focus (GTK_WIDGET (self));
    }
}

gboolean
nuvashalauncher_worlds_page_is_search_visible (NuvashaLauncherWorldsPage *self)
{
  g_return_val_if_fail (NUVASHALAUNCHER_IS_WORLDS_PAGE (self), FALSE);
  if (self->datapacks_page != NULL)
    return nuvashalauncher_content_page_is_installed_search_visible (
      self->datapacks_page);
  return self->search_bar != NULL &&
         gtk_widget_get_visible (GTK_WIDGET (self->search_bar));
}

static void
worlds_attach_own_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header)
{
  if (self->header_search_clamp == NULL)
    {
      self->header_search_clamp = ADW_CLAMP (g_object_ref_sink (adw_clamp_new ()));
      adw_clamp_set_maximum_size (self->header_search_clamp, 460);
      gtk_widget_set_hexpand (GTK_WIDGET (self->header_search_clamp), TRUE);
    }

  adw_clamp_set_child (self->header_search_clamp, GTK_WIDGET (self->search_bar));
  gtk_widget_set_visible (GTK_WIDGET (self->header_search_clamp),
                          gtk_widget_get_visible (GTK_WIDGET (self->search_bar)));
  adw_header_bar_set_title_widget (header, GTK_WIDGET (self->header_search_clamp));

  if (gtk_widget_get_parent (self->add_button) == GTK_WIDGET (self->bar))
    gtk_box_remove (self->bar, self->add_button);
  adw_header_bar_pack_start (header, self->add_button);

  if (gtk_widget_get_parent (self->right_actions) == GTK_WIDGET (self->bar))
    gtk_box_remove (self->bar, self->right_actions);
  adw_header_bar_pack_end (header, self->right_actions);

  gtk_widget_set_visible (GTK_WIDGET (self->bar), FALSE);
}

static void
worlds_detach_own_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header)
{
  adw_header_bar_set_title_widget (header, NULL);
  if (gtk_widget_is_ancestor (self->add_button, GTK_WIDGET (header)))
    adw_header_bar_remove (header, self->add_button);
  if (gtk_widget_is_ancestor (self->right_actions, GTK_WIDGET (header)))
    adw_header_bar_remove (header, self->right_actions);
  if (gtk_widget_get_parent (self->add_button) != GTK_WIDGET (self->bar))
    gtk_box_prepend (self->bar, self->add_button);
  if (gtk_widget_get_parent (self->right_actions) != GTK_WIDGET (self->bar))
    gtk_box_append (self->bar, self->right_actions);
  gtk_widget_set_visible (GTK_WIDGET (self->bar), TRUE);
  if (self->header_search_clamp != NULL)
    adw_clamp_set_child (self->header_search_clamp, NULL);
}

static void
worlds_attach_datapacks_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header)
{
  if (self->back_button != NULL &&
      !gtk_widget_is_ancestor (self->back_button, GTK_WIDGET (header)))
    adw_header_bar_pack_start (header, self->back_button);
  if (self->datapacks_page != NULL)
    nuvashalauncher_content_page_attach_header (self->datapacks_page, header);
}

static void
worlds_detach_datapacks_header (NuvashaLauncherWorldsPage *self, AdwHeaderBar *header)
{
  if (self->datapacks_page != NULL)
    nuvashalauncher_content_page_detach_header (self->datapacks_page, header);
  if (self->back_button != NULL &&
      gtk_widget_is_ancestor (self->back_button, GTK_WIDGET (header)))
    adw_header_bar_remove (header, self->back_button);
}

void
nuvashalauncher_worlds_page_attach_header (NuvashaLauncherWorldsPage *self,
                                           AdwHeaderBar *header)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_WORLDS_PAGE (self));
  g_return_if_fail (ADW_IS_HEADER_BAR (header));

  if (self->header_bar == header)
    return;
  if (self->header_bar != NULL)
    nuvashalauncher_worlds_page_detach_header (self, self->header_bar);

  self->header_bar = header;
  if (self->datapacks_page != NULL)
    worlds_attach_datapacks_header (self, header);
  else
    worlds_attach_own_header (self, header);
}

void
nuvashalauncher_worlds_page_detach_header (NuvashaLauncherWorldsPage *self,
                                           AdwHeaderBar *header)
{
  g_return_if_fail (NUVASHALAUNCHER_IS_WORLDS_PAGE (self));
  if (self->header_bar != header || header == NULL)
    return;

  self->header_bar = NULL;
  if (self->datapacks_page != NULL)
    worlds_detach_datapacks_header (self, header);
  else
    worlds_detach_own_header (self, header);
}
