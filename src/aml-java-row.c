/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlJavaRow: pick a Java. Mirrors AmlVersionRow's state machine: the
 * row shows it is fetching, it failed (with how to retry), or it holds
 * the installations detection found plus any custom path the instance
 * carries. */

/* realpath wants glibc's default feature set; AML is Linux-only. */
#define _GNU_SOURCE

#include "aml-java-row.h"
#include "aml-i18n.h"

#include <stdlib.h>
#include <unistd.h>

static void custom_loaded (GObject*, GAsyncResult*, gpointer);

struct _AmlJavaRow
{
  AdwComboRow parent_instance;

  /* Cancelled on reload and on dispose. */
  GCancellable *cancellable;

  /* The detected installations, GPtrArray<AmlJava*> owned by the row. */
  GPtrArray *javas;

  /* Model contents: index 0 is the default entry with a NULL path, one
   * per detected Java, and a trailing one for the custom paths
   * detection never reported. */
  GPtrArray *ids;   /* strdupped paths; ids[0] is NULL */
  GPtrArray *names; /* the titles shown in the model */
  char *default_label;

  /* What the first entry stands for: the Java the default resolves to.
   * The global default path and the game's required version decide it;
   * that runtime is named in the first entry and left out of the list
   * below so it is not shown twice. */
  char *default_path;
  int required_major;

  /* Selection the instance asked for; /"/" is the default entry. */
  char *wanted;
  gboolean probe_started;

  /* The probed custom Java, even one that failed to report (then this
   * holds an empty AmlJava with just the path), NULL until then. */
  AmlJava *probed;

  gboolean ready;
  GtkWidget *retry;
};

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

  g_ptr_array_add (names, g_strdup ("Loading…"));

  self->ready = FALSE;
  put_model (self, g_steal_pointer (&names), 0);
  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);
  gtk_widget_set_visible (self->retry, FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);
}

static void
show_error (AmlJavaRow *self, const char *message)
{
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);

  self->ready = FALSE;
  put_model (self, g_steal_pointer (&names), GTK_INVALID_LIST_POSITION);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), message);
  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (self->retry, TRUE);
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

  /* The Java the default stands for: named in the first entry, and left
     out of the list below when it is one of the detected runtimes even
     so. */
  if (self->default_path != NULL || self->required_major > 0)
    fallback = aml_java_select (self->javas, NULL, self->default_path,
                                self->required_major);

  if (fallback != NULL)
    g_ptr_array_add (names, java_label ((AmlJava *) fallback));
  else
    g_ptr_array_add (names, g_strdup (self->default_label));
  g_ptr_array_add (ids, NULL);

  for (guint i = 0; i < self->javas->len; i++)
    {
      AmlJava *java = g_ptr_array_index (self->javas, i);
      g_autofree char *real = realpath (java->executable, NULL);
      gboolean is_wanted =
        wanted_real != NULL &&
        g_strcmp0 (real != NULL ? real : java->executable, wanted_real) == 0;

      /* The runtime the default resolves to is the first entry; it does
         not belong in the list again, unless the instance itself asked
         for it and so needs its row to be selectable. */
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

  g_clear_pointer (&self->ids, g_ptr_array_unref);
  self->ids = ids;
  self->ready = TRUE;
  put_model (self, names, position);

  gtk_widget_set_sensitive (GTK_WIDGET (self), TRUE);
  gtk_widget_set_visible (self->retry, FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self), NULL);

  /* A wanted path detection never saw needs a probe. */
  if (self->wanted != NULL && *self->wanted != '\0' && !self->probe_started &&
      self->probed == NULL && position == 0)
    {
      g_autofree char *wanted_real = realpath (self->wanted, NULL);
      gboolean found = FALSE;

      for (guint i = 0; i < self->javas->len; i++)
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
          self->probe_started = TRUE;
          aml_java_probe_async (self->wanted, NULL, custom_loaded,
                                g_object_ref (self));
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
  AmlJavaRow *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) javas = aml_java_detect_finish (result, &error);

  if (g_task_get_cancellable (G_TASK (result)) == self->cancellable)
    {
      if (javas != NULL)
        {
          g_clear_pointer (&self->javas, g_ptr_array_unref);
          self->javas = g_steal_pointer (&javas);
          fill (self);
        }
      else
        show_error (self, error->message);
    }

  g_object_unref (self);
}

static void
custom_loaded (GObject      *source G_GNUC_UNUSED,
               GAsyncResult *result,
               gpointer      user_data)
{
  AmlJavaRow *self = user_data;
  g_autoptr (GError) error = NULL;
  AmlJava *custom = aml_java_probe_finish (result, &error);

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
  g_object_unref (self);
}

static void
on_retry_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlJavaRow *self = user_data;

  aml_java_row_load (self, self->wanted);
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
aml_java_row_set_default_entry_label (AmlJavaRow *self,
                                      const char *label)
{
  g_return_if_fail (AML_IS_JAVA_ROW (self));

  g_set_str (&self->default_label, label);
}

void
aml_java_row_set_default_choice (AmlJavaRow *self,
                                 const char *global_path,
                                 int         required_major)
{
  g_return_if_fail (AML_IS_JAVA_ROW (self));

  /* Filling the list again makes the combo row emit its selection, which
   * lands back here through the caller's handler. Doing nothing when the
   * choice is the same is what keeps that from being an endless
   * recursion. */
  if (g_strcmp0 (self->default_path, global_path) == 0 &&
      self->required_major == required_major)
    return;

  g_set_str (&self->default_path, global_path);
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
  aml_java_detect_async (self->cancellable, detect_finished,
                         g_object_ref (self));
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

  if (position == GTK_INVALID_LIST_POSITION || position >= self->ids->len)
    return NULL;

  /* Entry 0 carries a NULL path: it is the global default. */
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

              /* The runtime the default resolves to is not listed again:
                 its entry is the first, "Default", one. */
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

  /* The probed entry is the last one that has a path: the "Add Java
     Runtime" entry, when it is on, sits after it and has none. */
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
  g_free (self->default_path);

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
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self),
                                 "Java Installation");

  self->ready = FALSE;
  self->javas = g_ptr_array_new_with_free_func ((GDestroyNotify) aml_java_free);
  self->ids = g_ptr_array_new_with_free_func (g_free);
  self->names = g_ptr_array_new_with_free_func (g_free);
  self->default_label = g_strdup ("Default");
  self->default_path = NULL;
  self->required_major = 0;

  self->retry = gtk_button_new_with_label (_("Retry"));
  gtk_widget_add_css_class (self->retry, "flat");
  gtk_widget_set_valign (self->retry, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->retry, FALSE);
  g_signal_connect (self->retry, "clicked", G_CALLBACK (on_retry_clicked),
                    self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (self), self->retry);
}
