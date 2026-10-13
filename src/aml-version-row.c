/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlVersionRow: filling the "Minecraft Version" row from Mojang's
 * version manifest.
 *
 * The row only ever shows what the manifest holds. Until the manifest
 * arrives it is insensitive and says that it is loading; when the
 * fetch fails it says why and offers a Retry; when it works the list
 * holds the version ids, releases only unless the settings ask for the
 * rest. The position in the model means nothing before that, so
 * get_selected() answers nothing at all while the row is loading or
 * empty: a consumer that saves what it is told never saves a version
 * nobody picked. */

#include "aml-version-row.h"
#include "aml-i18n.h"

#include "aml-minecraft-versions.h"

struct _AmlVersionRow
{
  AdwComboRow parent_instance;

  AmlSettings *settings;

  /* Cancelled when a load replaces it or when the row goes away. */
  GCancellable *cancellable;

  /* The load the row is waiting for. Borrowed from the ManifestAsk that
     started it: that one releases it when the answer arrives, and a newer
     load replaces it before an older answer can arrive. */
  AmlTaskScope *operation;

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

G_DEFINE_FINAL_TYPE (AmlVersionRow, aml_version_row, ADW_TYPE_COMBO_ROW)

/* ---------------------------------------------------------- *
 * Showing one state
 * ---------------------------------------------------------- */

/* Puts @ids behind the model, in that order, and picks @position,
 * which is a position in @ids or GTK_INVALID_LIST_POSITION for
 * nothing. @ids is taken over. */
static void
set_entries (AmlVersionRow *self, GPtrArray *ids, guint position)
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
show_loading (AmlVersionRow *self)
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
show_error (AmlVersionRow *self, const char *message)
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
fill (AmlVersionRow *self, AmlVersionManifest *manifest)
{
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  gboolean snapshots = aml_settings_get_show_snapshots (self->settings);
  guint position = GTK_INVALID_LIST_POSITION;

  self->ready = FALSE;

  for (guint i = 0; i < manifest->entries->len; i++)
    {
      AmlVersionEntry *entry = g_ptr_array_index (manifest->entries, i);

      if (self->wanted != NULL && g_str_equal (entry->id, self->wanted))
        {
          /* The version an instance already uses stays in the list,
           * even when the filter would hide its type. */
          position = ids->len;
        }
      else if (!snapshots && entry->type != AML_MC_TYPE_RELEASE)
        continue;

      g_ptr_array_add (ids, g_strdup (entry->id));
    }

  /* A version that was asked for but the manifest does not hold at all
     (an instance that runs a version Mojang has since removed) stays in
     the list on its own, so opening the instance shows what it runs and
     never changes it. */
  if (self->wanted != NULL && position == GTK_INVALID_LIST_POSITION)
    {
      g_ptr_array_insert (ids, 0, g_strdup (self->wanted));
      position = 0;
    }

  /* Nothing was asked for: the newest version is what a new instance is
     made with, and the manifest is newest first, so that is the first
     entry that was kept. */
  if (self->wanted == NULL && position == GTK_INVALID_LIST_POSITION &&
      ids->len > 0)
    position = 0;

  /* The model is replaced with the row marked as not ready, so the
     selection it makes on its own (GtkSingleSelection picks the first
     item) is not read by a screen as a version the user picked. Once the
     real selection is in place, the row says so a single time, and that
     answer is the version the screens store. */
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
  AmlVersionRow *row;
  AmlTaskScope *operation;
} ManifestAsk;

static void
manifest_ask_free (ManifestAsk *ask)
{
  AmlVersionRow *row = ask->row;

  /* The row's own pointer is a loan of this reference, so it is given
     back before the reference goes. */
  if (row != NULL && row->operation == ask->operation)
    row->operation = NULL;

  g_clear_object (&ask->row);

  if (ask->operation != NULL)
    aml_task_scope_unref (ask->operation);

  g_free (ask);
}

static void
manifest_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  ManifestAsk *ask = user_data;
  AmlVersionRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlVersionManifest) manifest = NULL;

  /* The answer belongs to the load this row already replaced, which
   * was cancelled on the way out: it says nothing about the list the
   * row shows now. */
  if (self != NULL && self->operation == ask->operation)
    {
      manifest = aml_version_manifest_fetch_finish (result, &error);

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
  AmlVersionRow *self = user_data;
  g_autofree char *wanted = NULL;

  if (self->settings == NULL)
    return;

  wanted = g_strdup (self->wanted);
  aml_version_row_load (self, self->settings, wanted);
}

/* ---------------------------------------------------------- *
 * The public face
 * ---------------------------------------------------------- */

AmlVersionRow *
aml_version_row_new (void)
{
  return g_object_new (AML_TYPE_VERSION_ROW, NULL);
}

void
aml_version_row_load (AmlVersionRow *self,
                      AmlSettings   *settings,
                      const char    *selected)
{
  g_return_if_fail (AML_IS_VERSION_ROW (self));
  g_return_if_fail (AML_IS_SETTINGS (settings));

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
    ask->operation = aml_version_manifest_fetch_async (AML_VERSION_MANIFEST_URL,
                                                       manifest_loaded, ask);
    aml_task_scope_follow_cancellable (ask->operation, self->cancellable);
    self->operation = ask->operation;
  }
}

const char *
aml_version_row_get_selected (AmlVersionRow *self)
{
  guint position;

  g_return_val_if_fail (AML_IS_VERSION_ROW (self), NULL);

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
aml_version_row_select (AmlVersionRow *self, const char *version)
{
  g_return_if_fail (AML_IS_VERSION_ROW (self));

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
aml_version_row_dispose (GObject *object)
{
  AmlVersionRow *self = AML_VERSION_ROW (object);

  /* A load still in flight only holds a reference on this row;
   * cancelling it and dropping the settings stops it from writing
   * into a row that is on its way out. */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->settings);
  self->retry = NULL;

  G_OBJECT_CLASS (aml_version_row_parent_class)->dispose (object);
}

static void
aml_version_row_finalize (GObject *object)
{
  AmlVersionRow *self = AML_VERSION_ROW (object);

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  g_free (self->wanted);

  G_OBJECT_CLASS (aml_version_row_parent_class)->finalize (object);
}

static void
aml_version_row_class_init (AmlVersionRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_version_row_dispose;
  object_class->finalize = aml_version_row_finalize;
}

static void
aml_version_row_init (AmlVersionRow *self)
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
  gtk_widget_add_css_class (GTK_WIDGET (self->retry), "flat");
  gtk_widget_set_valign (GTK_WIDGET (self->retry), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  g_signal_connect (self->retry, "clicked", G_CALLBACK (on_retry_clicked),
                    self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self), GTK_WIDGET (self->retry));
}
