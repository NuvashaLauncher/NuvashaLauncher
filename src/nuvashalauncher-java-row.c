/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherJavaRow: Java picker with loading, retry, automatic, and custom
 * runtime entries. */

/* realpath() is provided by glibc; nuvashalauncher targets Linux. */
#define _GNU_SOURCE

#include "nuvashalauncher-java-row-private.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-i18n.h"

#include <stdlib.h>
#include <unistd.h>

void
nuvashalauncher_java_row_ask_free (JavaAsk *ask)
{
  NuvashaLauncherJavaRow *row = ask->row;

  /* The row's own pointer is a loan of this reference, so it is given
     back before the reference goes. */
  if (row != NULL && row->operation == ask->operation)
    row->operation = NULL;

  g_clear_object (&ask->row);

  if (ask->operation != NULL)
    nuvashalauncher_task_scope_unref (ask->operation);

  g_free (ask);
}

G_DEFINE_FINAL_TYPE (NuvashaLauncherJavaRow, nuvashalauncher_java_row, ADW_TYPE_COMBO_ROW)

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
put_model (NuvashaLauncherJavaRow *self, GPtrArray *names, guint position)
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
show_loading (NuvashaLauncherJavaRow *self)
{
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (names, g_strdup (_("Loading…")));

  self->ready = FALSE;
  self->add_index = GTK_INVALID_LIST_POSITION;
  put_model (self, g_steal_pointer (&names), 0);
  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->add_button), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);
}

void
nuvashalauncher_java_row_show_error (NuvashaLauncherJavaRow *self, const char *message)
{
  /* Reuse the normal model construction so the combo keeps its factory,
   * selection and Add Java Runtime entry exactly as it does after a
   * successful scan. */
  nuvashalauncher_java_row_fill (self);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), message);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), TRUE);
}

static char *
java_label (NuvashaLauncherJava *java)
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

void
nuvashalauncher_java_row_fill (NuvashaLauncherJavaRow *self)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  const NuvashaLauncherJava *fallback = NULL;
  g_autofree char *wanted_real = NULL;
  guint position = GTK_INVALID_LIST_POSITION;

  if (self->wanted != NULL && *self->wanted != '\0' &&
      !(g_file_test (self->wanted, G_FILE_TEST_IS_REGULAR) &&
        g_file_test (self->wanted, G_FILE_TEST_IS_EXECUTABLE)))
    {
      g_clear_pointer (&self->wanted, g_free);
      g_clear_pointer (&self->probed, nuvashalauncher_java_free);
    }

  wanted_real = self->wanted != NULL ? realpath (self->wanted, NULL) : NULL;

  /* The Java the automatic choice stands for: named in the first entry,
     and left out of the list below when it is one of the detected runtimes.
     If no exact runtime exists, still show the best detected runtime so the
     row always starts with a useful selection. */
  if (self->javas != NULL)
    fallback = nuvashalauncher_java_select (self->javas, NULL, self->required_major);

  if (fallback == NULL && self->javas != NULL &&
      (self->wanted == NULL || *self->wanted == '\0'))
    fallback = nuvashalauncher_java_best_compatible (self->javas, 0);

  if (fallback != NULL)
    {
      g_ptr_array_add (names, java_label ((NuvashaLauncherJava *) fallback));
      g_ptr_array_add (ids, NULL);
      position = 0;
    }

  for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
    {
      NuvashaLauncherJava *java = g_ptr_array_index (self->javas, i);
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

  self->add_index = GTK_INVALID_LIST_POSITION;
  if (ids->len > 0)
    {
      g_ptr_array_add (names, g_strdup (_("Add Java Runtime")));
      self->add_index = names->len - 1;
    }

  /* Keep one blank item when detection found nothing. AdwComboRow uses the
   * model to keep the row enabled, while the error is shown only in the
   * subtitle and the real recovery action is the suffix button. */
  if (names->len == 0)
    {
      g_ptr_array_add (names, g_strdup (""));
      position = 0;
    }


  g_clear_pointer (&self->ids, g_ptr_array_unref);
  self->ids = ids;
  self->automatic_selection = (self->wanted == NULL || *self->wanted == '\0');
  self->ready = TRUE;

  /* Keep a real item selected so the popup is usable even when no exact
     automatic runtime exists. */
  if (position == GTK_INVALID_LIST_POSITION && names->len > 0 && ids->len > 0)
    position = 0;

  /* Filled in below, once the model carries the entry this points at. */
  self->restore = position;
  put_model (self, names, position);

  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->add_button),
                          ids->len == 0);
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
          NuvashaLauncherJava *java = g_ptr_array_index (self->javas, i);
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
          ask->operation = nuvashalauncher_java_probe_async (
            self->wanted, nuvashalauncher_java_row_custom_loaded,
                                                 ask);
          nuvashalauncher_task_scope_follow_cancellable (ask->operation,
                                             self->cancellable);
          self->operation = ask->operation;
        }
    }
}

/* ---------------------------------------------------------- *
 * Async completion
 * ---------------------------------------------------------- */

/* ---------------------------------------------------------- *
 * Public API
 * ---------------------------------------------------------- */

NuvashaLauncherJavaRow *
nuvashalauncher_java_row_new (void)
{
  return g_object_new (nuvashalauncher_TYPE_JAVA_ROW, NULL);
}

void
nuvashalauncher_java_row_set_required_major (NuvashaLauncherJavaRow *self, int required_major)
{
  g_return_if_fail (nuvashalauncher_IS_JAVA_ROW (self));

  /* Filling the list again makes the combo row emit its selection, which
   * lands back here through the caller's handler. Doing nothing when the
   * requirement is the same is what keeps that from being an endless
   * recursion. */
  if (self->required_major == required_major)
    return;

  self->required_major = required_major;

  if (self->ready)
    nuvashalauncher_java_row_fill (self);
}

void
nuvashalauncher_java_row_load (NuvashaLauncherJavaRow *self, const char *wanted)
{
  g_return_if_fail (nuvashalauncher_IS_JAVA_ROW (self));

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_set_str (&self->wanted, wanted);
  self->probe_started = FALSE;
  nuvashalauncher_java_free (self->probed);
  self->probed = NULL;

  show_loading (self);
  {
    JavaAsk *ask = g_new0 (JavaAsk, 1);

    ask->row = g_object_ref (self);
    ask->operation = nuvashalauncher_java_detect_async (
      nuvashalauncher_java_row_detect_finished, ask);
    nuvashalauncher_task_scope_follow_cancellable (ask->operation, self->cancellable);
    self->operation = ask->operation;
  }
}

GPtrArray *
nuvashalauncher_java_row_get_detected (NuvashaLauncherJavaRow *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_JAVA_ROW (self), NULL);

  return self->javas;
}

gboolean
nuvashalauncher_java_row_get_ready (NuvashaLauncherJavaRow *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_JAVA_ROW (self), FALSE);

  return self->ready;
}

const char *
nuvashalauncher_java_row_get_selected_path (NuvashaLauncherJavaRow *self)
{
  guint position;

  g_return_val_if_fail (nuvashalauncher_IS_JAVA_ROW (self), NULL);

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
nuvashalauncher_java_row_select_path (NuvashaLauncherJavaRow *self, const char *path)
{
  g_return_if_fail (nuvashalauncher_IS_JAVA_ROW (self));

  if (!self->ready)
    return;

  if (path == NULL || *path == '\0')
    {
      guint automatic = self->ids->len > 0 && self->ids->pdata[0] == NULL
                          ? 0 : GTK_INVALID_LIST_POSITION;
      g_clear_pointer (&self->wanted, g_free);
      self->automatic_selection = automatic != GTK_INVALID_LIST_POSITION;
      adw_combo_row_set_selected (ADW_COMBO_ROW (self), automatic);
      return;
    }

  for (guint i = 0; i < self->ids->len; i++)
    {
      if (g_strcmp0 (self->ids->pdata[i], path) == 0)
        {
          g_set_str (&self->wanted, path);
          self->automatic_selection = FALSE;
          adw_combo_row_set_selected (ADW_COMBO_ROW (self), i);
          return;
        }
    }
}

void
nuvashalauncher_java_row_set_custom (NuvashaLauncherJavaRow *self,
                         const char *path,
                         NuvashaLauncherJava    *probed)
{
  g_return_if_fail (nuvashalauncher_IS_JAVA_ROW (self));

  g_set_str (&self->wanted, path);

  /* Reuse an entry that detection already reports. */
  for (guint i = 0; self->javas != NULL && i < self->javas->len; i++)
    {
      NuvashaLauncherJava *java = g_ptr_array_index (self->javas, i);
      g_autofree char *java_real = realpath (java->executable, NULL);
      g_autofree char *real = realpath (path, NULL);

      if (g_strcmp0 (java_real, real) == 0)
        {
          if (self->ready)
            {
              nuvashalauncher_java_row_select_path (self, java->executable);

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

          nuvashalauncher_java_free (probed);
          return;
        }
    }

  nuvashalauncher_java_free (self->probed);

  if (probed != NULL)
    self->probed = probed;
  else
    {
      self->probed = g_new0 (NuvashaLauncherJava, 1);
      self->probed->executable = g_strdup (path);
    }

  nuvashalauncher_java_row_fill (self);

  /* The probed entry is the last one that has a path: the entry that
     asks for another runtime sits after it and has none. */
  if (self->ready && self->ids->len > 0)
    adw_combo_row_set_selected (ADW_COMBO_ROW (self), self->ids->len - 1);
}

/* ---------------------------------------------------------- *
 * GObject
 * ---------------------------------------------------------- */

static void
nuvashalauncher_java_row_dispose (GObject *object)
{
  NuvashaLauncherJavaRow *self = nuvashalauncher_JAVA_ROW (object);

  self->disposed = TRUE;
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->retry = NULL;
  self->add_button = NULL;

  G_OBJECT_CLASS (nuvashalauncher_java_row_parent_class)->dispose (object);
}

static void
nuvashalauncher_java_row_finalize (GObject *object)
{
  NuvashaLauncherJavaRow *self = nuvashalauncher_JAVA_ROW (object);

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  g_clear_pointer (&self->names, g_ptr_array_unref);
  g_clear_pointer (&self->javas, g_ptr_array_unref);
  nuvashalauncher_java_free (self->probed);
  g_free (self->wanted);

  G_OBJECT_CLASS (nuvashalauncher_java_row_parent_class)->finalize (object);
}

static void
nuvashalauncher_java_row_class_init (NuvashaLauncherJavaRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_java_row_dispose;
  object_class->finalize = nuvashalauncher_java_row_finalize;
}

static void
nuvashalauncher_java_row_init (NuvashaLauncherJavaRow *self)
{
  /* A Java path is a file name and may hold an "&": it is plain text. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (self), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self), _("Java Runtime"));

   self->ready = FALSE;
  self->javas = g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_java_free);
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
   nuvashalauncher_ui_prepare_icon_button (GTK_WIDGET (self->retry), _("Retry"));
  gtk_widget_set_valign (GTK_WIDGET (self->retry), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->retry), FALSE);
  g_signal_connect_object (self->retry, "clicked",
                           G_CALLBACK (nuvashalauncher_java_row_on_retry_clicked),
                           self, 0);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self), GTK_WIDGET (self->retry));

  /* First, so the entry that asks for a runtime can put the selection
     back before whoever uses the row sees it. */
  g_signal_connect_object (self, "notify::selected",
                           G_CALLBACK (nuvashalauncher_java_row_on_picked), self, 0);

  self->add_button = GTK_BUTTON (gtk_button_new_with_label (_("Add Java Runtime")));
  gtk_widget_set_valign (GTK_WIDGET (self->add_button), GTK_ALIGN_CENTER);
  gtk_widget_set_visible (GTK_WIDGET (self->add_button), FALSE);
  g_signal_connect_object (self->add_button, "clicked",
                           G_CALLBACK (nuvashalauncher_java_row_on_add_clicked),
                           self, 0);
   adw_action_row_add_suffix (ADW_ACTION_ROW (self),
                              GTK_WIDGET (self->add_button));
}
