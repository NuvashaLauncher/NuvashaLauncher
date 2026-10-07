/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The AML main window: header bar menus and the vertical list of
 * instances, shown exactly as AmlInstanceManager keeps them, with an
 * empty state behind it. The Play button of a row starts the game
 * through the launcher of that instance and follows it while it runs. */

#include "aml-main-window.h"
#include "aml-create-instance-dialog.h"
#include "aml-download-modpack-dialog.h"
#include "aml-instance-page.h"
#include "aml-java-row.h"
#include "aml-java.h"
#include "aml-launcher.h"
#include "aml-settings.h"
#include "aml-users-popover.h"

struct _AmlMainWindow
{
  AdwApplicationWindow parent_instance;

  AmlInstanceManager *manager;
  AmlSettings *settings;

  /* The accounts a launch plays as, and the popover in the header that
     shows them and the two ways of adding one. The window owns the store
     for as long as it is open, and every launcher of an instance is
     handed the same one. */
  AmlAccounts *accounts;
  AmlUsersPopover *users_popover;

  /* One launcher per instance, keyed by its id, so the row of an
     instance and its dialog always talk about the same launch. */
  GHashTable *launchers;

  /* The dialog of each instance that is open, keyed by its id. An
     AdwDialog is not a window, so the window list of the application
     cannot be asked about it; this is the bookkeeping that is. The
     dialogs are held by the window they were presented on, so the table
     holds borrowed pointers and only says which ones are open. */
  GHashTable *instance_dialogs;

  /* Switches between the empty state and the instance list. The list is
     what the window shows; an instance opens a dialog over it. */
  GtkWidget *stack;

  /* Says what happened without the user asking: a game that crashed,
     an operation that failed. */
  GtkWidget *toasts;
};

G_DEFINE_FINAL_TYPE (AmlMainWindow, aml_main_window, ADW_TYPE_APPLICATION_WINDOW)

static void
on_create_instance (GSimpleAction *action G_GNUC_UNUSED,
                    GVariant      *parameter G_GNUC_UNUSED,
                    gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  GtkWidget *dialog = GTK_WIDGET (
    aml_create_instance_dialog_new (self->manager, self->settings));

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_download_modpack (GSimpleAction *action G_GNUC_UNUSED,
                     GVariant      *parameter G_GNUC_UNUSED,
                     gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  GtkWidget *dialog = GTK_WIDGET (aml_download_modpack_dialog_new (
    self->manager));

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

/* Written the moment it is switched. When the file cannot be written
 * the setting keeps the value it had, the switch goes back to it and
 * the row says why: the dialog and the file never disagree. */
static void
on_show_snapshots_toggled (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);
  gboolean stored = aml_settings_get_show_snapshots (self->settings);
  g_autoptr (GError) error = NULL;

  if (active == stored)
    return;

  if (aml_settings_set_show_snapshots (self->settings, active, &error))
    return;

  adw_switch_row_set_active (row, stored);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), error->message);
}

/* Default-Java picker state shared by the two async hops below. */
typedef struct
{
  guint refs;
  AmlMainWindow *owner;
  AmlJavaRow *row;
} SettingsJava;

static void
settings_java_unref (gpointer data)
{
  SettingsJava *ctx = data;

  if (--ctx->refs > 0)
    return;

  g_clear_object (&ctx->owner);
  g_clear_object (&ctx->row);
  g_free (ctx);
}

static SettingsJava *
settings_java_ref (SettingsJava *ctx)
{
  ctx->refs++;
  return ctx;
}

static void
on_settings_java_selected (GObject *object,
                           GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
  SettingsJava *ctx = user_data;
  AmlJavaRow *row = AML_JAVA_ROW (object);
  const char *path = aml_java_row_get_selected_path (row);
  g_autoptr (GError) error = NULL;
  const char *current;
  const char *next;

  if (path == NULL)
    return; /* still loading or failed: nothing valid to store */

  current = aml_settings_get_default_java_path (ctx->owner->settings);
  next = (path[0] != '\0') ? path : NULL;

  if (g_strcmp0 (current, next) == 0)
    return;

  if (aml_settings_set_default_java_path (ctx->owner->settings, next, &error))
    {
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), NULL);
      return;
    }

  aml_java_row_select_path (row, current);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), error->message);
}

static void
on_settings_custom_probed (GObject *source G_GNUC_UNUSED,
                           GAsyncResult *result,
                           gpointer user_data)
{
  SettingsJava *ctx = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (AmlJava) probe = aml_java_probe_finish (result, &error);

  if (probe == NULL)
    {
      adw_action_row_set_subtitle (
        ADW_ACTION_ROW (ctx->row),
        error != NULL ? error->message
                      : "the chosen file does not report a version");
    }
  else if (aml_settings_set_default_java_path (ctx->owner->settings,
                                               probe->executable, &error))
    {
      adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->row), NULL);
      aml_java_row_set_custom (ctx->row, probe->executable,
                               g_steal_pointer (&probe));
      /* set_custom() fires notify::selected; the handler persists the
       * same path and skips as it compares against the new one. */
    }
  else
    {
      adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->row),
                                   error->message);
    }

  settings_java_unref (ctx);
}

static void
on_settings_custom_chosen (GObject *dialog, GAsyncResult *result,
                           gpointer user_data)
{
  SettingsJava *ctx = user_data;
  g_autoptr (GError) error = NULL;
  GFile *file =
    gtk_file_dialog_open_finish (GTK_FILE_DIALOG (dialog), result, &error);

  g_object_unref (dialog);

  if (file == NULL)
    {
      if (error != NULL &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_DISMISSED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR,
                            GTK_DIALOG_ERROR_CANCELLED))
        {
          adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->row),
                                       error->message);
        }

      settings_java_unref (ctx);
      return;
    }

  g_autofree char *path = g_file_get_path (file);

  g_object_unref (file);

  aml_java_probe_async (path, NULL, on_settings_custom_probed,
                        settings_java_ref (ctx));
  settings_java_unref (ctx);
}

static void
on_settings_custom_clicked (GtkButton *button G_GNUC_UNUSED,
                            gpointer user_data)
{
  SettingsJava *ctx = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, "Choose Java executable");
  gtk_file_dialog_open (dialog, GTK_WINDOW (ctx->owner), NULL,
                        on_settings_custom_chosen,
                        settings_java_ref (ctx));
}

/* Written the moment the value changes. A spin row can produce a value
 * for every click of its arrows, so the number that is already stored is
 * skipped rather than written over and over. */
static void
on_log_limit_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSpinRow *row = ADW_SPIN_ROW (object);
  guint limit = (guint) adw_spin_row_get_value (row);

  if (limit == aml_settings_get_log_line_limit (self->settings))
    return;

  /* A limit that cannot be written leaves the row showing the number the
     settings file holds. */
  if (!aml_settings_set_log_line_limit (self->settings, limit, NULL))
    adw_spin_row_set_value (
      row, aml_settings_get_log_line_limit (self->settings));
}

/* Same contract as the snapshots switch: written at once, and put back
   what the file says when the file says no. */
static void
on_show_unstable_loaders_toggled (GObject *object,
                                  GParamSpec *pspec G_GNUC_UNUSED,
                                  gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == aml_settings_get_show_unstable_loaders (self->settings))
    return;

  if (!aml_settings_set_show_unstable_loaders (self->settings, active, NULL))
    adw_switch_row_set_active (
      row, aml_settings_get_show_unstable_loaders (self->settings));
}

static void
on_open_logs_after_crash_toggled (GObject *object,
                                  GParamSpec *pspec G_GNUC_UNUSED,
                                  gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwSwitchRow *row = ADW_SWITCH_ROW (object);
  gboolean active = adw_switch_row_get_active (row);

  if (active == aml_settings_get_open_logs_after_crash (self->settings))
    return;

  if (!aml_settings_set_open_logs_after_crash (self->settings, active, NULL))
    adw_switch_row_set_active (
      row, aml_settings_get_open_logs_after_crash (self->settings));
}

static void
on_settings (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  AdwDialog *dialog = adw_preferences_dialog_new ();
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *versions_group = adw_preferences_group_new ();
  GtkWidget *java_group = adw_preferences_group_new ();
  GtkWidget *logs_group = adw_preferences_group_new ();
  GtkWidget *row = adw_switch_row_new ();
  GtkWidget *java_row = GTK_WIDGET (aml_java_row_new ());
  GtkWidget *custom_row = adw_action_row_new ();
  GtkWidget *custom_button = gtk_button_new_with_label ("Choose…");
  GtkWidget *limit_row = NULL;
  GtkWidget *unstable_row = adw_switch_row_new ();
  GtkWidget *crash_row = adw_switch_row_new ();
  SettingsJava *ctx = g_new0 (SettingsJava, 1);

  /* Every one of these rows can show a path or an error message from
     GIO, which is plain text and may hold an "&". */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (unstable_row),
                                      FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (crash_row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (custom_row), FALSE);

  ctx->refs = 1;
  ctx->owner = g_object_ref (self);
  ctx->row = AML_JAVA_ROW (java_row);
  g_object_ref (ctx->row);

  adw_dialog_set_title (dialog, "Settings");
  adw_preferences_dialog_set_search_enabled (ADW_PREFERENCES_DIALOG (dialog),
                                             FALSE);
  g_object_set_data_full (G_OBJECT (dialog), "settings-java", ctx,
                          settings_java_unref);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (versions_group),
                                   "Versions");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Show Snapshots");
  adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                             aml_settings_get_show_snapshots (self->settings));

  /* Connected after the value was set, so opening the dialog never
   * writes anything by itself. */
  g_signal_connect (row, "notify::active",
                    G_CALLBACK (on_show_snapshots_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (versions_group), row);

  /* The loader versions live under the same choice: both say what the
     version lists hold. */
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (unstable_row),
                                 "Show unstable Mod Loader versions");
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (unstable_row),
    aml_settings_get_show_unstable_loaders (self->settings));
  g_signal_connect (unstable_row, "notify::active",
                    G_CALLBACK (on_show_unstable_loaders_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (versions_group),
                             unstable_row);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (java_group),
                                   "Java");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (java_row),
                                 "Default Java");

  g_signal_connect (java_row, "notify::selected",
                    G_CALLBACK (on_settings_java_selected), ctx);

  aml_java_row_set_default_entry_label (AML_JAVA_ROW (java_row),
                                        "Automatic (closest compatible)");
  aml_java_row_load (AML_JAVA_ROW (java_row),
                     aml_settings_get_default_java_path (self->settings));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (custom_row),
                                 "Custom Java…");
  gtk_widget_set_valign (custom_button, GTK_ALIGN_CENTER);
  g_signal_connect (custom_button, "clicked",
                    G_CALLBACK (on_settings_custom_clicked),
                    settings_java_ref (ctx));
  adw_action_row_add_suffix (ADW_ACTION_ROW (custom_row), custom_button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (custom_row),
                                         custom_button);

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (java_group), java_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (java_group), custom_row);

  /* Logs */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (logs_group), "Logs");

  limit_row = adw_spin_row_new (
    gtk_adjustment_new (aml_settings_get_log_line_limit (self->settings), 100,
                        100000, 500, 5000, 0),
    0, 0);
  adw_spin_row_set_digits (ADW_SPIN_ROW (limit_row), 0);
  adw_spin_row_set_numeric (ADW_SPIN_ROW (limit_row), TRUE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (limit_row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (limit_row),
                                 "Line Limit");
  g_signal_connect (limit_row, "notify::value",
                    G_CALLBACK (on_log_limit_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (logs_group), limit_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (crash_row),
                                 "Open Log Folder After Crash");
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (crash_row),
    aml_settings_get_open_logs_after_crash (self->settings));
  g_signal_connect (crash_row, "notify::active",
                    G_CALLBACK (on_open_logs_after_crash_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (logs_group), crash_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (versions_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (java_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (logs_group));
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog),
                              ADW_PREFERENCES_PAGE (page));

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_about (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  AdwDialog *dialog = adw_about_dialog_new ();
  const char *app_ids[] = { "io.github.Flia7.AML", NULL };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), "AML");
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "Flia");

  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog),
                                "https://github.com/Flia7");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog),
                                  "Copyright (C) 2026 Flia");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog),
                                     GTK_LICENSE_GPL_3_0_ONLY);
  adw_about_dialog_add_credit_section (ADW_ABOUT_DIALOG (dialog),
                                       "Application ID", app_ids);
  adw_dialog_set_title (dialog, "About AML");

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static const GActionEntry window_entries[] = {
  { .name = "create-instance", .activate = on_create_instance },
  { .name = "download-modpack", .activate = on_download_modpack },
  { .name = "settings", .activate = on_settings },
  { .name = "about", .activate = on_about },
};

static GMenu *
create_add_menu (void)
{
  GMenu *menu = g_menu_new ();

  g_menu_append (menu, "Create Instance", "win.create-instance");
  g_menu_append (menu, "Download Modpack", "win.download-modpack");

  return menu;
}

static GMenu *
create_app_menu (void)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) general = g_menu_new ();
  g_autoptr (GMenu) rest = g_menu_new ();

  g_menu_append (general, "Settings", "win.settings");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (general));

  g_menu_append (rest, "About AML", "win.about");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (rest));

  return g_steal_pointer (&menu);
}

/* GtkPopoverMenu builds its item widgets itself and ignores any
 * "tooltip-text" item attribute, so the tooltip of an entry is attached
 * to the item widget after the popover was built. */
static void
set_menu_item_tooltip (GtkWidget  *root,
                       const char *label_text,
                       const char *tooltip)
{
  for (GtkWidget *child = gtk_widget_get_first_child (root);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      if (GTK_IS_LABEL (child) &&
          g_strcmp0 (gtk_label_get_text (GTK_LABEL (child)), label_text) == 0)
        {
          /* The label sits directly inside the menu item. */
          GtkWidget *item = gtk_widget_get_parent (child);

          if (item != NULL)
            gtk_widget_set_tooltip_text (item, tooltip);

          return;
        }

      set_menu_item_tooltip (child, label_text, tooltip);
    }
}

/* What a launch ends with is said out loud: a crash, a launch that never
   got to the game, and an account that has to sign in again. Doing
   nothing visible, which is what the window used to do for all but a
   crash, looks exactly like a Play button that does not work. */
static void
on_window_launcher_state (AmlLauncher *launcher G_GNUC_UNUSED,
                          AmlLaunchState state,
                          const char *detail, gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwToast *toast = NULL;
  g_autofree char *message = NULL;
  const char *what = NULL;

  switch (state)
    {
    case AML_LAUNCH_CRASHED:
      what = "Minecraft crashed";
      break;

    case AML_LAUNCH_FAILED:
      what = "Minecraft could not start";
      break;

    case AML_LAUNCH_NEEDS_SIGN_IN:
      what = "Minecraft needs a sign-in";
      break;

    default:
      return;
    }

  if (detail != NULL && *detail != '\0')
    message = g_strdup_printf ("%s: %s", what, detail);
  else
    message = g_strdup (what);

  toast = adw_toast_new (message);
  adw_toast_set_use_markup (toast, FALSE);
  adw_toast_set_timeout (toast, 8);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toasts), toast);
}

/* The launcher of one instance, made when it is first asked for. It is
 * the one launcher the row and the page of that instance share, which
 * is what keeps a game from being started twice. */
static AmlLauncher *
window_launcher (AmlMainWindow *self, const char *instance_id)
{
  AmlLauncher *launcher = NULL;
  AmlInstance *instance = NULL;

  if (instance_id == NULL)
    return NULL;

  launcher = g_hash_table_lookup (self->launchers, instance_id);

  if (launcher != NULL)
    return launcher;

  instance = aml_instance_manager_get_by_id (self->manager, instance_id);

  if (instance == NULL)
    return NULL;

  launcher = aml_launcher_new (instance, self->settings, self->accounts);

  /* g_signal_connect_object drops the connection when either side goes
     away, so the window outliving a launcher is not a dangling
     pointer. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_window_launcher_state), self, 0);

  g_hash_table_insert (self->launchers, g_strdup (instance_id), launcher);

  return launcher;
}

/* ---------------------------------------------------------- *
 * The dialogs of the instances
 *
 * An instance opens as an AdwDialog over the launcher, exactly like the
 * Settings dialog: it is not resizable, it cannot be moved away from the
 * window, and the page's own header bar becomes the header of the
 * dialog. That is why the instance page is not put in a window of its
 * own.
 * ---------------------------------------------------------- */

/* The dialog @id is open in, or NULL. The id travels with the dialog, so
   the table of open dialogs is all the bookkeeping there is: a dialog
   that is closed is no longer in it, and the one that holds the id is
   the one to present again. */
static AdwDialog *
dialog_of_instance (AmlMainWindow *self, const char *id)
{
  if (id == NULL)
    return NULL;

  return g_hash_table_lookup (self->instance_dialogs, id);
}

/* A dialog that was closed is not open any more. AdwDialog emits this
   while it is still alive, so the borrowed pointer leaves the table
   before the dialog is freed. */
static void
on_instance_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  AmlMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (dialog), "aml-instance-id");

  if (id != NULL)
    g_hash_table_remove (self->instance_dialogs, id);
}

/* The dialog of an instance says the name of the instance, and says it
   again when the name changes: a dialog left saying the old name is one
   the user cannot match to anything. */
static void
on_instance_renamed (AmlInstance *instance, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  AmlMainWindow *self = user_data;
  const char *id = aml_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  char *name = NULL;

  if (id == NULL)
    return;

  dialog = dialog_of_instance (self, id);

  if (dialog == NULL)
    return;

  g_object_get (instance, "name", &name, NULL);
  adw_dialog_set_title (dialog, name != NULL ? name : "");
  g_free (name);
}

/* Opens @instance: the dialog it already has, brought forward, or a new
 * one over the launcher. The page's header bar carries the view switcher
 * and, because it is inside an AdwDialog, the close button of the
 * dialog. */
static void
open_instance_dialog (AmlMainWindow *self, AmlInstance *instance)
{
  const char *id = aml_instance_get_id (instance);
  AdwDialog *dialog = NULL;
  GtkWidget *page = NULL;
  char *name = NULL;

  if (id == NULL)
    return;

  dialog = dialog_of_instance (self, id);

  if (dialog != NULL)
    {
      adw_dialog_present (dialog, GTK_WIDGET (self));
      return;
    }

  page = GTK_WIDGET (
    aml_instance_page_new (self->manager, instance, self->settings,
                           window_launcher (self, id)));

  g_object_get (instance, "name", &name, NULL);

  dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, name != NULL ? name : "");
  /* The size the separate window used to have, so the lists inside have
     the room they had. */
  adw_dialog_set_content_width (dialog, 900);
  adw_dialog_set_content_height (dialog, 620);
  adw_dialog_set_child (dialog, page);
  g_free (name);

  /* The id travels with the dialog, which is what makes it findable
     again and closable when its instance is deleted. */
  g_object_set_data_full (G_OBJECT (dialog), "aml-instance-id",
                          g_strdup (id), g_free);
  g_hash_table_insert (self->instance_dialogs, g_strdup (id), dialog);
  /* Weak, so that a dialog that outlives the window cannot call back
     into a window that is gone. */
  g_signal_connect_object (dialog, "closed",
                           G_CALLBACK (on_instance_dialog_closed), self, 0);

  /* The connection goes with the dialog: a page that goes away first
     leaves nothing to call back into. */
  g_signal_connect_object (instance, "notify::name",
                           G_CALLBACK (on_instance_renamed), self, 0);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* Closes the dialog of an instance that is no longer in the list, which
 * is what deleting one does. A game that is still running keeps running:
 * its launcher is held by the table, not by the dialog. */
static void
close_dialogs_of_removed_instances (AmlMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_autoptr (GPtrArray) doomed = g_ptr_array_new_with_free_func (
    g_object_unref);

  /* Collect first: closing a dialog takes it out of the table being
     walked. */
  g_hash_table_iter_init (&iter, self->instance_dialogs);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *id = key;
      AdwDialog *dialog = value;

      if (aml_instance_manager_get_by_id (self->manager, id) == NULL)
        g_ptr_array_add (doomed, g_object_ref (dialog));
    }

  for (guint i = 0; i < doomed->len; i++)
    adw_dialog_force_close (g_ptr_array_index (doomed, i));
}

/* Opens the dialog of the instance this row is for: its Settings page.
 * The id is put on the button when the row is bound, so a row that lost
 * its instance simply does nothing. */
static void
on_open_instance_clicked (GtkButton *button, gpointer user_data)
{
  AmlMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (button), "aml-instance-id");
  AmlInstance *instance = NULL;

  if (id == NULL)
    return;

  instance = aml_instance_manager_get_by_id (self->manager, id);

  if (instance == NULL)
    return;

  open_instance_dialog (self, instance);
}

/* Play while the instance is idle, Stop while it is busy. */
static void
row_update (GtkWidget *play, AmlLauncher *launcher)
{
  gboolean busy = aml_launch_state_is_busy (aml_launcher_get_state (launcher));
  GtkWidget *icon = g_object_get_data (G_OBJECT (play), "aml-icon");
  GtkWidget *label = g_object_get_data (G_OBJECT (play), "aml-label");

  gtk_label_set_label (GTK_LABEL (label), busy ? "Stop" : "Play");
  gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                busy ? "media-playback-stop-symbolic"
                                     : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text (play, busy ? "Stop" : "Play");

  /* The green belongs to a game that is about to start, not to a stop:
     an idle row asks, a running one only allows stopping. */
  if (busy)
    gtk_widget_remove_css_class (play, "suggested-action");
  else
    gtk_widget_add_css_class (play, "suggested-action");
}

static void
on_row_launcher_state (AmlLauncher *launcher, AmlLaunchState state G_GNUC_UNUSED,
                       const char *detail G_GNUC_UNUSED, gpointer user_data)
{
  row_update (GTK_WIDGET (user_data), launcher);
}

/* The row follows the launcher of the instance it shows. The row takes a
 * reference of its own, so a launcher that goes away first cannot leave
 * the row pointing at nothing. */
static void
row_set_launcher (GtkWidget *play, AmlLauncher *launcher)
{
  /* Stealing keeps the reference the row took in one place instead of
   * leaving it to the notify of whatever is put there next. */
  gpointer previous = g_object_steal_data (G_OBJECT (play), "aml-launcher");

  if (previous != NULL)
    {
      g_signal_handlers_disconnect_by_data (previous, play);
      g_object_unref (previous);
    }

  if (launcher == NULL)
    return;

  g_object_set_data_full (G_OBJECT (play), "aml-launcher",
                          g_object_ref (launcher), g_object_unref);

  /* The connection goes with the row, so a row that outlives several
     launchers never follows one of them twice. */
  g_signal_connect_object (launcher, "state",
                           G_CALLBACK (on_row_launcher_state), play, 0);
}

/* Stopping a running game closes it and loses whatever it has not saved,
 * so it is asked for before it happens. */
static void
on_stop_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AmlLauncher *launcher = user_data;
  const char *response =
    adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (launcher != NULL && g_strcmp0 (response, "stop") == 0)
    aml_launcher_stop (launcher);

  g_clear_object (&launcher);
}

static void
confirm_stop (AmlMainWindow *self, AmlLauncher *launcher)
{
  AmlInstance *instance = aml_launcher_get_instance (launcher);
  g_autofree char *name = NULL;
  g_autofree char *heading = NULL;
  AdwAlertDialog *alert = NULL;

  g_object_get (instance, "name", &name, NULL);
  heading = g_strdup_printf (
    "Stop %s?", name != NULL && *name != '\0' ? name : "the game");

  alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (
      heading, "The game is running and will close. Progress since the last "
               "save is lost."));
  adw_alert_dialog_set_heading_use_markup (alert, FALSE);
  adw_alert_dialog_set_body_use_markup (alert, FALSE);
  adw_alert_dialog_add_response (alert, "cancel", "Keep Playing");
  adw_alert_dialog_add_response (alert, "stop", "Stop");
  adw_alert_dialog_set_response_appearance (alert, "stop",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (alert, "cancel");
  adw_alert_dialog_set_close_response (alert, "cancel");
  adw_alert_dialog_choose (alert, GTK_WIDGET (self), NULL, on_stop_chosen,
                           g_object_ref (launcher));
}

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  AmlMainWindow *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (button), "aml-instance-id");
  AmlLauncher *launcher = window_launcher (self, id);

  if (launcher == NULL)
    return;

  /* One button for both: what a busy instance needs is a stop, and what
     an idle one needs is a play. */
  if (aml_launch_state_is_busy (aml_launcher_get_state (launcher)))
    confirm_stop (self, launcher);
  else
    aml_launcher_start (launcher);
}

/* Puts the icon of @instance in @image, or the game square when it has
 * none or its file cannot be read. A row follows the icon of its
 * instance, so an icon chosen on the instance page and the icon a
 * modpack install gives an instance both land here. */
static void
row_set_icon (GtkImage *image, AmlInstance *instance)
{
  g_autofree char *path = NULL;

  g_object_get (instance, "icon", &path, NULL);

  if (path != NULL && g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (GFile) file = g_file_new_for_path (path);
      g_autoptr (GdkTexture) texture = gdk_texture_new_from_file (file, NULL);

      if (texture != NULL)
        {
          gtk_image_set_from_paintable (image, GDK_PAINTABLE (texture));
          return;
        }
    }

  gtk_image_set_from_icon_name (image, "applications-games-symbolic");
}

static void
on_row_icon_changed (GObject *object, GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer user_data)
{
  row_set_icon (GTK_IMAGE (user_data), AML_INSTANCE (object));
}

/* One row of the instance list: an AdwActionRow of the boxed list,
 * the same kind of row the content pages and the modpack browser use,
 * so the list is sized and margined exactly like theirs. Every button
 * is wired here and carries the instance id, which is what the click
 * handlers go by. */
static GtkWidget *
create_instance_row (gpointer item, gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AmlInstance *instance = AML_INSTANCE (item);
  g_autofree char *name = NULL;
  g_autofree char *minecraft_version = NULL;
  g_autofree char *subtitle = NULL;
  AmlLoader loader = AML_LOADER_VANILLA;
  const char *id = NULL;
  const char *loader_name = NULL;
  AmlLauncher *launcher = NULL;
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *icon = gtk_image_new_from_icon_name ("applications-games-symbolic");
  GtkWidget *settings = gtk_button_new_from_icon_name ("emblem-system-symbolic");
  GtkWidget *play = gtk_button_new ();
  GtkWidget *play_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *play_icon = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
  GtkWidget *play_label = gtk_label_new ("Play");

  g_object_get (instance,
                "name", &name,
                "minecraft-version", &minecraft_version,
                "loader", &loader,
                NULL);

  loader_name = aml_loader_to_string (loader);
  if (loader_name != NULL && loader_name[0] != '\0')
    {
      g_autofree char *capitalized = g_strdup (loader_name);

      /* "fabric" is written as "Fabric" in the row. */
      capitalized[0] = g_ascii_toupper (capitalized[0]);
      subtitle = g_strdup_printf ("%s %s", minecraft_version, capitalized);
    }
  else
    subtitle = g_strdup (minecraft_version);

  /* Names and versions are plain text and may hold an "&". */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_title_lines (ADW_ACTION_ROW (row), 1);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);

  gtk_widget_set_valign (settings, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (settings, "flat");
  gtk_widget_set_tooltip_text (settings, "Settings");
  gtk_accessible_update_property (GTK_ACCESSIBLE (settings),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Settings",
                                  -1);

  gtk_image_set_pixel_size (GTK_IMAGE (play_icon), 16);
  gtk_box_append (GTK_BOX (play_box), play_icon);
  gtk_box_append (GTK_BOX (play_box), play_label);
  gtk_button_set_child (GTK_BUTTON (play), play_box);
  gtk_widget_set_valign (play, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (play, "suggested-action");
  gtk_widget_set_tooltip_text (play, "Play");
  gtk_accessible_update_property (GTK_ACCESSIBLE (play),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Play", -1);

  /* The play state swaps the icon and the label, so they are known by
     the button itself rather than walked for. */
  g_object_set_data (G_OBJECT (play), "aml-icon", play_icon);
  g_object_set_data (G_OBJECT (play), "aml-label", play_label);

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), settings);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), play);

  g_signal_connect (settings, "clicked",
                    G_CALLBACK (on_open_instance_clicked), self);
  g_signal_connect (play, "clicked", G_CALLBACK (on_play_clicked), self);

  /* The icon follows the instance's; the connection goes with the
     image, so a row that is destroyed stops following it on its own. */
  row_set_icon (GTK_IMAGE (icon), instance);
  g_signal_connect_object (instance, "notify::icon",
                           G_CALLBACK (on_row_icon_changed), icon, 0);

  id = aml_instance_get_id (instance);

  /* The buttons all lead back to this instance; the id is what their
     click handlers go by. */
  g_object_set_data_full (G_OBJECT (settings), "aml-instance-id",
                          g_strdup (id), g_free);
  g_object_set_data_full (G_OBJECT (play), "aml-instance-id", g_strdup (id),
                          g_free);

  /* The launcher of this instance: the button follows it from here on. */
  launcher = window_launcher (self, id);
  row_set_launcher (play, launcher);

  if (launcher != NULL)
    row_update (play, launcher);

  /* A click anywhere on the row, outside its buttons, opens the
     instance. An AdwActionRow is not activatable by default. */
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

  return row;
}

/* A row clicked anywhere but on its buttons opens the instance. The
 * row's index comes from the boxed list, which holds the rows in the
 * same order as the manager. */
static void
on_row_activate (GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row,
                 gpointer user_data)
{
  AmlMainWindow *self = user_data;
  GListStore *instances = aml_instance_manager_get_instances (self->manager);
  AmlInstance *instance =
    g_list_model_get_item (G_LIST_MODEL (instances),
                           gtk_list_box_row_get_index (row));

  if (instance == NULL)
    return;

  open_instance_dialog (self, instance);
  g_object_unref (instance);
}

static void
update_visible_page (AmlMainWindow *self)
{
  GListStore *instances = aml_instance_manager_get_instances (self->manager);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (instances));

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack),
                                    n_items > 0 ? "instances" : "empty");
}

/* An instance that is gone leaves its launcher behind: a game running
 * for it is asked to stop, because there is nothing left to run. The
 * table holds one entry per instance, so walking it is cheap. */
static void
drop_launchers_of_removed_instances (AmlMainWindow *self)
{
  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;

  g_hash_table_iter_init (&iter, self->launchers);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      AmlLauncher *launcher = value;

      if (aml_instance_manager_get_by_id (self->manager, (const char *) key)
            != NULL)
        continue;

      aml_launcher_stop (launcher);
      g_hash_table_iter_remove (&iter);
    }
}

static void
on_instances_changed (AmlMainWindow *self,
                      GListModel    *model G_GNUC_UNUSED,
                      guint          position G_GNUC_UNUSED,
                      guint          removed G_GNUC_UNUSED,
                      guint          added G_GNUC_UNUSED)
{
  update_visible_page (self);
  drop_launchers_of_removed_instances (self);

  /* An instance that is gone has nothing to show, so its dialog goes
     with it. This is how deleting an instance closes its dialog,
     whichever screen the delete was asked from. */
  close_dialogs_of_removed_instances (self);
}

static void
aml_main_window_init (AmlMainWindow *self)
{
  g_autoptr (GMenu) add_menu = create_add_menu ();
  g_autoptr (GMenu) app_menu = create_app_menu ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *add_button = gtk_menu_button_new ();
  GtkWidget *users_button = gtk_menu_button_new ();
  GtkWidget *app_menu_button = gtk_menu_button_new ();
  GtkWidget *users_child = NULL;
  GtkWidget *empty_page = adw_status_page_new ();
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *add_popover = NULL;

  /* The settings and the accounts are the window's own: every launcher of
     an instance is handed the same ones, so the whole window agrees on
     what a launch does and as whom it plays. */
  self->settings = aml_settings_new ();
  self->accounts = aml_accounts_new ();

  g_action_map_add_action_entries (G_ACTION_MAP (self), window_entries,
                                   G_N_ELEMENTS (window_entries), self);

  /* Start of the header: the "+" menu. */
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (add_button),
                                 "list-add-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (add_button),
                                  G_MENU_MODEL (add_menu));
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), add_button);

  add_popover = GTK_WIDGET (
    gtk_menu_button_get_popover (GTK_MENU_BUTTON (add_button)));
  if (add_popover != NULL)
    set_menu_item_tooltip (add_popover, "Download Modpack",
                           "Search Modrinth and install a modpack");

  /* End of the header: the accounts, then the application menu. The
   * button shows the avatar of the active account, and what is behind it
   * is the list of the accounts this copy of AML has, which is also where
   * a new one is added. */
  self->users_popover = aml_users_popover_new (self->accounts);
  /* A widget is created with a floating reference, and set_popover()
     below sinks that one for the button. The window keeps a reference of
     its own as well, so that dropping it in dispose() never frees a
     popover the button still holds. */
  g_object_ref_sink (self->users_popover);
  users_child = aml_users_popover_get_button_content (self->users_popover);
  gtk_menu_button_set_child (GTK_MENU_BUTTON (users_button), users_child);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (users_button),
                              GTK_WIDGET (self->users_popover));
  /* The child is a box around the avatar, so the menu button does not
     recognize it as an icon button and would draw a frame behind it. */
  gtk_widget_add_css_class (users_button, "flat");
  /* The button is an icon and nothing else, so it needs to be named for
     what it opens rather than read from a label that is not there. */
  gtk_accessible_update_property (GTK_ACCESSIBLE (users_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Accounts",
                                  -1);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (app_menu_button),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app_menu_button),
                                  G_MENU_MODEL (app_menu));

  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app_menu_button);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), users_button);

  /* The empty state has no button in the middle: the "+" menu is the
   * only way to create an instance. */
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), "No Instances Yet");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty_page),
                                   "Use the + button in the header to create one.");
  gtk_widget_set_hexpand (empty_page, TRUE);
  gtk_widget_set_vexpand (empty_page, TRUE);

  self->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->stack), empty_page, "empty");
  gtk_widget_set_hexpand (self->stack, TRUE);
  gtk_widget_set_vexpand (self->stack, TRUE);

  /* The list is the content of the window: an instance is a window of
     its own and is never a page here. */
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), self->stack);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  /* A toast is how the window says something the user did not ask for: a
   game that crashed, an operation that failed. Without it the only way
   * to find out is to open the logs. */
  self->toasts = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toasts),
                               GTK_WIDGET (toolbar));

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      GTK_WIDGET (self->toasts));

  gtk_window_set_title (GTK_WINDOW (self), "AML");
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 640);
}

AmlMainWindow *
aml_main_window_new (GtkApplication *application, AmlInstanceManager *manager)
{
  g_return_val_if_fail (GTK_IS_APPLICATION (application), NULL);
  g_return_val_if_fail (AML_IS_INSTANCE_MANAGER (manager), NULL);

  AmlMainWindow *self = g_object_new (AML_TYPE_MAIN_WINDOW,
                                      "application", application,
                                      NULL);
  GListStore *instances = aml_instance_manager_get_instances (manager);
  GtkWidget *list = NULL;
  GtkWidget *scroll = NULL;

  self->manager = g_object_ref (manager);

  /* The table owns the launchers: the key is the id of the instance and
     the value is its launcher. */
  self->launchers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           g_object_unref);

  /* The open instance dialogs are only bookkept here: the window they
     are presented on owns them. */
  self->instance_dialogs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);

  /* The instances are a boxed list, the same list the content pages and
     the modpack browser use: full width rows with hairlines between
     them instead of cards on a margin. The top row's corners are
     square, because the list is flush with the top of the window. */
  list = gtk_list_box_new ();
  gtk_widget_add_css_class (list, "boxed-list");
  gtk_widget_add_css_class (list, "flush-top");
  gtk_widget_add_css_class (list, "flush-bottom");
  /* The card background of the list only covers its rows: the space
     under them is the window background, the same colour as the hairline
     between two rows, instead of the whole window being card coloured. */
  gtk_widget_set_valign (list, GTK_ALIGN_START);
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                   GTK_SELECTION_NONE);
  g_signal_connect (list, "row-activated", G_CALLBACK (on_row_activate),
                    self);
  gtk_list_box_bind_model (GTK_LIST_BOX (list), G_LIST_MODEL (instances),
                           create_instance_row, self, NULL);

  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
  gtk_widget_set_hexpand (scroll, TRUE);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_stack_add_named (GTK_STACK (self->stack), scroll, "instances");

  /* The connection goes away with the window, which outlives every
   * callback it could receive. */
  g_signal_connect_object (instances, "items-changed",
                           G_CALLBACK (on_instances_changed), self,
                           G_CONNECT_SWAPPED);
  update_visible_page (self);

  return self;
}

static void
aml_main_window_dispose (GObject *object)
{
  AmlMainWindow *self = AML_MAIN_WINDOW (object);

  /* A running game keeps its own launcher alive through the process
     readers, so dropping the table here never kills a game in front of
     the user; it only lets go of the ones that are not running. */
  g_clear_pointer (&self->launchers, g_hash_table_unref);
  g_clear_pointer (&self->instance_dialogs, g_hash_table_unref);
  g_clear_object (&self->manager);
  g_clear_object (&self->settings);
  g_clear_object (&self->accounts);
  g_clear_object (&self->users_popover);

  G_OBJECT_CLASS (aml_main_window_parent_class)->dispose (object);
}

static void
aml_main_window_class_init (AmlMainWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_main_window_dispose;
}
