/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-loader-row.h"

struct _AmlLoaderRow
{
  GObject parent_instance;

  AmlSettings *settings;

  /* Plain AdwComboRows, owned here and added to a group by whoever made
     this. Both hold a reference back, so either may go first. */
  GtkWidget *loader_row;
  GtkWidget *version_row;
  GtkWidget *retry;

  /* Cancelled when a load replaces it or when the row goes away. */
  GCancellable *cancellable;

  /* The versions of the loader, in the order the row shows them, and the
     game version they were fetched for. */
  GPtrArray *versions; /* AmlLoaderVersion* */
  char *game_version;
  char *wanted;

  /* Set only once the model holds the versions of the loader: a
     position in it means nothing before that. */
  gboolean ready;
};

G_DEFINE_FINAL_TYPE (AmlLoaderRow, aml_loader_row, G_TYPE_OBJECT)

static guint aml_loader_row_signals[AML_LOADER_ROW_N_SIGNALS];

/* ---------------------------------------------------------- *
 * The two rows
 * ---------------------------------------------------------- */

/* The handler of both combos: the change belongs to the row a screen
   stored, not to the combo it happened in, and a load that puts its own
   list in place is not a choice the user made. */
static void
on_selected (GObject *object G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED,
             gpointer user_data)
{
  AmlLoaderRow *self = user_data;

  if (!self->ready)
    return;

  g_signal_emit (self, aml_loader_row_signals[AML_LOADER_ROW_SIGNAL_CHANGED], 0);
}

/* The model of the first row holds one name per loader AML installs, in
   the order aml_loaders_supported() gives, so a position in it is that
   position in the list. */
static AmlLoader
loader_at (guint position)
{
  const AmlLoader *loaders = aml_loaders_supported ();

  if (position == GTK_INVALID_LIST_POSITION ||
      loaders[position] == AML_LOADER_VANILLA)
    return AML_LOADER_VANILLA;

  return loaders[position];
}

static AmlLoader
selected_loader (AmlLoaderRow *self)
{
  return loader_at (adw_combo_row_get_selected (ADW_COMBO_ROW (self->loader_row)));
}

/* Vanilla has no loader version: the row is not part of a choice then. */
static void
update_visibility (AmlLoaderRow *self)
{
  gboolean vanilla = selected_loader (self) == AML_LOADER_VANILLA;

  gtk_widget_set_visible (self->version_row, !vanilla);
}

/* Puts @labels behind the model of the version row and picks @position,
   which is GTK_INVALID_LIST_POSITION for nothing. */
static void
set_versions (AmlLoaderRow *self, GPtrArray *versions, guint position)
{
  g_autoptr (GtkStringList) list = NULL;
  g_autofree const char **labels = NULL;

  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = g_steal_pointer (&versions);

  labels = g_new0 (const char *, self->versions->len + 1);

  for (guint i = 0; i < self->versions->len; i++)
    {
      const AmlLoaderVersion *version = g_ptr_array_index (self->versions, i);

      /* What a user reads says which of these the loader does not call
         stable. */
      labels[i] = version->stable ? g_strdup (version->version)
                                  : g_strdup_printf ("%s (unstable)",
                                                     version->version);
    }

  list = gtk_string_list_new (labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row),
                           G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->version_row), position);
}

static void
show_loading (AmlLoaderRow *self)
{
  g_autoptr (GtkStringList) list = NULL;
  const char * const loading[] = { "Loading…", NULL };

  list = gtk_string_list_new (loading);

  self->ready = FALSE;
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_loader_version_free);

  adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row),
                           G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->version_row), 0);

  /* The row does not answer while it does not know the versions. */
  gtk_widget_set_sensitive (self->version_row, FALSE);
  gtk_widget_set_visible (self->retry, FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), NULL);
}

/* @retry is FALSE when trying again would not help, which is the case
   for a loader that cannot say anything before a Minecraft version is
   picked. */
static void
show_error (AmlLoaderRow *self, const char *message, gboolean retry)
{
  g_autoptr (GtkStringList) list = NULL;
  const char *nothing[] = { "None", NULL };

  self->ready = FALSE;
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_loader_version_free);

  list = gtk_string_list_new (nothing);
  adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->version_row),
                              GTK_INVALID_LIST_POSITION);

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), message);
  /* The Retry button is a suffix of the row, so the row itself has to
     answer clicks for it to be reachable. */
  gtk_widget_set_sensitive (self->version_row, retry);
  gtk_widget_set_visible (self->retry, retry);
}

/* Fills the version row from the versions a loader published. */
static void
fill (AmlLoaderRow *self, GPtrArray *published)
{
  g_autoptr (GPtrArray) shown =
    g_ptr_array_new_with_free_func ((GDestroyNotify) aml_loader_version_free);
  gboolean unstable =
    self->settings != NULL && aml_settings_get_show_unstable_loaders (self->settings);
  guint position = GTK_INVALID_LIST_POSITION;

  self->ready = FALSE;

  for (guint i = 0; i < published->len; i++)
    {
      const AmlLoaderVersion *version = g_ptr_array_index (published, i);
      AmlLoaderVersion *copy = g_new0 (AmlLoaderVersion, 1);

      copy->version = g_strdup (version->version);
      copy->stable = version->stable;

      if (self->wanted != NULL && g_str_equal (version->version, self->wanted))
        position = shown->len;
      else if (!version->stable && !unstable)
        continue;

      g_ptr_array_add (shown, copy);
    }

  set_versions (self, g_steal_pointer (&shown), position);

  self->ready = TRUE;
  gtk_widget_set_sensitive (self->version_row, TRUE);
  gtk_widget_set_visible (self->retry, FALSE);

  /* Nothing to pick is not the same as something to pick: the row says
     so instead of answering with a version. */
  if (self->versions->len == 0)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row),
                                 "This loader has no version for this "
                                 "Minecraft version");
  else
    adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), NULL);
}

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

static void
versions_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  AmlLoaderRow *self = user_data;
  g_autoptr (GError) error = NULL;
  GPtrArray *versions = NULL;

  /* The answer belongs to the load this row already replaced, which was
     cancelled on the way out. */
  if (g_task_get_cancellable (G_TASK (result)) == self->cancellable)
    {
      versions = aml_loaders_versions_finish (result, &error);

      if (versions != NULL)
        fill (self, versions);
      else
        show_error (self, error->message, TRUE);
    }

  g_object_unref (self);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlLoaderRow *self = user_data;
  g_autofree char *wanted = NULL;
  g_autofree char *game_version = NULL;

  if (self->settings == NULL)
    return;

  wanted = g_strdup (self->wanted);
  game_version = g_strdup (self->game_version);
  aml_loader_row_load (self, self->settings, selected_loader (self), wanted,
                       game_version);
}

/* ---------------------------------------------------------- *
 * The public face
 * ---------------------------------------------------------- */

AmlLoaderRow *
aml_loader_row_new (void)
{
  AmlLoaderRow *self = g_object_new (AML_TYPE_LOADER_ROW, NULL);
  const AmlLoader *loaders = aml_loaders_supported ();
  g_autofree const char **names = NULL;
  guint count = 0;

  /* Only the loaders AML installs, in the order they are read. */
  count = aml_loaders_supported_count ();
  names = g_new0 (const char *, count + 1);

  for (guint i = 0; i < count; i++)
    names[i] = aml_loader_display_name (loaders[i]);

  self->loader_row = g_object_ref_sink (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->loader_row),
                                 "Loader");
  adw_combo_row_set_model (ADW_COMBO_ROW (self->loader_row),
                           G_LIST_MODEL (gtk_string_list_new (names)));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->loader_row), 0);

  self->version_row = g_object_ref_sink (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->version_row),
                                 "Loader Version");
  self->retry = g_object_ref_sink (gtk_button_new_with_label ("Retry"));
  gtk_widget_add_css_class (self->retry, "flat");
  gtk_widget_set_valign (self->retry, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->retry, FALSE);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->version_row), self->retry);

  /* Every handler is connected with the object, so a row that a group
     still holds after this is gone cannot call back into it. */
  g_signal_connect_object (self->loader_row, "notify::selected",
                           G_CALLBACK (on_selected), self, 0);
  g_signal_connect_object (self->version_row, "notify::selected",
                           G_CALLBACK (on_selected), self, 0);
  g_signal_connect_object (self->retry, "clicked",
                           G_CALLBACK (on_retry_clicked), self, 0);

  show_loading (self);
  update_visibility (self);

  return self;
}

GtkWidget *
aml_loader_row_get_loader_row (AmlLoaderRow *self)
{
  g_return_val_if_fail (AML_IS_LOADER_ROW (self), NULL);

  return self->loader_row;
}

GtkWidget *
aml_loader_row_get_version_row (AmlLoaderRow *self)
{
  g_return_val_if_fail (AML_IS_LOADER_ROW (self), NULL);

  return self->version_row;
}

void
aml_loader_row_load (AmlLoaderRow *self,
                     AmlSettings   *settings,
                     AmlLoader      loader,
                     const char    *loader_version,
                     const char    *game_version)
{
  const AmlLoader *loaders = aml_loaders_supported ();

  g_return_if_fail (AML_IS_LOADER_ROW (self));
  g_return_if_fail (settings == NULL || AML_IS_SETTINGS (settings));

  /* The loader row shows what was asked for, whether AML knows it or
     not, so a screen that saves the choice stores what the user saw. */
  for (guint i = 0; i < aml_loaders_supported_count (); i++)
    {
      if (loaders[i] == loader)
        adw_combo_row_set_selected (ADW_COMBO_ROW (self->loader_row), i);
    }

  g_set_object (&self->settings, settings);

  g_free (self->game_version);
  self->game_version = g_strdup (game_version);
  g_free (self->wanted);
  self->wanted = g_strdup (loader_version);

  update_visibility (self);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  /* Vanilla has nothing to load. */
  if (loader == AML_LOADER_VANILLA)
    {
      self->ready = FALSE;
      g_clear_pointer (&self->versions, g_ptr_array_unref);
      self->versions = g_ptr_array_new_with_free_func (
        (GDestroyNotify) aml_loader_version_free);
      return;
    }

  show_loading (self);

  /* A loader needs a Minecraft version to say which of its versions run
     it; without one there is nothing to ask for. */
  if (game_version == NULL || *game_version == '\0')
    {
      show_error (self, "Pick a Minecraft version first", FALSE);
      return;
    }

  aml_loaders_versions_async (loader, game_version, self->cancellable,
                              versions_loaded, g_object_ref (self));
}

AmlLoader
aml_loader_row_get_loader (AmlLoaderRow *self)
{
  g_return_val_if_fail (AML_IS_LOADER_ROW (self), AML_LOADER_VANILLA);

  return selected_loader (self);
}

const char *
aml_loader_row_get_version (AmlLoaderRow *self)
{
  guint position;

  g_return_val_if_fail (AML_IS_LOADER_ROW (self), NULL);

  if (!self->ready)
    return NULL;

  position = adw_combo_row_get_selected (ADW_COMBO_ROW (self->version_row));

  if (position == GTK_INVALID_LIST_POSITION || position >= self->versions->len)
    return NULL;

  {
    const AmlLoaderVersion *version = g_ptr_array_index (self->versions,
                                                        position);

    return version->version;
  }
}

/* ---------------------------------------------------------- *
 * The GObject
 * ---------------------------------------------------------- */

static void
aml_loader_row_dispose (GObject *object)
{
  AmlLoaderRow *self = AML_LOADER_ROW (object);

  /* A load still in flight only holds a reference on this object;
     cancelling it and dropping the settings stops it from writing into a
     row that is on its way out. */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->settings);

  /* The rows are ours. A group that still holds one keeps it alive, and
     its handlers went with this object. */
  g_clear_object (&self->retry);
  g_clear_object (&self->version_row);
  g_clear_object (&self->loader_row);

  G_OBJECT_CLASS (aml_loader_row_parent_class)->dispose (object);
}

static void
aml_loader_row_finalize (GObject *object)
{
  AmlLoaderRow *self = AML_LOADER_ROW (object);

  g_clear_pointer (&self->versions, g_ptr_array_unref);
  g_free (self->game_version);
  g_free (self->wanted);

  G_OBJECT_CLASS (aml_loader_row_parent_class)->finalize (object);
}

static void
aml_loader_row_class_init (AmlLoaderRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_loader_row_dispose;
  object_class->finalize = aml_loader_row_finalize;

  aml_loader_row_signals[AML_LOADER_ROW_SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
aml_loader_row_init (AmlLoaderRow *self)
{
  /* Everything is built in aml_loader_row_new(), which needs the loaders
     AML installs. */
  (void) self;
}