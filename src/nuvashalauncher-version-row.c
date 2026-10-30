/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherVersionRow: Minecraft version picker backed by Mojang's manifest.
 * It exposes loading and retry states and returns no selection until a
 * version has been chosen. */

#include "nuvashalauncher-version-row.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"

#include "nuvashalauncher-minecraft-versions.h"

struct _nuvashalauncherVersionRow
{
  AdwComboRow parent_instance;

  nuvashalauncherSettings *settings;

  /* Cancelled when a load replaces it or when the row goes away. */
  GCancellable *cancellable;

  /* The load the row is waiting for. Borrowed from the ManifestAsk that
     started it: that one releases it when the answer arrives, and a newer
     load replaces it before an older answer can arrive. */
  nuvashalauncherTaskScope *operation;

  /* The version ids behind the model, in the order the row shows
   * them. Never NULL, empty while there is no list. */
  GPtrArray *ids;

  /* The version the row was asked to pick, or NULL for none. */
  char *wanted;

  /* Set only once the model holds the versions of the manifest: a
   * position in the model means nothing before that. */
  gboolean ready;

  /* Borrowed from the row's own suffixes, cleared in dispose(). */
  GtkButton *retry;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherVersionRow, nuvashalauncher_version_row, ADW_TYPE_COMBO_ROW)

/* ---------------------------------------------------------- *
 * Showing one state
 * ---------------------------------------------------------- */

/* Puts @ids behind the model, in that order, and picks @position,
 * which is a position in @ids or GTK_INVALID_LIST_POSITION for
 * nothing. @ids is taken over. */
static void
set_entries (nuvashalauncherVersionRow *self, GPtrArray *ids, guint position)
{
  g_autofree const char **strings = g_new0 (const char *, ids->len + 1);
  g_autoptr (GtkStringList) list = NULL;

  for (guint i = 0; i < ids->len; i++)
    strings[i] = g_ptr_array_index (ids, i);

  list = gtk_string_list_new (strings);

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  self->ids = g_steal_pointer (&ids);

  adw_combo_row_set_model (ADW_COMBO_ROW (self), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self), position);
}

static void
show_loading (nuvashalauncherVersionRow *self)
{
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);

   g_ptr_array_add (ids, g_strdup (_("Loading…")));

  self->ready = FALSE;
  set_entries (self, g_steal_pointer (&ids), 0);

  /* The row does not answer while it does not know the versions. */
  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);
}

static void
show_error (nuvashalauncherVersionRow *self, const char *message)
{
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);

  self->ready = FALSE;
  set_entries (self, g_steal_pointer (&ids), GTK_INVALID_LIST_POSITION);

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), message);
  /* The Retry button is a suffix of the row, so the row itself has to
   * answer clicks for it to be reachable. */
  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), TRUE);
}

/* Fills the row from a manifest that arrived. */
static void
fill (nuvashalauncherVersionRow *self, nuvashalauncherVersionManifest *manifest)
{
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  gboolean unstable =
    nuvashalauncher_settings_get_show_unstable_versions (self->settings);
  guint position = GTK_INVALID_LIST_POSITION;

  self->ready = FALSE;

  for (guint i = 0; i < manifest->entries->len; i++)
    {
      nuvashalauncherVersionEntry *entry = g_ptr_array_index (manifest->entries, i);

      if (self->wanted != NULL && g_str_equal (entry->id, self->wanted))
        {
          /* The version an instance already uses stays in the list,
           * even when the filter would hide its type. */
          position = ids->len;
        }
      else if (!unstable && entry->type != nuvashalauncher_MC_TYPE_RELEASE)
        continue;

      g_ptr_array_add (ids, g_strdup (entry->id));
    }

  /* Preserve the instance's version if Mojang removed it from the manifest. */
  if (self->wanted != NULL && position == GTK_INVALID_LIST_POSITION)
    {
      g_ptr_array_insert (ids, 0, g_strdup (self->wanted));
      position = 0;
    }

  /* A new instance always starts on Mojang's latest stable release.  The
     manifest is normally newest first, but its explicit latest.release
     field is authoritative and remains stable when snapshots are shown. */
  if (self->wanted == NULL && position == GTK_INVALID_LIST_POSITION)
    {
      for (guint i = 0; i < ids->len; i++)
        {
          if (g_str_equal (g_ptr_array_index (ids, i), manifest->latest_release))
            {
              position = i;
              break;
            }
        }
    }

  /* Mark the row as unavailable while replacing the model. This prevents
     GtkSingleSelection's temporary first item from being treated as user input. */
  set_entries (self, g_steal_pointer (&ids), position);

  self->ready = TRUE;
  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);

  g_object_notify (G_OBJECT (self), "selected");
}

/* ---------------------------------------------------------- *
 * Fetching the manifest
 * ---------------------------------------------------------- */

/* One load of the manifest: the row, which can be disposed while the
   answer is on its way, and the handle that answer is meant to release. */
typedef struct
{
  nuvashalauncherVersionRow *row;
  nuvashalauncherTaskScope *operation;
} ManifestAsk;

static void
manifest_ask_free (ManifestAsk *ask)
{
  nuvashalauncherVersionRow *row = ask->row;

  /* The row's own pointer is a loan of this reference, so it is given
     back before the reference goes. */
  if (row != NULL && row->operation == ask->operation)
    row->operation = NULL;

  g_clear_object (&ask->row);

  if (ask->operation != NULL)
    nuvashalauncher_task_scope_unref (ask->operation);

  g_free (ask);
}

static void
manifest_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  ManifestAsk *ask = user_data;
  nuvashalauncherVersionRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  g_autoptr (nuvashalauncherVersionManifest) manifest = NULL;

  /* Ignore results from an operation replaced by a newer load. */
  if (self != NULL && self->operation == ask->operation)
    {
      manifest = nuvashalauncher_version_manifest_fetch_finish (result, &error);

      if (manifest != NULL)
        fill (self, manifest);
      else
        show_error (self, error->message);
    }

  /* The reference the load took on the row, and its handle. */
  manifest_ask_free (ask);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherVersionRow *self = nuvashalauncher_VERSION_ROW (user_data);
  g_autofree char *wanted = NULL;

  if (self->settings == NULL)
    return;

  wanted = g_strdup (self->wanted);
  nuvashalauncher_version_row_load (self, self->settings, wanted);
}

/* ---------------------------------------------------------- *
 * The public face
 * ---------------------------------------------------------- */

nuvashalauncherVersionRow *
nuvashalauncher_version_row_new (void)
{
  return g_object_new (nuvashalauncher_TYPE_VERSION_ROW, NULL);
}

void
nuvashalauncher_version_row_load (nuvashalauncherVersionRow *self,
                      nuvashalauncherSettings   *settings,
                      const char    *selected)
{
  g_return_if_fail (nuvashalauncher_IS_VERSION_ROW (self));
  g_return_if_fail (nuvashalauncher_IS_SETTINGS (settings));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_set_object (&self->settings, settings);

  g_free (self->wanted);
  self->wanted = g_strdup (selected);

  show_loading (self);

{
    ManifestAsk *ask = g_new0 (ManifestAsk, 1);

    ask->row = g_object_ref (self);
    ask->operation = nuvashalauncher_version_manifest_fetch_async (nuvashalauncher_VERSION_MANIFEST_URL,
                                                       manifest_loaded, ask);
    nuvashalauncher_task_scope_follow_cancellable (ask->operation, self->cancellable);
    self->operation = ask->operation;
  }
}

const char *
nuvashalauncher_version_row_get_selected (nuvashalauncherVersionRow *self)
{
  guint position;

  g_return_val_if_fail (nuvashalauncher_IS_VERSION_ROW (self), NULL);

  /* The model is replaced while the row loads, so a position in it
   * means nothing until the versions are there. */
  if (!self->ready)
    return NULL;

  position = adw_combo_row_get_selected (ADW_COMBO_ROW (self));

  if (position == GTK_INVALID_LIST_POSITION || position >= self->ids->len)
    return NULL;

  return g_ptr_array_index (self->ids, position);
}

void
nuvashalauncher_version_row_select (nuvashalauncherVersionRow *self, const char *version)
{
  g_return_if_fail (nuvashalauncher_IS_VERSION_ROW (self));

  if (version == NULL || !self->ready)
    return;

  for (guint i = 0; i < self->ids->len; i++)
    {
      if (g_str_equal (g_ptr_array_index (self->ids, i), version))
        {
          adw_combo_row_set_selected (ADW_COMBO_ROW (self), i);
          return;
        }
    }
}

/* ---------------------------------------------------------- *
 * The GObject
 * ---------------------------------------------------------- */

static void
nuvashalauncher_version_row_dispose (GObject *object)
{
  nuvashalauncherVersionRow *self = nuvashalauncher_VERSION_ROW (object);

  /* A load still in flight only holds a reference on this row;
   * cancelling it and dropping the settings stops it from writing
   * into a row that is on its way out. */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->settings);
  self->retry = NULL;

  G_OBJECT_CLASS (nuvashalauncher_version_row_parent_class)->dispose (object);
}

static void
nuvashalauncher_version_row_finalize (GObject *object)
{
  nuvashalauncherVersionRow *self = nuvashalauncher_VERSION_ROW (object);

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  g_free (self->wanted);

  G_OBJECT_CLASS (nuvashalauncher_version_row_parent_class)->finalize (object);
}

static void
nuvashalauncher_version_row_class_init (nuvashalauncherVersionRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_version_row_dispose;
  object_class->finalize = nuvashalauncher_version_row_finalize;
}

static void
nuvashalauncher_version_row_init (nuvashalauncherVersionRow *self)
{
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);

  /* A Minecraft version is a name from Mojang, not markup. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self),
                                  _("Minecraft Version"));

  /* Nothing to choose until a load fills the list. */
  self->ready = FALSE;
  set_entries (self, g_steal_pointer (&ids), GTK_INVALID_LIST_POSITION);
  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);

  self->retry = GTK_BUTTON (gtk_button_new_with_label (_("Retry")));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (self->retry), _("Retry"));
  gtk_widget_set_valign (GTK_WIDGET (self->retry), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  g_signal_connect_object (self->retry, "clicked", G_CALLBACK (on_retry_clicked),
                           self, 0);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self), GTK_WIDGET (self->retry));
}
