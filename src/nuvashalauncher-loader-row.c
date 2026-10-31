/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-loader-row.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-download.h"

/* Said when a load fails without anything to say: the versions are gone
   and the row has to tell the user that in words. */
#define NO_VERSION_MESSAGE \
  _("the versions of this loader could not be read")

struct _nuvashalauncherLoaderRow
{
  GObject parent_instance;

  nuvashalauncherSettings *settings;

  /* Plain AdwComboRows, owned here and added to a group by whoever made
     this. Both hold a reference back, so either may go first. */
  AdwComboRow *loader_row;
  AdwComboRow *version_row;
  GtkButton *retry;

  /* Cancelled when a load replaces it or when the row goes away. */
  GCancellable *cancellable;

  /* The load the row is waiting for. Borrowed from the VersionsAsk that
     started it: that one releases it when the answer arrives, and a newer
     load replaces it before an older answer can arrive. */
  nuvashalauncherTaskScope *operation;

  /* The versions of the loader, in the order the row shows them, and the
     game version they were fetched for. */
  GPtrArray *versions; /* nuvashalauncherLoaderVersion* */
  char *game_version;
  char *wanted;

  /* The loader the row is showing or fetching for, so that a change the
     row itself makes to the combo (putting the stored loader in place)
     is not read back as a choice the user made. */
  nuvashalauncherLoader current;

  /* TRUE while the game runs: neither combo answers then, and a screen
     must not write down a choice made under it. */
  gboolean blocked;

  /* Whether the version row may answer at all: FALSE while it is loading
     or has nothing to show, TRUE once a list is in place. */
  gboolean interactive;

  /* Set only once the model holds the versions of the loader: a
     position in it means nothing before that. */
  gboolean ready;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherLoaderRow, nuvashalauncher_loader_row, G_TYPE_OBJECT)

static guint nuvashalauncher_loader_row_signals[nuvashalauncher_LOADER_ROW_N_SIGNALS];

/* ---------------------------------------------------------- *
 * The two rows
 * ---------------------------------------------------------- */

/* The handler of the version combo: the change belongs to the row a
   screen stored, not to the combo it happened in, and a load that puts
   its own list in place is not a choice the user made. */
static void
on_version_selected (GObject *object G_GNUC_UNUSED,
                     GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherLoaderRow *self = nuvashalauncher_LOADER_ROW (user_data);

  if (!self->ready || self->blocked)
    return;

  g_signal_emit (self, nuvashalauncher_loader_row_signals[nuvashalauncher_LOADER_ROW_SIGNAL_CHANGED], 0);
}

static void start_versions (nuvashalauncherLoaderRow *self, nuvashalauncherLoader loader,
                            const char *wanted);
static nuvashalauncherLoader loader_at (guint position);

/* Picking another loader is a question for the version list, so the row
   asks it again itself: a screen never has to tell it to. */
static void
on_loader_selected (GObject *object G_GNUC_UNUSED,
                    GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherLoaderRow *self = nuvashalauncher_LOADER_ROW (user_data);
  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;

  if (self->blocked)
    return;

  /* What the row itself put in the combo, or a change already being
     answered, is not a second question. */
  loader = loader_at (
    adw_combo_row_get_selected (ADW_COMBO_ROW (self->loader_row)));

  if (loader == self->current)
    return;

  start_versions (self, loader, NULL);
}

/* The model of the first row holds one name per loader nuvashalauncher installs, in
   the order nuvashalauncher_loaders_supported() gives, so a position in it is that
   position in the list. */
static nuvashalauncherLoader
loader_at (guint position)
{
  const nuvashalauncherLoader *loaders = nuvashalauncher_loaders_supported ();

  if (position == GTK_INVALID_LIST_POSITION ||
      loaders[position] == nuvashalauncher_LOADER_VANILLA)
    return nuvashalauncher_LOADER_VANILLA;

  return loaders[position];
}

static nuvashalauncherLoader
selected_loader (nuvashalauncherLoaderRow *self)
{
  return loader_at (adw_combo_row_get_selected (ADW_COMBO_ROW (self->loader_row)));
}

/* Vanilla has no loader version: the row is not part of a choice then. */
static void
update_visibility (nuvashalauncherLoaderRow *self)
{
  /* The loader combo can still be emitting its selection notification while
   * a new request is being started. @current is the request's authoritative
   * loader, so use it instead of rereading the combo during that transition. */
  gboolean vanilla = self->current == nuvashalauncher_LOADER_VANILLA;

  gtk_widget_set_visible (GTK_WIDGET (self->version_row), !vanilla);
}

/* Both combos answer only when nothing else has locked them: a running
   game locks both, and the version row also locks itself while it is
   loading or has nothing to show. */
static void
update_sensitivity (nuvashalauncherLoaderRow *self)
{
  gtk_widget_set_sensitive (GTK_WIDGET (self->loader_row), !self->blocked);
  gtk_widget_set_sensitive (GTK_WIDGET (self->version_row),
                            !self->blocked && self->interactive);
}

/* Puts @labels behind the model of the version row and picks @position,
   which is GTK_INVALID_LIST_POSITION for nothing. */
static void
set_versions (nuvashalauncherLoaderRow *self, GPtrArray *versions, guint position)
{
  g_autoptr (GtkStringList) list = NULL;
  g_autofree const char **labels = NULL;

  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = g_steal_pointer (&versions);

  labels = g_new0 (const char *, self->versions->len + 1);

  for (guint i = 0; i < self->versions->len; i++)
    {
      const nuvashalauncherLoaderVersion *version = g_ptr_array_index (self->versions, i);

      /* What a user reads says which of these the loader does not call
         stable. */
      labels[i] = version->stable ? g_strdup (version->version)
                                   : g_strdup_printf (_("%s (unstable)"),
                                                      version->version);
    }

  list = gtk_string_list_new (labels);

  for (guint i = 0; i < self->versions->len; i++)
    g_free ((char *) labels[i]);

  adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row),
                           G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->version_row), position);
}

/* A screen stores a choice when it hears this; it is emitted whenever
   what get_loader()/get_version() would answer changes, including while
   a list loads (nothing to store then, which the guards on the screens
   know). */
static void
emit_changed (nuvashalauncherLoaderRow *self)
{
  g_signal_emit (self, nuvashalauncher_loader_row_signals[nuvashalauncher_LOADER_ROW_SIGNAL_CHANGED], 0);
}

static void
show_loading (nuvashalauncherLoaderRow *self)
{
  g_autoptr (GtkStringList) list = NULL;
  const char * const loading[] = { _("Loading…"), NULL };

  list = gtk_string_list_new (loading);

  self->ready = FALSE;
  self->interactive = FALSE;
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_loader_version_free);

  adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row),
                           G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->version_row), 0);

  /* The row does not answer while it does not know the versions. */
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), NULL);
  update_sensitivity (self);
  emit_changed (self);
}

/* @retry is FALSE when trying again would not help, which is the case
   for a loader that cannot say anything before a Minecraft version is
   picked. */
static void
show_error (nuvashalauncherLoaderRow *self, const char *message, gboolean retry)
{
  g_autoptr (GtkStringList) list = NULL;
  const char *nothing[] = { _("None"), NULL };

  self->ready = FALSE;
  /* The Retry button is a suffix of the row, so the row itself has to
     answer clicks for it to be reachable. */
  self->interactive = retry;
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_loader_version_free);

  list = gtk_string_list_new (nothing);
  adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->version_row),
                              GTK_INVALID_LIST_POSITION);

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), message);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), retry);
  update_sensitivity (self);
  emit_changed (self);
}

/* A loader that answered a real list that holds nothing for this
   Minecraft version: not a failure a Retry could fix, so the row says
   what is missing instead of offering one. */
static void
show_empty (nuvashalauncherLoaderRow *self)
{
  g_autoptr (GtkStringList) list = NULL;
  g_autofree char *message = NULL;
  const char *nothing[] = { _("None"), NULL };

  self->ready = FALSE;
  self->interactive = FALSE;
  g_clear_pointer (&self->versions, g_ptr_array_unref);
  self->versions = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_loader_version_free);

  list = gtk_string_list_new (nothing);
  adw_combo_row_set_model (ADW_COMBO_ROW (self->version_row), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->version_row),
                              GTK_INVALID_LIST_POSITION);

  message = g_strdup_printf (_("No compatible %s version found for Minecraft %s."),
                             nuvashalauncher_loader_display_name (self->current),
                             self->game_version != NULL ? self->game_version
                                                        : "?");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), message);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  update_sensitivity (self);
  emit_changed (self);
}

/* Fills the version row from the versions a loader published. */
static void
fill (nuvashalauncherLoaderRow *self, GPtrArray *published)
{
  gboolean unstable =
    self->settings != NULL && nuvashalauncher_settings_get_show_unstable_loaders (self->settings);
  guint position = GTK_INVALID_LIST_POSITION;
  g_autoptr (GPtrArray) shown = NULL;

  self->ready = FALSE;

  shown = nuvashalauncher_loaders_versions_for_display (published, unstable, self->wanted,
                                            &position);

  set_versions (self, g_steal_pointer (&shown), position);

  self->ready = TRUE;
  self->interactive = TRUE;
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);

  /* Nothing to pick is not the same as something to pick: the row says
     so instead of answering with a version. */
  if (self->versions->len == 0)
    {
      g_autofree char *message = g_strdup_printf (
        _("No compatible %s version found for Minecraft %s."),
        nuvashalauncher_loader_display_name (self->current),
        self->game_version != NULL ? self->game_version : "?");

      adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), message);
    }
  else
    adw_action_row_set_subtitle (ADW_ACTION_ROW (self->version_row), NULL);

  update_sensitivity (self);

  /* A version is in the list now, so the choice is settled: a screen
     stores it or lets the Create button answer. */
  if (position != GTK_INVALID_LIST_POSITION)
    emit_changed (self);
}

/* ---------------------------------------------------------- *
 * Fetching
 * ---------------------------------------------------------- */

/* One load of the version list: the row, which can be disposed while the
   answer is on its way, and the handle that answer is meant to release. */
typedef struct
{
  nuvashalauncherLoaderRow *row;
  nuvashalauncherTaskScope *operation;
} VersionsAsk;

static void
versions_ask_free (VersionsAsk *ask)
{
  nuvashalauncherLoaderRow *row = ask->row;

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
versions_loaded (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                 gpointer user_data)
{
  VersionsAsk *ask = user_data;
  nuvashalauncherLoaderRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) versions = NULL;

  /* Ignore results from an operation replaced by a newer load. */
  if (self != NULL && self->operation == ask->operation)
    {
      versions = nuvashalauncher_loaders_versions_finish (result, &error);

      if (versions != NULL)
        fill (self, versions);
      else if (error != NULL &&
               g_error_matches (error, nuvashalauncher_LOADERS_ERROR,
                                 nuvashalauncher_LOADERS_ERROR_NO_VERSIONS))
        show_empty (self);
      else
        /* A failure that carries no error is still a failure: say what
           happened in words rather than reading a message that is not
           there. */
        {
          show_error (self, error != NULL ? nuvashalauncher_error_message (error) : NO_VERSION_MESSAGE,
                      TRUE);
          gtk_widget_set_tooltip_text (GTK_WIDGET (self->version_row),
                                       error != NULL ? nuvashalauncher_error_message (error) : NULL);
        }
    }

  versions_ask_free (ask);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherLoaderRow *self = nuvashalauncher_LOADER_ROW (user_data);
  g_autofree char *wanted = NULL;
  g_autofree char *game_version = NULL;

  if (self->settings == NULL)
    return;

  wanted = g_strdup (self->wanted);
  game_version = g_strdup (self->game_version);
  nuvashalauncher_loader_row_load (self, self->settings, selected_loader (self), wanted,
                       game_version);
}

/* Starts the fetch for @loader and remembers what was wanted. The
   caller has already set the game version and the settings. */
static void
start_versions (nuvashalauncherLoaderRow *self, nuvashalauncherLoader loader, const char *wanted)
{
  const nuvashalauncherLoader *loaders = nuvashalauncher_loaders_supported ();

  /* What the row is showing is this loader now, so the change the combo
     below is about to report is not read back as a user choice. */
  self->current = loader;

  g_free (self->wanted);
  self->wanted = g_strdup (wanted);

  /* The combo shows what was asked for, whether nuvashalauncher knows it or not, so
     a screen that stores the choice stores what the user saw. */
  for (guint i = 0; i < nuvashalauncher_loaders_supported_count (); i++)
    {
      if (loaders[i] == loader)
        adw_combo_row_set_selected (ADW_COMBO_ROW (self->loader_row), i);
    }

  update_visibility (self);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  if (loader == nuvashalauncher_LOADER_VANILLA)
    {
      self->ready = FALSE;
      self->interactive = FALSE;
      g_clear_pointer (&self->versions, g_ptr_array_unref);
      self->versions = g_ptr_array_new_with_free_func (
        (GDestroyNotify) nuvashalauncher_loader_version_free);
      update_sensitivity (self);
      emit_changed (self);
      return;
    }

  show_loading (self);

  /* A loader needs a Minecraft version to say which of its versions run
     it; without one there is nothing to ask for. */
  if (self->game_version == NULL || *self->game_version == '\0')
    {
      show_error (self, _("Pick a Minecraft version first"), FALSE);
      return;
    }

  {
    VersionsAsk *ask = g_new0 (VersionsAsk, 1);

    ask->row = g_object_ref (self);
    ask->operation = nuvashalauncher_loaders_versions_async (loader, self->game_version,
                                                 versions_loaded, ask);
    nuvashalauncher_task_scope_follow_cancellable (ask->operation, self->cancellable);
    self->operation = ask->operation;
  }
}

/* ---------------------------------------------------------- *
 * The public face
 * ---------------------------------------------------------- */

nuvashalauncherLoaderRow *
nuvashalauncher_loader_row_new (void)
{
  nuvashalauncherLoaderRow *self = g_object_new (nuvashalauncher_TYPE_LOADER_ROW, NULL);
  const nuvashalauncherLoader *loaders = nuvashalauncher_loaders_supported ();
  g_autofree const char **names = NULL;
  guint count = 0;

  count = nuvashalauncher_loaders_supported_count ();
  names = g_new0 (const char *, count + 1);

  for (guint i = 0; i < count; i++)
    names[i] = nuvashalauncher_loader_display_name (loaders[i]);

  self->loader_row = ADW_COMBO_ROW (g_object_ref_sink (adw_combo_row_new ()));
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->loader_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->loader_row),
                                  _("Loader"));
  {
    g_autoptr (GtkStringList) loaders_list = gtk_string_list_new (names);

    adw_combo_row_set_model (ADW_COMBO_ROW (self->loader_row),
                             G_LIST_MODEL (loaders_list));
  }
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->loader_row), 0);

  self->version_row = ADW_COMBO_ROW (g_object_ref_sink (adw_combo_row_new ()));
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self->version_row),
                                      FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->version_row),
                                  _("Loader Version"));
  self->retry = GTK_BUTTON (
    g_object_ref_sink (gtk_button_new_with_label (_("Retry"))));
  nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (self->retry), _("Retry"));
  gtk_widget_set_valign (GTK_WIDGET (self->retry), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self->version_row), GTK_WIDGET (self->retry));

  self->current = nuvashalauncher_LOADER_VANILLA;

  /* Every handler is connected with the object, so a row that a group
     still holds after this is gone cannot call back into it. */
  g_signal_connect_object (self->loader_row, "notify::selected",
                           G_CALLBACK (on_loader_selected), self, 0);
  g_signal_connect_object (self->version_row, "notify::selected",
                           G_CALLBACK (on_version_selected), self, 0);
  g_signal_connect_object (self->retry, "clicked",
                           G_CALLBACK (on_retry_clicked), self, 0);

  show_loading (self);
  update_visibility (self);

  return self;
}

AdwComboRow *
nuvashalauncher_loader_row_get_loader_row (nuvashalauncherLoaderRow *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LOADER_ROW (self), NULL);

  return self->loader_row;
}

AdwComboRow *
nuvashalauncher_loader_row_get_version_row (nuvashalauncherLoaderRow *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LOADER_ROW (self), NULL);

  return self->version_row;
}

void
nuvashalauncher_loader_row_load (nuvashalauncherLoaderRow *self,
                     nuvashalauncherSettings   *settings,
                     nuvashalauncherLoader      loader,
                     const char    *loader_version,
                     const char    *game_version)
{
  g_return_if_fail (nuvashalauncher_IS_LOADER_ROW (self));
  g_return_if_fail (settings == NULL || nuvashalauncher_IS_SETTINGS (settings));

  g_set_object (&self->settings, settings);

  g_free (self->game_version);
  self->game_version = g_strdup (game_version);

  start_versions (self, loader, loader_version);
}

void
nuvashalauncher_loader_row_set_blocked (nuvashalauncherLoaderRow *self, gboolean blocked)
{
  g_return_if_fail (nuvashalauncher_IS_LOADER_ROW (self));

  blocked = !!blocked;

  if (self->blocked == blocked)
    return;

  self->blocked = blocked;
  update_sensitivity (self);
}

nuvashalauncherLoader
nuvashalauncher_loader_row_get_loader (nuvashalauncherLoaderRow *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_LOADER_ROW (self), nuvashalauncher_LOADER_VANILLA);

  return selected_loader (self);
}

const char *
nuvashalauncher_loader_row_get_version (nuvashalauncherLoaderRow *self)
{
  guint position;

  g_return_val_if_fail (nuvashalauncher_IS_LOADER_ROW (self), NULL);

  if (!self->ready)
    return NULL;

  position = adw_combo_row_get_selected (ADW_COMBO_ROW (self->version_row));

  if (position == GTK_INVALID_LIST_POSITION || position >= self->versions->len)
    return NULL;

  {
    const nuvashalauncherLoaderVersion *version = g_ptr_array_index (self->versions,
                                                        position);

    return version->version;
  }
}

/* ---------------------------------------------------------- *
 * The GObject
 * ---------------------------------------------------------- */

static void
nuvashalauncher_loader_row_dispose (GObject *object)
{
  nuvashalauncherLoaderRow *self = nuvashalauncher_LOADER_ROW (object);

  /* A load still in flight only holds a reference on this object;
     cancelling it and dropping the settings stops it from writing into a
     row that is on its way out. */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->settings);

  g_clear_object (&self->retry);
  g_clear_object (&self->version_row);
  g_clear_object (&self->loader_row);

  G_OBJECT_CLASS (nuvashalauncher_loader_row_parent_class)->dispose (object);
}

static void
nuvashalauncher_loader_row_finalize (GObject *object)
{
  nuvashalauncherLoaderRow *self = nuvashalauncher_LOADER_ROW (object);

  g_clear_pointer (&self->versions, g_ptr_array_unref);
  g_free (self->game_version);
  g_free (self->wanted);

  G_OBJECT_CLASS (nuvashalauncher_loader_row_parent_class)->finalize (object);
}

static void
nuvashalauncher_loader_row_class_init (nuvashalauncherLoaderRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_loader_row_dispose;
  object_class->finalize = nuvashalauncher_loader_row_finalize;

  nuvashalauncher_loader_row_signals[nuvashalauncher_LOADER_ROW_SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nuvashalauncher_loader_row_init (nuvashalauncherLoaderRow *self)
{
  /* Everything is built in nuvashalauncher_loader_row_new(), which needs the loaders
     nuvashalauncher installs. */
  (void) self;
}
