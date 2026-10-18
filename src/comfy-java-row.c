/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyJavaRow: pick a Java. Mirrors ComfyVersionRow's state machine: the
 * row shows it is fetching, it failed (with how to retry), or it holds
 * the installations detection found plus any custom path the instance
 * carries. The automatic choice is displayed even when the detected runtime
 * does not satisfy the game's exact Java requirement. */

/* realpath wants glibc's default feature set; comfy is Linux-only. */
#define _GNU_SOURCE

#include "comfy-java-row.h"
#include "comfy-i18n.h"

#include <stdlib.h>
#include <unistd.h>

static void custom_loaded (GObject*, GAsyncResult*, gpointer);
static void on_picked (GObject*, GParamSpec*, gpointer);

struct _ComfyJavaRow
{
  AdwComboRow parent_instance;

  /* Cancelled on reload and on dispose. */
  GCancellable *cancellable;

  /* The detected installations, GPtrArray<ComfyJava*> owned by the row. */
  GPtrArray *javas;

  /* Model contents: the automatic runtime is index 0 with a NULL path;
   * the remaining entries are detected/custom runtimes and a trailing entry
   * that asks for another runtime instead of naming one. */
  GPtrArray *ids;   /* strdupped paths; the automatic entry has a NULL id */
  GPtrArray *names; /* the titles shown in the model */

  /* Where the add entry sits in the model, GTK_INVALID_LIST_POSITION
   * while the list does not hold one. */
  guint add_index;

  /* The entry to go back to when the add entry is picked: it names no
   * runtime, so it cannot stay selected. */
  guint restore;

  /* What the first entry stands for when an automatic runtime exists.
   * That runtime is named in the first entry and left out of the list below
   * so it is not shown twice. */
  int required_major;

  /* Selection the instance asked for; NULL is the automatic entry. */
  char *wanted;
  gboolean probe_started;

  /* The probed custom Java, even one that failed to report (then this
   * holds an empty ComfyJava with just the path), NULL until then. */
  ComfyJava *probed;

  gboolean ready;
  gboolean disposed;
  gboolean automatic_selection;
  gboolean rebuilding;
  GtkButton *retry;

  /* The request the row is waiting for. Borrowed from the JavaAsk that
     started it: that one releases it when the answer arrives, and a
     newer request replaces it before an older answer can arrive. */
  ComfyTaskScope *operation;
};

/* One Java request the row started: the row, which can be disposed while
   the answer is on its way, and the handle that answer is to release. */
typedef struct
{
  ComfyJavaRow *row;
  ComfyTaskScope *operation;
} JavaAsk;

static void
java_ask_free (JavaAsk *ask)
{
  ComfyJavaRow *row = ask->row;

  /* The row's own pointer is a loan of this reference, so it is given
     back before the reference goes. */
  if (row != NULL && row->operation == ask->operation)
    row->operation = NULL;

  g_clear_object (&ask->row);

  if (ask->operation != NULL)
    comfy_task_scope_unref (ask->operation);

  g_free (ask);
}

G_DEFINE_FINAL_TYPE (ComfyJavaRow, comfy_java_row, ADW_TYPE_COMBO_ROW)

static void
java_factory_setup (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                    GtkListItem *item,
                    gpointer user_data G_GNUC_UNUSED)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (NULL));

  gtk_label_set_xalign (label, 0.0);
  gtk_list_item_set_child (item, GTK_WIDGET (label));
}

static void
java_list_item_bind (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                     GtkListItem *item,
                     gpointer user_data G_GNUC_UNUSED)
{
  GtkWidget *child = gtk_list_item_get_child (item);
  gpointer value = gtk_list_item_get_item (item);

  if (GTK_IS_STRING_OBJECT (value))
    gtk_label_set_text (GTK_LABEL (child),
                        gtk_string_object_get_string (value));
}

/* ---------------------------------------------------------- *
 * Rebuilding the visible list
 * ---------------------------------------------------------- */

static void
put_model (ComfyJavaRow *self, GPtrArray *names, guint position)
{
  g_autofree const char **strings = NULL;
  g_autoptr (GtkStringList) list = NULL;

  strings = g_new0 (const char *, names->len + 1);

  for (guint i = 0; i < names->len; i++)
    strings[i] = (char *) g_ptr_array_index (names, i);

  list = gtk_string_list_new (strings);
  g_clear_pointer (&self->names, g_ptr_array_unref);
  self->names = names;

  self->rebuilding = TRUE;
  adw_combo_row_set_model (ADW_COMBO_ROW (self), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self), position);
  self->rebuilding = FALSE;
}

static void
show_loading (ComfyJavaRow *self)
{
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (names, g_strdup (_("Loading…")));

  self->ready = FALSE;
  self->add_index = GTK_INVALID_LIST_POSITION;
  put_model (self, g_steal_pointer (&names), 0);
  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);
}

static void
show_error (ComfyJavaRow *self, const char *message)
{
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);

  self->ready = FALSE;
  self->add_index = GTK_INVALID_LIST_POSITION;
  put_model (self, g_steal_pointer (&names), GTK_INVALID_LIST_POSITION);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), message);
  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), TRUE);
}

static char *
java_label (ComfyJava *java)
{
  if (java->major_version == 0)
    return g_strdup_printf (_("Custom: %s"), java->executable);

  /* Just what the runtime calls itself: "OpenJDK 25". The architecture,
     the headless flag and the folder it lives in are left out; the row's
     subtitle is where a runtime that cannot run the game is called
     out. */
  return g_strdup_printf ("%s %d",
                          java->vendor != NULL ? java->vendor : "Java",
                          java->major_version);
}

static void
fill (ComfyJavaRow *self)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  const ComfyJava *fallback = NULL;
  g_autofree char *wanted_real =
    self->wanted != NULL ? realpath (self->wanted, NULL) : NULL;
  guint position = GTK_INVALID_LIST_POSITION;

  /* The Java the automatic choice stands for: named in the first entry,
     and left out of the list below when it is one of the detected runtimes.
     If no exact runtime exists, still show the best detected runtime so the
     row always starts with a useful selection. */
  if (self->javas != NULL)
    fallback = comfy_java_select (self->javas, NULL, self->required_major);

  if (fallback == NULL && self->javas != NULL &&
      (self->wanted == NULL || *self->wanted == '\0'))
    fallback = comfy_java_best_compatible (self->javas, 0);

  if (fallback != NULL)
    {
      g_ptr_array_add (names, java_label ((ComfyJava *) fallback));
      g_ptr_array_add (ids, NULL);
      position = 0;
    }

  for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
    {
      ComfyJava *java = g_ptr_array_index (self->javas, i);
      g_autofree char *real = realpath (java->executable, NULL);
      gboolean is_wanted =
        wanted_real != NULL &&
        g_strcmp0 (real != NULL ? real : java->executable, wanted_real) == 0;

      /* The runtime the automatic choice resolves to is the first entry;
         it does not belong in the list again, unless the instance itself
         asked for it and so needs its row to be selectable. */
      if (java == fallback && !is_wanted)
        continue;

      g_ptr_array_add (names, java_label (java));
      g_ptr_array_add (ids, g_strdup (java->executable));

      if (is_wanted)
        position = names->len - 1;
    }

  if (self->probed != NULL)
    {
      g_ptr_array_add (names, java_label (self->probed));
      g_ptr_array_add (ids, g_strdup (self->probed->executable));

      if (self->wanted != NULL &&
          g_strcmp0 (self->probed->executable, self->wanted) == 0)
        position = names->len - 1;
    }

  /* The trailing entry is how a runtime detection did not report gets
     added. It names no runtime, so it gets no id. */
  g_ptr_array_add (names, g_strdup (_("Add Java Runtime")));
  self->add_index = names->len - 1;

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  self->ids = ids;
  self->automatic_selection = fallback != NULL &&
                              (self->wanted == NULL || *self->wanted == '\0');
  self->ready = TRUE;

  /* Keep a real item selected so the popup is usable even when no exact
     automatic runtime exists. */
  if (position == GTK_INVALID_LIST_POSITION && names->len > 0)
    position = 0;

  /* Filled in below, once the model carries the entry this points at. */
  self->restore = position;
  put_model (self, names, position);

  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);

  /* A wanted path detection never saw needs a probe. */
  if (self->wanted != NULL && *self->wanted != '\0' && !self->probe_started &&
       self->probed == NULL &&
       (position == GTK_INVALID_LIST_POSITION || position == 0))
    {
      g_autofree char *wanted_path_real = realpath (self->wanted, NULL);
      gboolean found = FALSE;

      for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
        {
          ComfyJava *java = g_ptr_array_index (self->javas, i);
          g_autofree char *real = realpath (java->executable, NULL);

          if (g_strcmp0 (real != NULL ? real : java->executable,
                          wanted_path_real != NULL ? wanted_path_real : self->wanted) == 0)
            {
              found = TRUE;
              break;
            }
        }

      if (!found)
        {
          JavaAsk *ask = g_new0 (JavaAsk, 1);

          self->probe_started = TRUE;
          ask->row = g_object_ref (self);
          ask->operation = comfy_java_probe_async (self->wanted, custom_loaded,
                                                 ask);
          comfy_task_scope_follow_cancellable (ask->operation,
                                             self->cancellable);
          self->operation = ask->operation;
        }
    }
}

/* ---------------------------------------------------------- *
 * Async completion
 * ---------------------------------------------------------- */

static void
detect_finished (GObject      *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  JavaAsk *ask = user_data;
  ComfyJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = comfy_java_detect_finish (result, &error);

  /* A detection of a row that has been reloaded since is about a machine
     state that is gone: it fills nothing. */
  if (self != NULL && self->operation == ask->operation && !self->disposed)
    {
      if (javas != NULL)
        {
          g_clear_pointer (&self->javas, g_ptr_array_unref);
          self->javas = g_steal_pointer (&javas);
          fill (self);
        }
      else if (error != NULL &&
               !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_error (self, error->message);
    }

  java_ask_free (ask);
}

static void
custom_loaded (GObject      *source G_GNUC_UNUSED,
               GAsyncResult *result,
               gpointer      user_data)
{
  JavaAsk *ask = user_data;
  ComfyJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  ComfyJava *custom = comfy_java_probe_finish (result, &error);

  /* Disposing or reloading the row cancels a custom probe. Do not rebuild
   * a row whose state has already been torn down for that cancellation. */
  if (self == NULL || self->disposed || self->operation != ask->operation ||
      (error != NULL &&
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
    {
      comfy_java_free (custom);
      java_ask_free (ask);
      return;
    }

  if (custom != NULL)
    {
      comfy_java_free (self->probed);
      self->probed = custom;
    }
  else
    {
      /* Probing failed: the path is still a setting, just label it raw. */
      comfy_java_free (self->probed);
      self->probed = g_new0 (ComfyJava, 1);
      self->probed->executable = g_strdup (self->wanted);
    }

  fill (self);
  java_ask_free (ask);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  ComfyJavaRow *self = comfy_JAVA_ROW (user_data);

  comfy_java_row_load (self, self->wanted);
}

/* ---------------------------------------------------------- *
 * Adding a runtime the scan did not report
 *
 * The last entry of the list asks for one instead of naming one, so it
 * carries no path and the caller's handler has nothing to store. Picking
 * it puts the row back to the entry it came from, so the list never
 * shows the ask as the answer, and opens the chooser. What comes back is
 * probed and appended like any other runtime, and selected: the selection
 * is what tells the caller which Java the user wants.
 * ---------------------------------------------------------- */

static void
add_probed (GObject      *source G_GNUC_UNUSED,
            GAsyncResult *result,
            gpointer      user_data)
{
  JavaAsk *ask = user_data;
  ComfyJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  ComfyJava *probe = comfy_java_probe_finish (result, &error);

  if (self == NULL || self->disposed || self->operation != ask->operation)
    {
      comfy_java_free (probe);
      java_ask_free (ask);
      return;
    }

  if (probe == NULL)
    {
      adw_action_row_set_subtitle (
        ADW_ACTION_ROW (self),
        error != NULL ? error->message
                      : _("the chosen file does not report a Java version"));
      java_ask_free (ask);
      return;
    }

  {
    const char *executable = probe->executable;
    comfy_java_row_set_custom (self, executable, g_steal_pointer (&probe));
  }

  java_ask_free (ask);
}

static void
add_chosen (GObject *dialog, GAsyncResult *result, gpointer user_data)
{
  ComfyJavaRow *self = comfy_JAVA_ROW (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file =
    gtk_file_dialog_open_finish (GTK_FILE_DIALOG (dialog), result, &error);

  g_object_unref (dialog);

  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED))
        adw_action_row_set_subtitle (ADW_ACTION_ROW (self), error->message);

      g_object_unref (self);
      return;
    }

  {
    g_autofree char *path = g_file_get_path (file);

    if (path == NULL)
      adw_action_row_set_subtitle (
        ADW_ACTION_ROW (self),
        _("the chosen file does not report a Java version"));
    else
      {
        JavaAsk *ask = g_new0 (JavaAsk, 1);

        ask->row = g_object_ref (self);
        ask->operation = comfy_java_probe_async (path, add_probed, ask);
        comfy_task_scope_follow_cancellable (ask->operation, self->cancellable);
        self->operation = ask->operation;
      }
  }

  g_object_unref (self);
}

static void
on_picked (GObject    *object G_GNUC_UNUSED,
           GParamSpec *pspec G_GNUC_UNUSED,
           gpointer    user_data)
{
  ComfyJavaRow *self = comfy_JAVA_ROW (user_data);
  GtkRoot *root;
  GtkFileDialog *dialog;
  guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (self));

  if (self->rebuilding)
    return;

  if (selected != self->add_index)
    {
      /* Anything else in the list names a runtime, and is where the ask
         goes back to. */
      if (selected < self->ids->len)
        {
          self->restore = selected;
          self->automatic_selection = FALSE;
        }

      return;
    }

  /* Connected before whoever uses the row, so the selection is already
     back on a runtime by the time its handler asks which one was
     picked. */
  adw_combo_row_set_selected (ADW_COMBO_ROW (self), self->restore);

  root = gtk_widget_get_root (GTK_WIDGET (self));
  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Choose Java executable"));
  gtk_file_dialog_open (dialog, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                        NULL, add_chosen, g_object_ref (self));
}

/* ---------------------------------------------------------- *
 * Public API
 * ---------------------------------------------------------- */

ComfyJavaRow *
comfy_java_row_new (void)
{
  return g_object_new (comfy_TYPE_JAVA_ROW, NULL);
}

void
comfy_java_row_set_required_major (ComfyJavaRow *self, int required_major)
{
  g_return_if_fail (comfy_IS_JAVA_ROW (self));

  /* Filling the list again makes the combo row emit its selection, which
   * lands back here through the caller's handler. Doing nothing when the
   * requirement is the same is what keeps that from being an endless
   * recursion. */
  if (self->required_major == required_major)
    return;

  self->required_major = required_major;

  if (self->ready)
    fill (self);
}

void
comfy_java_row_load (ComfyJavaRow *self, const char *wanted)
{
  g_return_if_fail (comfy_IS_JAVA_ROW (self));

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_set_str (&self->wanted, wanted);
  self->probe_started = FALSE;
  comfy_java_free (self->probed);
  self->probed = NULL;

  show_loading (self);
  {
    JavaAsk *ask = g_new0 (JavaAsk, 1);

    ask->row = g_object_ref (self);
    ask->operation = comfy_java_detect_async (detect_finished, ask);
    comfy_task_scope_follow_cancellable (ask->operation, self->cancellable);
    self->operation = ask->operation;
  }
}

GPtrArray *
comfy_java_row_get_detected (ComfyJavaRow *self)
{
  g_return_val_if_fail (comfy_IS_JAVA_ROW (self), NULL);

  return self->javas;
}

gboolean
comfy_java_row_get_ready (ComfyJavaRow *self)
{
  g_return_val_if_fail (comfy_IS_JAVA_ROW (self), FALSE);

  return self->ready;
}

const char *
comfy_java_row_get_selected_path (ComfyJavaRow *self)
{
  guint position;

  g_return_val_if_fail (comfy_IS_JAVA_ROW (self), NULL);

  if (!self->ready)
    return NULL;

  /* The visible first item is automatic, not an explicit instance choice. */
  if (self->automatic_selection)
    return "";

  position = adw_combo_row_get_selected (ADW_COMBO_ROW (self));

  /* Past the end of the ids is the entry that asks for another runtime:
     it names none, so there is nothing to hand back. */
  if (position == GTK_INVALID_LIST_POSITION || position >= self->ids->len)
    return NULL;

  /* A NULL id marks the automatic choice. */
  if (position < self->ids->len && self->ids->pdata[position] == NULL)
    return "";

  return self->ids->pdata[position];
}

void
comfy_java_row_select_path (ComfyJavaRow *self, const char *path)
{
  g_return_if_fail (comfy_IS_JAVA_ROW (self));

  if (!self->ready)
    return;

  if (path == NULL || *path == '\0')
    {
      guint automatic = self->ids->len > 0 && self->ids->pdata[0] == NULL
                          ? 0 : GTK_INVALID_LIST_POSITION;
      self->automatic_selection = automatic != GTK_INVALID_LIST_POSITION;
      adw_combo_row_set_selected (ADW_COMBO_ROW (self), automatic);
      return;
    }

  for (guint i = 0; i < self->ids->len; i++)
    {
      if (g_strcmp0 (self->ids->pdata[i], path) == 0)
        {
          adw_combo_row_set_selected (ADW_COMBO_ROW (self), i);
          return;
        }
    }
}

void
comfy_java_row_set_custom (ComfyJavaRow *self,
                         const char *path,
                         ComfyJava    *probed)
{
  g_return_if_fail (comfy_IS_JAVA_ROW (self));

  /* Reuse an entry that detection already reports. */
  for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
    {
      ComfyJava *java = g_ptr_array_index (self->javas, i);
      g_autofree char *java_real = realpath (java->executable, NULL);
      g_autofree char *real = realpath (path, NULL);

      if (g_strcmp0 (java_real, real) == 0)
        {
          if (self->ready)
            {
              comfy_java_row_select_path (self, java->executable);

              /* The runtime the automatic choice resolves to is not
                 listed again: its entry is the first one. */
              {
                gboolean listed = FALSE;

                for (guint j = 0; j < self->ids->len; j++)
                  if (g_strcmp0 (self->ids->pdata[j], java->executable) == 0)
                    listed = TRUE;

                if (!listed)
                  adw_combo_row_set_selected (ADW_COMBO_ROW (self), 0);
              }
            }

          comfy_java_free (probed);
          return;
        }
    }

  comfy_java_free (self->probed);

  if (probed != NULL)
    self->probed = probed;
  else
    {
      self->probed = g_new0 (ComfyJava, 1);
      self->probed->executable = g_strdup (path);
    }

  fill (self);

  /* The probed entry is the last one that has a path: the entry that
     asks for another runtime sits after it and has none. */
  if (self->ready)
    adw_combo_row_set_selected (ADW_COMBO_ROW (self), self->ids->len - 1);
}

/* ---------------------------------------------------------- *
 * GObject
 * ---------------------------------------------------------- */

static void
comfy_java_row_dispose (GObject *object)
{
  ComfyJavaRow *self = comfy_JAVA_ROW (object);

  self->disposed = TRUE;
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->retry = NULL;

  G_OBJECT_CLASS (comfy_java_row_parent_class)->dispose (object);
}

static void
comfy_java_row_finalize (GObject *object)
{
  ComfyJavaRow *self = comfy_JAVA_ROW (object);

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  g_clear_pointer (&self->names, g_ptr_array_unref);
  g_clear_pointer (&self->javas, g_ptr_array_unref);
  comfy_java_free (self->probed);
  g_free (self->wanted);

  G_OBJECT_CLASS (comfy_java_row_parent_class)->finalize (object);
}

static void
comfy_java_row_class_init (ComfyJavaRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = comfy_java_row_dispose;
  object_class->finalize = comfy_java_row_finalize;
}

static void
comfy_java_row_init (ComfyJavaRow *self)
{
  /* A Java path is a file name and may hold an "&": it is plain text. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self), _("Java Runtime"));

  self->ready = FALSE;
  self->javas = g_ptr_array_new_with_free_func ((GDestroyNotify) comfy_java_free);
  self->ids = g_ptr_array_new_with_free_func (g_free);
  self->names = g_ptr_array_new_with_free_func (g_free);
  self->required_major = 0;
  self->automatic_selection = FALSE;
  self->add_index = GTK_INVALID_LIST_POSITION;
  self->restore = 0;

  {
    GtkSignalListItemFactory *button_factory =
      GTK_SIGNAL_LIST_ITEM_FACTORY (gtk_signal_list_item_factory_new ());
    GtkSignalListItemFactory *popup_factory =
      GTK_SIGNAL_LIST_ITEM_FACTORY (gtk_signal_list_item_factory_new ());

    g_signal_connect (button_factory, "setup",
                      G_CALLBACK (java_factory_setup), self);
    g_signal_connect (button_factory, "bind",
                       G_CALLBACK (java_list_item_bind), self);
    g_signal_connect (popup_factory, "setup",
                      G_CALLBACK (java_factory_setup), self);
    g_signal_connect (popup_factory, "bind",
                       G_CALLBACK (java_list_item_bind), self);
    adw_combo_row_set_factory (ADW_COMBO_ROW (self),
                               GTK_LIST_ITEM_FACTORY (button_factory));
    adw_combo_row_set_list_factory (ADW_COMBO_ROW (self),
                                    GTK_LIST_ITEM_FACTORY (popup_factory));
    g_object_unref (button_factory);
    g_object_unref (popup_factory);
  }

   self->retry = GTK_BUTTON (gtk_button_new_with_label (_("Retry")));
  gtk_widget_add_css_class (GTK_WIDGET (self->retry), "flat");
  gtk_widget_set_valign (GTK_WIDGET (self->retry), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  g_signal_connect (self->retry, "clicked", G_CALLBACK (on_retry_clicked),
                    self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self), GTK_WIDGET (self->retry));

  /* First, so the entry that asks for a runtime can put the selection
     back before whoever uses the row sees it. */
  g_signal_connect (self, "notify::selected", G_CALLBACK (on_picked), self);
}
