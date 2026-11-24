/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "nuvashalauncher-instance-page-private.h"
#include "nuvashalauncher-i18n.h"
#include <glib/gstdio.h>
#include <sys/stat.h>

/* The Stats page of an instance: activity history and instance storage/creation
 * metadata. Read-only, searchable via the shared header search bar. */

typedef struct {
  NuvashaLauncherInstance *instance;
  GtkLabel *time_played;
  GtkLabel *last_played;
  GtkLabel *total_launches;
  GtkLabel *total_crashes;
  GtkLabel *total_size;
  GtkLabel *created_on;
  GCancellable *size_cancel;
  gboolean alive;
} StatsState;

static char *
format_duration (guint64 seconds, const char *zero_text)
{
  guint64 minutes = seconds / 60;

  if (seconds == 0)
    return g_strdup (zero_text);
  if (minutes == 0)
    return g_strdup (_("Less than a minute"));
  if (minutes < 60)
    return g_strdup_printf (_("%llu min"), (unsigned long long) minutes);

  return g_strdup_printf (_("%llu h %llu min"),
                          (unsigned long long) (minutes / 60),
                          (unsigned long long) (minutes % 60));
}

static char *
format_timestamp (gint64 unix_seconds, gboolean include_time, const char *never_text)
{
  if (unix_seconds <= 0)
    return g_strdup (never_text);

  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (unix_seconds);
  if (dt == NULL)
    return g_strdup (never_text);

  if (include_time)
    {
      g_autoptr (GDateTime) now = g_date_time_new_now_local ();
      g_autofree char *time_str = g_date_time_format (dt, "%H:%M");

      if (now != NULL &&
          g_date_time_get_year (dt) == g_date_time_get_year (now) &&
          g_date_time_get_day_of_year (dt) == g_date_time_get_day_of_year (now))
        return g_strdup_printf (_("Today, %s"), time_str);

      return g_date_time_format (dt, "%Y-%m-%d %H:%M");
    }

  return g_date_time_format (dt, "%Y-%m-%d");
}

static gint64
directory_created_timestamp (const char *directory)
{
  g_autoptr (GFile) file = g_file_new_for_path (directory);
  g_autoptr (GFileInfo) info = g_file_query_info (
    file, G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
    G_FILE_QUERY_INFO_NONE, NULL, NULL);
  guint64 ts = 0;

  if (info == NULL)
    return 0;

  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_CREATED))
    ts = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED);
  if (ts == 0 && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
    ts = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);

  return (gint64) ts;
}

static void
set_uint_label (GtkLabel *label, guint value)
{
  g_autofree char *text = g_strdup_printf ("%u", value);

  gtk_label_set_label (label, text);
}

static void
refresh_stats (StatsState *state)
{
  guint64 play_seconds = 0;
  gint64 last_played = 0;
  gint64 created_at = 0;
  guint launches = 0;
  guint crashes = 0;
  g_autofree char *directory = NULL;
  g_autofree char *played_text = NULL;
  g_autofree char *last_played_text = NULL;
  g_autofree char *created_text = NULL;

  g_object_get (state->instance,
                "directory", &directory,
                "play-seconds", &play_seconds,
                "last-played-at", &last_played,
                "created-at", &created_at,
                "launch-count", &launches,
                "crash-count", &crashes,
                NULL);

  played_text = format_duration (play_seconds, _("Not played yet"));
  gtk_label_set_label (state->time_played, played_text);

  last_played_text = format_timestamp (last_played, TRUE, _("Never"));
  gtk_label_set_label (state->last_played, last_played_text);

  set_uint_label (state->total_launches, launches);
  set_uint_label (state->total_crashes, crashes);

  if (directory != NULL && created_at <= 0)
    created_at = directory_created_timestamp (directory);

  created_text = format_timestamp (created_at, FALSE, "—");
  gtk_label_set_label (state->created_on, created_text);
}

static guint64
directory_size_recursive (const char *path, GCancellable *cancellable)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;
  guint64 total = 0;

  if (g_cancellable_is_cancelled (cancellable))
    return 0;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL)
    return 0;

  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      GStatBuf st;

      if (g_cancellable_is_cancelled (cancellable))
        return total;

      g_autofree char *full = g_build_filename (path, entry, NULL);
      if (g_lstat (full, &st) != 0)
        continue;

      if (S_ISDIR (st.st_mode))
        total += directory_size_recursive (full, cancellable);
      else if (st.st_size > 0)
        total += (guint64) st.st_size;
    }

  return total;
}

static void
size_worker (GTask *task, gpointer source_object G_GNUC_UNUSED,
             gpointer task_data, GCancellable *cancellable)
{
  const char *directory = task_data;
  guint64 *size = g_new (guint64, 1);

  *size = directory_size_recursive (directory, cancellable);
  g_task_return_pointer (task, size, g_free);
}

static void
on_size_ready (GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer user_data)
{
  StatsState *state = user_data;
  g_autofree guint64 *size = g_task_propagate_pointer (G_TASK (result), NULL);

  if (!state->alive || size == NULL)
    return;

  g_autofree char *formatted = g_format_size (*size);
  gtk_label_set_label (state->total_size, formatted);
}

static void
start_size_scan (StatsState *state)
{
  g_autofree char *directory = NULL;

  if (state->size_cancel != NULL)
    {
      g_cancellable_cancel (state->size_cancel);
      g_clear_object (&state->size_cancel);
    }

  g_object_get (state->instance, "directory", &directory, NULL);
  if (directory == NULL)
    {
      gtk_label_set_label (state->total_size, "—");
      return;
    }

  gtk_label_set_label (state->total_size, _("Calculating…"));
  state->size_cancel = g_cancellable_new ();

  g_autoptr (GTask) task = g_task_new (NULL, state->size_cancel, on_size_ready, state);
  g_task_set_task_data (task, g_strdup (directory), g_free);
  g_task_run_in_thread (task, size_worker);
}

static void
on_stats_map (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  StatsState *state = user_data;

  refresh_stats (state);
  start_size_scan (state);
}

static void
on_stats_unmap (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  StatsState *state = user_data;

  if (state->size_cancel != NULL)
    {
      g_cancellable_cancel (state->size_cancel);
      g_clear_object (&state->size_cancel);
    }
}

static void
on_instance_stat_changed (GObject *object G_GNUC_UNUSED,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data)
{
  StatsState *state = user_data;

  if (state->alive)
    refresh_stats (state);
}

static void
stats_state_free (StatsState *state)
{
  state->alive = FALSE;
  if (state->size_cancel != NULL)
    {
      g_cancellable_cancel (state->size_cancel);
      g_clear_object (&state->size_cancel);
    }
  g_signal_handlers_disconnect_by_data (state->instance, state);
  g_object_unref (state->instance);
  g_free (state);
}

/* A row with its value on the right. The label is returned so the value
 * can change. */
static GtkLabel *
add_stat_row (AdwPreferencesGroup *group, gpointer search_group,
              const char *title, const char *value)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  GtkLabel *label = GTK_LABEL (gtk_label_new (value));

  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_label_set_selectable (label, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (label), "dim-label");
  gtk_widget_set_valign (GTK_WIDGET (label), GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (row, GTK_WIDGET (label));
  adw_preferences_group_add (group, GTK_WIDGET (row));
  nuvashalauncher_preferences_search_add_row (search_group, GTK_WIDGET (row));
  return label;
}

GtkWidget *
create_stats_page (NuvashaLauncherInstancePage *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  AdwPreferencesGroup *activity = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *instance_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  gpointer search = nuvashalauncher_preferences_search_attach (page);
  gpointer activity_search = nuvashalauncher_preferences_search_add_group (search, activity);
  gpointer instance_search = nuvashalauncher_preferences_search_add_group (search, instance_group);
  StatsState *state = g_new0 (StatsState, 1);

  gtk_widget_add_css_class (GTK_WIDGET (page), "nuvashalauncher-settings-page");
  gtk_widget_set_vexpand (GTK_WIDGET (page), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (activity), "nuvashalauncher-preferences-surface");
  gtk_widget_add_css_class (GTK_WIDGET (instance_group), "nuvashalauncher-preferences-surface");

  state->instance = g_object_ref (self->instance);
  state->alive = TRUE;

  adw_preferences_group_set_title (activity, _("Activity"));
  state->time_played = add_stat_row (activity, activity_search, _("Time played"), "");
  state->last_played = add_stat_row (activity, activity_search, _("Last played"), "");
  state->total_launches = add_stat_row (activity, activity_search, _("Total launches"), "0");
  state->total_crashes = add_stat_row (activity, activity_search, _("Total crashes"), "0");

  adw_preferences_group_set_title (instance_group, _("Instance"));
  state->total_size = add_stat_row (instance_group, instance_search, _("Total size"), "—");
  state->created_on = add_stat_row (instance_group, instance_search, _("Created on"), "—");

  refresh_stats (state);

  {
    static const char *props[] = {
      "notify::play-seconds",
      "notify::last-played-at",
      "notify::launch-count",
      "notify::crash-count",
    };

    for (guint i = 0; i < G_N_ELEMENTS (props); i++)
      g_signal_connect (self->instance, props[i],
                        G_CALLBACK (on_instance_stat_changed), state);
  }

  g_object_set_data_full (G_OBJECT (page), "nuvashalauncher-stats-state", state,
                          (GDestroyNotify) stats_state_free);
  g_signal_connect (page, "map", G_CALLBACK (on_stats_map), state);
  g_signal_connect (page, "unmap", G_CALLBACK (on_stats_unmap), state);

  adw_preferences_page_add (page, activity);
  adw_preferences_page_add (page, instance_group);

  return GTK_WIDGET (page);
}
