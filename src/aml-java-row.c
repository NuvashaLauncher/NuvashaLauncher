/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlJavaRow: pick a Java. Mirrors AmlVersionRow's state machine: the
 * row shows it is fetching, it failed (with how to retry), or it holds
 * the installations detection found plus any custom path the instance
 * carries. The first entry is the automatic choice: the runtime that
 * fits the game's Java requirement, named there instead of in the list
 * when one is found. */

/* realpath wants glibc's default feature set; AML is Linux-only. */
#define _GNU_SOURCE

#include "aml-java-row.h"
#include "aml-i18n.h"

#include <stdlib.h>
#include <unistd.h>

static void custom_loaded (GObject*, GAsyncResult*, gpointer);
static void on_picked (GObject*, GParamSpec*, gpointer);

struct _AmlJavaRow
{
  AdwComboRow parent_instance;

  /* Cancelled on reload and on dispose. */
  GCancellable *cancellable;

  /* The detected installations, GPtrArray<AmlJava*> owned by the row. */
  GPtrArray *javas;

  /* Model contents: index 0 is the automatic entry with a NULL path, one
   * per detected Java, one for a custom path detection never reported,
   * and a trailing one that asks for another runtime instead of naming
   * one. That last entry has no path, so the ids array is always one
   * entry shorter than the names array. */
  GPtrArray *ids;   /* strdupped paths; ids[0] is NULL */
  GPtrArray *names; /* the titles shown in the model */

  /* What index 0 is called when no runtime resolves it. */
  char *default_label;

  /* Where the add entry sits in the model, GTK_INVALID_LIST_POSITION
   * while the list does not hold one. */
  guint add_index;

  /* The entry to go back to when the add entry is picked: it names no
   * runtime, so it cannot stay selected. */
  guint restore;

  /* What the first entry stands for: the runtime the automatic choice
   * resolves to. Only the game's required version decides that, and that
   * runtime is named in the first entry and left out of the list below
   * so it is not shown twice. */
  int required_major;

  /* Selection the instance asked for; NULL is the automatic entry. */
  char *wanted;
  gboolean probe_started;

  /* The probed custom Java, even one that failed to report (then this
   * holds an empty AmlJava with just the path), NULL until then. */
  AmlJava *probed;

  gboolean ready;
  gboolean disposed;
  GtkButton *retry;

  /* The request the row is waiting for. Borrowed from the JavaAsk that
     started it: that one releases it when the answer arrives, and a
     newer request replaces it before an older answer can arrive. */
  AmlTaskScope *operation;
};

/* One Java request the row started: the row, which can be disposed while
   the answer is on its way, and the handle that answer is to release. */
typedef struct
{
  AmlJavaRow *row;
  AmlTaskScope *operation;
} JavaAsk;

static void
java_ask_free (JavaAsk *ask)
{
  AmlJavaRow *row = ask->row;

  /* The row's own pointer is a loan of this reference, so it is given
     back before the reference goes. */
  if (row != NULL && row->operation == ask->operation)
    row->operation = NULL;

  g_clear_object (&ask->row);

  if (ask->operation != NULL)
    aml_task_scope_unref (ask->operation);

  g_free (ask);
}

G_DEFINE_FINAL_TYPE (AmlJavaRow, aml_java_row, ADW_TYPE_COMBO_ROW)

/* ---------------------------------------------------------- *
 * Rebuilding the visible list
 * ---------------------------------------------------------- */

static void
put_model (AmlJavaRow *self, GPtrArray *names, guint position)
{
  g_autofree const char **strings = NULL;
  g_autoptr (GtkStringList) list = NULL;

  strings = g_new0 (const char *, names->len + 1);

  for (guint i = 0; i < names->len; i++)
    strings[i] = (char *) g_ptr_array_index (names, i);

  list = gtk_string_list_new (strings);
  g_clear_pointer (&self->names, g_ptr_array_unref);
  self->names = names;

  adw_combo_row_set_model (ADW_COMBO_ROW (self), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (self), position);
}

static void
show_loading (AmlJavaRow *self)
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
show_error (AmlJavaRow *self, const char *message)
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
java_label (AmlJava *java)
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
fill (AmlJavaRow *self)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  const AmlJava *fallback = NULL;
  g_autofree char *wanted_real =
    self->wanted != NULL ? realpath (self->wanted, NULL) : NULL;
  guint position = 0;

  /* The Java the automatic choice stands for: named in the first entry,
     and left out of the list below when it is one of the detected
     runtimes even so. Detection clears the list and fills it again, so
     the row is walked while it may still hold nothing. */
  if (self->javas != NULL && self->required_major > 0)
    fallback = aml_java_select (self->javas, NULL, self->required_major);

  if (fallback != NULL)
    g_ptr_array_add (names, java_label ((AmlJava *) fallback));
  else
    g_ptr_array_add (names, g_strdup (self->default_label));
  g_ptr_array_add (ids, NULL);

  for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
    {
      AmlJava *java = g_ptr_array_index (self->javas, i);
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
     added. It names no runtime, so it gets no id and the arrays end up
     one entry apart. */
  g_ptr_array_add (names, g_strdup (_("Add Java Runtime")));
  self->add_index = names->len - 1;

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  self->ids = ids;
  self->ready = TRUE;

  /* Filled in below, once the model carries the entry this points at. */
  self->restore = position;
  put_model (self, names, position);

  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);

  /* A wanted path detection never saw needs a probe. */
  if (self->wanted != NULL && *self->wanted != '\0' && !self->probe_started &&
      self->probed == NULL && position == 0)
    {
      g_autofree char *wanted_real = realpath (self->wanted, NULL);
      gboolean found = FALSE;

      for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
        {
          AmlJava *java = g_ptr_array_index (self->javas, i);
          g_autofree char *real = realpath (java->executable, NULL);

          if (g_strcmp0 (real != NULL ? real : java->executable,
                         wanted_real != NULL ? wanted_real : self->wanted) == 0)
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
          ask->operation = aml_java_probe_async (self->wanted, custom_loaded,
                                                 ask);
          aml_task_scope_follow_cancellable (ask->operation,
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
  AmlJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = aml_java_detect_finish (result, &error);

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
  AmlJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  AmlJava *custom = aml_java_probe_finish (result, &error);

  /* Disposing or reloading the row cancels a custom probe. Do not rebuild
   * a row whose state has already been torn down for that cancellation. */
  if (self == NULL || self->disposed || self->operation != ask->operation ||
      (error != NULL &&
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
    {
      aml_java_free (custom);
      java_ask_free (ask);
      return;
    }

  if (custom != NULL)
    {
      aml_java_free (self->probed);
      self->probed = custom;
    }
  else
    {
      /* Probing failed: the path is still a setting, just label it raw. */
      aml_java_free (self->probed);
      self->probed = g_new0 (AmlJava, 1);
      self->probed->executable = g_strdup (self->wanted);
    }

  fill (self);
  java_ask_free (ask);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlJavaRow *self = user_data;

  aml_java_row_load (self, self->wanted);
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
  AmlJavaRow *self = ask->row;
  g_autoptr (GError) error = NULL;
  AmlJava *probe = aml_java_probe_finish (result, &error);

  if (self == NULL || self->disposed || self->operation != ask->operation)
    {
      aml_java_free (probe);
      java_ask_free (ask);
      return;
    }

  if (probe == NULL)
    adw_action_row_set_subtitle (
      ADW_ACTION_ROW (self),
      error != NULL ? error->message
                    : _("the chosen file does not report a Java version"));
  else
    aml_java_row_set_custom (self, probe->executable,
                             g_steal_pointer (&probe));

  java_ask_free (ask);
}

static void
add_chosen (GObject *dialog, GAsyncResult *result, gpointer user_data)
{
  AmlJavaRow *self = user_data;
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
        ask->operation = aml_java_probe_async (path, add_probed, ask);
        aml_task_scope_follow_cancellable (ask->operation, self->cancellable);
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
  AmlJavaRow *self = user_data;
  GtkRoot *root;
  GtkFileDialog *dialog;
  guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (self));

  if (selected != self->add_index)
    {
      /* Anything else in the list names a runtime, and is where the ask
         goes back to. */
      if (selected < self->ids->len)
        self->restore = selected;

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

AmlJavaRow *
aml_java_row_new (void)
{
  return g_object_new (AML_TYPE_JAVA_ROW, NULL);
}

void
aml_java_row_set_required_major (AmlJavaRow *self, int required_major)
{
  g_return_if_fail (AML_IS_JAVA_ROW (self));

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
aml_java_row_load (AmlJavaRow *self, const char *wanted)
{
  g_return_if_fail (AML_IS_JAVA_ROW (self));

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_set_str (&self->wanted, wanted);
  self->probe_started = FALSE;
  aml_java_free (self->probed);
  self->probed = NULL;

  show_loading (self);
  {
    JavaAsk *ask = g_new0 (JavaAsk, 1);

    ask->row = g_object_ref (self);
    ask->operation = aml_java_detect_async (detect_finished, ask);
    aml_task_scope_follow_cancellable (ask->operation, self->cancellable);
    self->operation = ask->operation;
  }
}

GPtrArray *
aml_java_row_get_detected (AmlJavaRow *self)
{
  g_return_val_if_fail (AML_IS_JAVA_ROW (self), NULL);

  return self->javas;
}

gboolean
aml_java_row_get_ready (AmlJavaRow *self)
{
  g_return_val_if_fail (AML_IS_JAVA_ROW (self), FALSE);

  return self->ready;
}

const char *
aml_java_row_get_selected_path (AmlJavaRow *self)
{
  guint position;

  g_return_val_if_fail (AML_IS_JAVA_ROW (self), NULL);

  if (!self->ready)
    return NULL;

  position = adw_combo_row_get_selected (ADW_COMBO_ROW (self));

  /* Past the end of the ids is the entry that asks for another runtime:
     it names none, so there is nothing to hand back. */
  if (position == GTK_INVALID_LIST_POSITION || position >= self->ids->len)
    return NULL;

  /* Entry 0 carries a NULL path: it is the automatic choice. */
  if (self->ids->pdata[position] == NULL)
    return "";

  return self->ids->pdata[position];
}

void
aml_java_row_select_path (AmlJavaRow *self, const char *path)
{
  g_return_if_fail (AML_IS_JAVA_ROW (self));

  if (!self->ready)
    return;

  if (path == NULL || *path == '\0')
    {
      adw_combo_row_set_selected (ADW_COMBO_ROW (self), 0);
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
aml_java_row_set_custom (AmlJavaRow *self,
                         const char *path,
                         AmlJava    *probed)
{
  g_return_if_fail (AML_IS_JAVA_ROW (self));

  /* Reuse an entry that detection already reports. */
  for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
    {
      AmlJava *java = g_ptr_array_index (self->javas, i);
      g_autofree char *java_real = realpath (java->executable, NULL);
      g_autofree char *real = realpath (path, NULL);

      if (g_strcmp0 (java_real, real) == 0)
        {
          if (self->ready)
            {
              aml_java_row_select_path (self, java->executable);

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

          aml_java_free (probed);
          return;
        }
    }

  aml_java_free (self->probed);

  if (probed != NULL)
    self->probed = probed;
  else
    {
      self->probed = g_new0 (AmlJava, 1);
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
aml_java_row_dispose (GObject *object)
{
  AmlJavaRow *self = AML_JAVA_ROW (object);

  self->disposed = TRUE;
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->retry = NULL;

  G_OBJECT_CLASS (aml_java_row_parent_class)->dispose (object);
}

static void
aml_java_row_finalize (GObject *object)
{
  AmlJavaRow *self = AML_JAVA_ROW (object);

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  g_clear_pointer (&self->names, g_ptr_array_unref);
  g_clear_pointer (&self->javas, g_ptr_array_unref);
  aml_java_free (self->probed);
  g_free (self->wanted);
  g_free (self->default_label);

  G_OBJECT_CLASS (aml_java_row_parent_class)->finalize (object);
}

static void
aml_java_row_class_init (AmlJavaRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_java_row_dispose;
  object_class->finalize = aml_java_row_finalize;
}

static void
aml_java_row_init (AmlJavaRow *self)
{
  /* A Java path is a file name and may hold an "&": it is plain text. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self), _("Java Runtime"));

  self->ready = FALSE;
  self->javas = g_ptr_array_new_with_free_func ((GDestroyNotify) aml_java_free);
  self->ids = g_ptr_array_new_with_free_func (g_free);
  self->names = g_ptr_array_new_with_free_func (g_free);
  self->default_label = g_strdup (_("Automatic (closest compatible)"));
  self->required_major = 0;
  self->add_index = GTK_INVALID_LIST_POSITION;
  self->restore = 0;

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
