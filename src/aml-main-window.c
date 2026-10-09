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
#include "aml-downloads.h"
#include "aml-instance-page.h"
#include "aml-i18n.h"
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

  /* What a launch ended with, which is worth reading after it happened:
     a toast is gone in seconds, a banner stays until it is dismissed. */
  GtkWidget *banner;

  /* What is downloading right now, shown next to the "+" menu: a spinner
     while anything is on its way, and a dropdown listing each entry. The
     rows are keyed by the AmlDownload they show. */
  GtkWidget *downloads_button;  /* a GtkMenuButton around the spinner */
  GtkWidget *downloads_spinner;
  GtkWidget *downloads_icon;      /* what the button opens, beside the spinner */
  GtkWidget *downloads_list;    /* the box inside the dropdown */
  GHashTable *download_rows;    /* AmlDownload* -> DownloadRow* */
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
on_language_selected (GObject *object,
                      GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer user_data)
{
  AmlMainWindow *self = user_data;
  AdwComboRow *row = ADW_COMBO_ROW (object);
  gsize length = 0;
  const AmlLanguage *languages = aml_i18n_languages (&length);
  guint selected = adw_combo_row_get_selected (row);

  if (selected >= length)
    return;

  aml_settings_set_language (self->settings, languages[selected].code, NULL);
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
  GtkWidget *logs_group = adw_preferences_group_new ();
  GtkWidget *language_group = adw_preferences_group_new ();
  GtkWidget *language_row = adw_combo_row_new ();
  GtkWidget *row = adw_switch_row_new ();
  GtkWidget *limit_row = NULL;
  GtkWidget *unstable_row = adw_switch_row_new ();
  GtkWidget *crash_row = adw_switch_row_new ();

  /* Every one of these rows can show a path or an error message from
     GIO, which is plain text and may hold an "&". */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (unstable_row),
                                      FALSE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (crash_row), FALSE);

  adw_dialog_set_title (dialog, _("Settings"));
  adw_preferences_dialog_set_search_enabled (ADW_PREFERENCES_DIALOG (dialog),
                                             FALSE);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (versions_group),
                                    _("Versions"));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                 _("Show Snapshots"));
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
                                  _("Show unstable Mod Loader versions"));
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (unstable_row),
    aml_settings_get_show_unstable_loaders (self->settings));
  g_signal_connect (unstable_row, "notify::active",
                    G_CALLBACK (on_show_unstable_loaders_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (versions_group),
                             unstable_row);

  /* Logs */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (logs_group),
                                   _("Logs"));

  {
    gsize length = 0;
    const AmlLanguage *languages = aml_i18n_languages (&length);
    GtkStringList *model = gtk_string_list_new (NULL);
    guint selected = 0;
    const char *current = aml_settings_get_language (self->settings);

    for (gsize i = 0; i < length; i++)
      {
        gtk_string_list_append (model, _(languages[i].name));
        if (g_strcmp0 (current, languages[i].code) == 0)
          selected = (guint) i;
      }

    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (language_group),
                                     _("Language"));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (language_row),
                                   _("Interface language"));
    adw_combo_row_set_model (ADW_COMBO_ROW (language_row),
                             G_LIST_MODEL (model));
    adw_combo_row_set_selected (ADW_COMBO_ROW (language_row), selected);
    g_signal_connect (language_row, "notify::selected",
                      G_CALLBACK (on_language_selected), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (language_group),
                               language_row);
    g_object_unref (model);
  }

  limit_row = adw_spin_row_new (
    gtk_adjustment_new (aml_settings_get_log_line_limit (self->settings), 100,
                        100000, 500, 5000, 0),
    0, 0);
  adw_spin_row_set_digits (ADW_SPIN_ROW (limit_row), 0);
  adw_spin_row_set_numeric (ADW_SPIN_ROW (limit_row), TRUE);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (limit_row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (limit_row),
                                  _("Line Limit"));
  g_signal_connect (limit_row, "notify::value",
                    G_CALLBACK (on_log_limit_changed), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (logs_group), limit_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (crash_row),
                                  _("Open Log Folder After Crash"));
  adw_switch_row_set_active (
    ADW_SWITCH_ROW (crash_row),
    aml_settings_get_open_logs_after_crash (self->settings));
  g_signal_connect (crash_row, "notify::active",
                    G_CALLBACK (on_open_logs_after_crash_toggled), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (logs_group), crash_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (versions_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             ADW_PREFERENCES_GROUP (logs_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                             ADW_PREFERENCES_GROUP (language_group));
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
  const char *app_ids[] = { "io.gitlab.anotherminecraftlauncher.AML", NULL };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), "AML");
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "Flia");

  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog),
                                "https://gitlab.com/anotherminecraftlauncher/aml");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog),
                                  "Copyright (C) 2026 Flia");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog),
                                     GTK_LICENSE_GPL_3_0_ONLY);
  adw_about_dialog_add_credit_section (ADW_ABOUT_DIALOG (dialog),
                                       "Application ID", app_ids);
  adw_dialog_set_title (dialog, _("About AML"));

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* The shortcuts the window answers to, in the list the Shortcuts dialog
 * shows and as the accelerators registered at the end of init. */
static void
on_shortcuts (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  AmlMainWindow *self = user_data;
  AdwDialog *dialog = adw_shortcuts_dialog_new ();
  AdwShortcutsSection *general = adw_shortcuts_section_new ("General");

  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new ("Show Shortcuts", "F1"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new ("Create Instance", "<primary>n"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new ("Settings", "<primary>comma"));
  adw_shortcuts_section_add (
    general, adw_shortcuts_item_new ("Quit", "<primary>q"));

  adw_shortcuts_dialog_add (ADW_SHORTCUTS_DIALOG (dialog), general);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static const GActionEntry window_entries[] = {
  { .name = "create-instance", .activate = on_create_instance },
  { .name = "download-modpack", .activate = on_download_modpack },
  { .name = "settings", .activate = on_settings },
  { .name = "shortcuts", .activate = on_shortcuts },
  { .name = "about", .activate = on_about },
};

static GMenu *
create_add_menu (void)
{
  GMenu *menu = g_menu_new ();

  g_menu_append (menu, _("Create Instance"), "win.create-instance");
  g_menu_append (menu, _("Download Modpack"), "win.download-modpack");

  return menu;
}

static GMenu *
create_app_menu (void)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) general = g_menu_new ();
  g_autoptr (GMenu) rest = g_menu_new ();

  g_menu_append (general, _("Settings"), "win.settings");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (general));

  g_menu_append (rest, _("Keyboard Shortcuts"), "win.shortcuts");
  g_menu_append (rest, _("About AML"), "win.about");
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

/* The banner is dismissed by its own button. */
static void
on_banner_dismiss (AdwBanner *banner, gpointer user_data G_GNUC_UNUSED)
{
  adw_banner_set_revealed (banner, FALSE);
}

/* What a launch ends with is said out loud: a crash, a launch that never
   got to the game, and an account that has to sign in again. It stays in
   a banner until it is read, so a reason cannot be missed the way a
   second-long toast is. */
static void
on_window_launcher_state (AmlLauncher *launcher G_GNUC_UNUSED,
                          AmlLaunchState state,
                          const char *detail, gpointer user_data)
{
  AmlMainWindow *self = user_data;
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

  if (self->banner == NULL)
    return;

  if (detail != NULL && *detail != '\0')
    message = g_strdup_printf ("%s: %s", what, detail);
  else
    message = g_strdup (what);

  adw_banner_set_use_markup (ADW_BANNER (self->banner), FALSE);
  adw_banner_set_title (ADW_BANNER (self->banner), message);
  adw_banner_set_revealed (ADW_BANNER (self->banner), TRUE);
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

/* Play while the instance is idle, Stop while it is busy. */
static void
row_update (GtkWidget *play, AmlLauncher *launcher)
{
  gboolean busy = aml_launch_state_is_busy (aml_launcher_get_state (launcher));
  GtkWidget *icon = g_object_get_data (G_OBJECT (play), "aml-icon");
  GtkWidget *label = g_object_get_data (G_OBJECT (play), "aml-label");

  gtk_label_set_label (GTK_LABEL (label), busy ? _("Stop") : _("Play"));
  gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                busy ? "media-playback-stop-symbolic"
                                     : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text (play, busy ? _("Stop") : _("Play"));

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
  adw_alert_dialog_add_response (alert, "cancel", _("Keep Playing"));
  adw_alert_dialog_add_response (alert, "stop", _("Stop"));
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
  GtkWidget *play = gtk_button_new ();
  GtkWidget *play_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *play_icon = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
  GtkWidget *play_label = gtk_label_new (_("Play"));

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
  gtk_widget_set_size_request (icon, 48, 48);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);

  gtk_image_set_pixel_size (GTK_IMAGE (play_icon), 16);
  gtk_widget_set_size_request (play_icon, 16, 16);
  gtk_box_append (GTK_BOX (play_box), play_icon);
  gtk_box_append (GTK_BOX (play_box), play_label);
  gtk_button_set_child (GTK_BUTTON (play), play_box);
  gtk_widget_set_valign (play, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (play, "suggested-action");
  gtk_widget_set_tooltip_text (play, _("Play"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (play),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, _("Play"), -1);

  /* The play state swaps the icon and the label, so they are known by
     the button itself rather than walked for. */
  g_object_set_data (G_OBJECT (play), "aml-icon", play_icon);
  g_object_set_data (G_OBJECT (play), "aml-label", play_label);

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), play);

  g_signal_connect (play, "clicked", G_CALLBACK (on_play_clicked), self);

  /* The icon follows the instance's; the connection goes with the
     image, so a row that is destroyed stops following it on its own. */
  row_set_icon (GTK_IMAGE (icon), instance);
  g_signal_connect_object (instance, "notify::icon",
                           G_CALLBACK (on_row_icon_changed), icon, 0);

  id = aml_instance_get_id (instance);

  /* The button leads back to this instance; the id is what its click
     handler goes by. */
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

/* One dropdown entry: the box GTK owns plus the widgets that change as
 * the download does. The box is a child of the downloads list, so it is
 * destroyed with it; only the struct is ours. */
typedef struct {
  GtkWidget *box;
  GtkWidget *phase;  /* what it is doing, on the left */
  GtkWidget *count;  /* how far it is, on the right */
  GtkWidget *bar;
} DownloadRow;

static void
download_row_free (gpointer data)
{
  g_free (data);
}

static void
on_download_cancel (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  aml_download_cancel (AML_DOWNLOAD (user_data));
}

/* What is on the right of the phase line: "312.0/500.0 MB" for a transfer
 * whose length the server gave, "12 of 48 files" for an install. */
static char *
download_count_text (AmlDownload *download)
{
  guint64 total = aml_download_get_total (download);
  guint64 done = aml_download_get_done (download);

  if (total == 0)
    return g_strdup ("");

  if (aml_download_get_unit (download) == AML_DOWNLOAD_UNIT_BYTES)
    {
      gboolean gigabytes = total >= (G_GUINT64_CONSTANT (1) << 30);
      double divisor = gigabytes ? 1024.0 * 1024.0 * 1024.0
                                 : 1024.0 * 1024.0;

      return g_strdup_printf ("%.1f/%.1f %s", (double) done / divisor,
                              (double) total / divisor,
                              gigabytes ? "GB" : "MB");
    }

  return g_strdup_printf ("%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT, done,
                          total);
}

static void
refresh_downloads (AmlMainWindow *self)
{
  GPtrArray *jobs = aml_downloads_get_all ();
  /* What the content pages have picked: what the icon carries a number
     of, and the reason the button is there when nothing is downloading. */
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  if (self->downloads_list == NULL)
    return;

  /* The rows of the downloads that are over. */
  g_hash_table_iter_init (&iter, self->download_rows);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      DownloadRow *row = value;
      gboolean present = FALSE;

      for (guint i = 0; i < jobs->len; i++)
        if (g_ptr_array_index (jobs, i) == key)
          present = TRUE;

      if (!present)
        {
          gtk_box_remove (GTK_BOX (self->downloads_list), row->box);
          g_hash_table_iter_remove (&iter);
        }
    }

  for (guint i = 0; i < jobs->len; i++)
    {
      AmlDownload *download = g_ptr_array_index (jobs, i);
      DownloadRow *row = g_hash_table_lookup (self->download_rows, download);
      const char *phase = aml_download_get_detail (download);
      g_autofree char *count = download_count_text (download);
      guint64 done = aml_download_get_done (download);
      guint64 total = aml_download_get_total (download);

      if (row == NULL)
        {
          GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
          GtkWidget *title = gtk_label_new (aml_download_get_title (download));
          GtkWidget *sub = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          GtkWidget *line_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          GtkWidget *cancel = gtk_button_new_from_icon_name (
            "window-close-symbolic");

          gtk_widget_set_halign (title, GTK_ALIGN_START);
          gtk_widget_add_css_class (title, "heading");
          gtk_widget_set_hexpand (title, TRUE);
          gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
          gtk_label_set_xalign (GTK_LABEL (title), 0.0f);

          row = g_new0 (DownloadRow, 1);
          row->box = box;
          row->phase = gtk_label_new (NULL);
          row->count = gtk_label_new (NULL);
          row->bar = gtk_progress_bar_new ();

          /* The phase ("Downloading compressed modpack") on the left and
             the amount ("312.0/500.0 MB") on the right, under the title. */
          gtk_widget_set_halign (row->phase, GTK_ALIGN_START);
          gtk_widget_set_hexpand (row->phase, TRUE);
          gtk_label_set_xalign (GTK_LABEL (row->phase), 0.0f);
          gtk_widget_add_css_class (row->phase, "dim-label");

          gtk_widget_set_halign (row->count, GTK_ALIGN_END);
          gtk_widget_set_valign (row->count, GTK_ALIGN_CENTER);
          gtk_widget_add_css_class (row->count, "dim-label");
          gtk_label_set_xalign (GTK_LABEL (row->count), 1.0f);

          gtk_widget_set_margin_start (box, 12);
          gtk_widget_set_margin_end (box, 12);
          gtk_widget_set_margin_top (box, 8);
          gtk_widget_set_margin_bottom (box, 8);
          gtk_widget_set_size_request (box, 360, -1);

          /* The bar takes the room and the × sits right of it. */
          gtk_widget_set_hexpand (row->bar, TRUE);
          gtk_widget_set_valign (row->bar, GTK_ALIGN_CENTER);
          gtk_widget_add_css_class (cancel, "flat");
          gtk_widget_set_valign (cancel, GTK_ALIGN_CENTER);
          gtk_widget_set_tooltip_text (cancel, _("Cancel"));
          gtk_accessible_update_property (GTK_ACCESSIBLE (cancel),
                                          GTK_ACCESSIBLE_PROPERTY_LABEL,
                                          "Cancel", -1);

          g_signal_connect (cancel, "clicked", G_CALLBACK (on_download_cancel),
                            download);

          gtk_box_append (GTK_BOX (sub), row->phase);
          gtk_box_append (GTK_BOX (sub), row->count);
          gtk_box_append (GTK_BOX (line_box), row->bar);
          gtk_box_append (GTK_BOX (line_box), cancel);

          gtk_box_append (GTK_BOX (box), title);
          gtk_box_append (GTK_BOX (box), sub);
          gtk_box_append (GTK_BOX (box), line_box);
          gtk_box_append (GTK_BOX (self->downloads_list), box);

          g_hash_table_insert (self->download_rows, download, row);
        }

      gtk_label_set_text (GTK_LABEL (row->phase),
                          phase != NULL ? phase : "");
      gtk_label_set_text (GTK_LABEL (row->count), count);

      if (total > 0)
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (row->bar),
                                       (double) done / (double) total);
      else
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (row->bar));
    }

  /* The button is the page of the header: the spinner that appears while
     anything is downloading and opens a dropdown of what is, and the
     number of what the content pages have picked on the icon beside it.
     Either of the two is a reason for it to be there at all. */
  if (jobs->len > 0)
    {
      gtk_widget_set_visible (self->downloads_button, TRUE);
    }
  else
    {
      GtkPopover *popover = gtk_menu_button_get_popover (
        GTK_MENU_BUTTON (self->downloads_button));

      /* The last download is over: a dropdown with nothing left in it
         should not stay open over the window. */
      if (popover != NULL)
        gtk_popover_popdown (popover);

       gtk_widget_set_visible (self->downloads_button, FALSE);
    }

  /* Browser selections are shown by the browser's own button, not in this
     menu. This header button is only for transfers that are running. */
  {
    gtk_widget_set_visible (self->downloads_spinner, jobs->len > 0);
    gtk_widget_set_visible (self->downloads_icon, jobs->len == 0);
  }
}

static void
on_downloads_changed (AmlDownloads *downloads G_GNUC_UNUSED, gpointer user_data)
{
  refresh_downloads (user_data);
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

  /* Flat is the default top-bar style, and it paints no surface of its
     own, so the header would take the window background and read as the
     same colour as the content under it. Raised is what gives a header
     the colour every Adwaita header has. */
  adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (toolbar),
                                      ADW_TOOLBAR_RAISED);

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

  /* Beside the "+": the downloads. A spinner appears while anything is
     downloading and opens a dropdown of what is; the icon beside it
     carries the number of what the content pages have picked and
     installed at once. The button is hidden when there is neither. */
  self->downloads_icon = gtk_image_new_from_icon_name (
    "folder-download-symbolic");
   self->downloads_spinner = adw_spinner_new ();
  self->downloads_button = gtk_menu_button_new ();
  self->downloads_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->download_rows = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               NULL, download_row_free);

  {
    GtkWidget *packed = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *icon_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *popover = gtk_popover_new ();
    GtkWidget *scroller = gtk_scrolled_window_new ();

    /* The icon and the spinner share the middle: the icon says what the
       button opens, the spinner that something is on its way. */
    gtk_widget_set_valign (self->downloads_icon, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (icon_row), self->downloads_icon);
    gtk_widget_set_valign (self->downloads_spinner, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (icon_row), self->downloads_spinner);

    gtk_menu_button_set_child (GTK_MENU_BUTTON (self->downloads_button),
                               icon_row);
    gtk_widget_add_css_class (self->downloads_button, "flat");
    gtk_menu_button_set_always_show_arrow (
      GTK_MENU_BUTTON (self->downloads_button), FALSE);
  gtk_widget_set_tooltip_text (self->downloads_button, _("Downloads"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (self->downloads_button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, _("Downloads"),
                                    -1);
    gtk_widget_set_visible (self->downloads_button, FALSE);

    gtk_box_append (GTK_BOX (packed), self->downloads_button);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (
      GTK_SCROLLED_WINDOW (scroller), TRUE);
    gtk_scrolled_window_set_max_content_height (
      GTK_SCROLLED_WINDOW (scroller), 360);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                   self->downloads_list);
    gtk_popover_set_child (GTK_POPOVER (popover), scroller);
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->downloads_button),
                                 popover);

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), packed);
  }

  g_signal_connect_object (aml_downloads_get_default (), "changed",
                           G_CALLBACK (on_downloads_changed), self, 0);

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
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, _("Accounts"),
                                  -1);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (app_menu_button),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (app_menu_button),
                                  G_MENU_MODEL (app_menu));

  adw_header_bar_pack_end (ADW_HEADER_BAR (header), app_menu_button);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), users_button);

  /* The empty state has no button in the middle: the "+" menu is the
   * only way to create an instance. */
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), _("No Instances Yet"));
  adw_status_page_set_description (ADW_STATUS_PAGE (empty_page),
                                   _("Use the + button in the header to create one"));
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

  /* A launch that ends badly is said in a banner under the header, where
     it stays until it is read: a toast is gone in seconds, which is
     exactly when a reason is worth having. */
  self->banner = adw_banner_new ("");
  adw_banner_set_button_label (ADW_BANNER (self->banner), "Dismiss");
  adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
  g_signal_connect (self->banner, "button-clicked",
                    G_CALLBACK (on_banner_dismiss), NULL);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), self->banner);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      GTK_WIDGET (toolbar));

  gtk_window_set_title (GTK_WINDOW (self), "AML");
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 640);

  /* A download may already be running (a window opened while one does),
     so the header starts agreeing with the manager rather than assuming
     nothing is on its way. */
  refresh_downloads (self);
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

  /* The standard shortcuts, registered on the application because the
     window is not the one that owns them. */
  {
    const char *create[] = { "<primary>n", NULL };
    const char *settings[] = { "<primary>comma", NULL };
    const char *shortcuts[] = { "F1", NULL };
    const char *quit[] = { "<primary>q", NULL };

    gtk_application_set_accels_for_action (application, "win.create-instance",
                                           create);
    gtk_application_set_accels_for_action (application, "win.settings",
                                           settings);
    gtk_application_set_accels_for_action (application, "win.shortcuts",
                                           shortcuts);
    gtk_application_set_accels_for_action (application, "app.quit", quit);
  }

  /* The table owns the launchers: the key is the id of the instance and
     the value is its launcher. */
  self->launchers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           g_object_unref);

  /* The open instance dialogs are only bookkept here: the window they
     are presented on owns them. */
  self->instance_dialogs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);

  /* The instances are a boxed list, the same list the content pages and
     the modpack browser use, on a margin the way a boxed list is meant
     to sit: its card has round corners on every side. */
  list = gtk_list_box_new ();
  gtk_widget_add_css_class (list, "boxed-list");
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
  gtk_widget_set_margin_top (scroll, 12);
  gtk_widget_set_margin_bottom (scroll, 12);
  gtk_widget_set_margin_start (scroll, 12);
  gtk_widget_set_margin_end (scroll, 12);
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
  g_clear_pointer (&self->download_rows, g_hash_table_unref);

  G_OBJECT_CLASS (aml_main_window_parent_class)->dispose (object);
}

static void
aml_main_window_class_init (AmlMainWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_main_window_dispose;
}
