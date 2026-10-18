/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* The small pieces of the browser UI that have broken before.
 *
 * Rows take their title and subtitle as plain text: Modrinth titles and
 * descriptions, file names and paths hold "&" and "<", and Pango reads
 * them as markup unless the row is told not to. A row a user is meant to
 * click is made activatable, because an AdwActionRow is not by default.
 * An icon the toolkit cannot decode keeps the placeholder instead of an
 * empty square. And a request that finishes after the page it belongs to
 * is gone must not touch it.
 *
 * Everything here is local. No display means the widget part is skipped,
 * never faked. */

#include <gtk/gtk.h>
#include <libadwaita-1/adwaita.h>

#include <glib/gstdio.h>

#include "nuvasha-content-page.h"
#include "nuvasha-icons.h"
#include "nuvasha-instance-manager.h"
#include "nuvasha-modrinth.h"
#include "nuvasha-ui-feedback.h"

/* The titles the browser has to survive: an ampersand, a tag-like word
 * and a name that reads like markup. */
static const char *const markup_titles[] = {
  "Fresh & Smooth",
  "Guns&Fun",
  "Ben 10 | Omnicraft - Alien Evolution & Omnitrix RPG",
  "<b>x</b>",
};

static guint markup_warnings = 0;

static void
test_feedback_ignores_unattached_pages (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = adw_toast_overlay_new ();

  nuvasha_ui_show_toast (NULL, NULL, "message");
  nuvasha_ui_show_toast (page, overlay, "message");
  g_assert_null (gtk_widget_get_root (page));
  g_assert_nonnull (overlay);
  g_object_ref_sink (page);
  g_object_unref (page);
  g_object_ref_sink (overlay);
  g_object_unref (overlay);
}

/* GTK logs through the structured writer, so the count is taken there
 * and the message is still handed to the default writer to be printed
 * like any other. */
static GLogWriterOutput
count_markup_warning (GLogLevelFlags   log_level,
                      const GLogField *fields,
                      gsize            n_fields,
                      gpointer         user_data)
{
  const char *message = NULL;

  (void) user_data;

  for (gsize i = 0; i < n_fields; i++)
    {
      if (g_strcmp0 (fields[i].key, "MESSAGE") == 0)
        message = fields[i].value;
    }

  if ((log_level & G_LOG_LEVEL_WARNING) != 0 &&
      message != NULL && g_strstr_len (message, -1, "markup") != NULL)
    markup_warnings++;

  return g_log_writer_default (log_level, fields, n_fields, NULL);
}

static void
test_markup (void)
{
  GtkWidget *group = adw_preferences_group_new ();

  markup_warnings = 0;

  for (guint i = 0; i < G_N_ELEMENTS (markup_titles); i++)
    {
      GtkWidget *row = adw_action_row_new ();

      /* The order is the whole point: markup off first, then the text. */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                     markup_titles[i]);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                                   markup_titles[i]);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
    }

  g_object_ref_sink (group);
  g_object_unref (group);

  g_assert_cmpuint (markup_warnings, ==, 0);
  g_print ("ok - titles and descriptions with markup characters are plain\n");
}

static gboolean activated = FALSE;

static void
on_activated (AdwActionRow *row G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  activated = TRUE;
}

static void
test_row_activation (void)
{
  GtkWidget *group = adw_preferences_group_new ();
  GtkWidget *row = adw_action_row_new ();

  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Clickable");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);

  /* An AdwActionRow is unactivatable until it is told otherwise; a click
   * on a row that is not does nothing at all. */
  g_assert_false (gtk_list_box_row_get_activatable (GTK_LIST_BOX_ROW (row)));

  g_signal_connect (row, "activated", G_CALLBACK (on_activated), NULL);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_assert_true (gtk_list_box_row_get_activatable (GTK_LIST_BOX_ROW (row)));

  adw_action_row_activate (ADW_ACTION_ROW (row));
  g_assert_true (activated);

  g_object_ref_sink (group);
  g_object_unref (group);
  g_print ("ok - a result row is activatable and its click is delivered\n");
}

/* A one pixel PNG, so the decode path has something that really is an
 * image without a file to carry around. */
static const char *const one_pixel_png_base64 =
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4"
  "z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";

static void
test_icon_decode (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmp = g_dir_make_tmp ("nuvasha-ui-icon-XXXXXX", &error);
  g_autofree char *bad_path = NULL;
  g_autofree char *good_path = NULL;
  g_autofree guchar *png = NULL;
  gsize png_len = 0;
  GtkWidget *icon = NULL;

  g_assert_no_error (error);
  g_assert_nonnull (tmp);

  bad_path = g_build_filename (tmp, "not-an-image.webp", NULL);
  good_path = g_build_filename (tmp, "one-pixel.png", NULL);

  g_assert_true (g_file_set_contents (bad_path, "this is not an image", -1,
                                      &error));
  g_assert_no_error (error);

  png = g_base64_decode (one_pixel_png_base64, &png_len);
  g_assert_true (g_file_set_contents (good_path, (const char *) png,
                                      png_len, &error));
  g_assert_no_error (error);

  icon = nuvasha_icon_widget_new ("image-x-generic-symbolic");

  /* A file that cannot be decoded leaves the placeholder in place. */
  g_assert_false (nuvasha_icon_widget_set_file (icon, bad_path));
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (icon)), ==,
                   "placeholder");

  /* A file that is an image is shown. */
  g_assert_true (nuvasha_icon_widget_set_file (icon, good_path));
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (icon)), ==,
                   "icon");

  /* What is shown is tied to the square: an icon whose source is larger
     than the square must not make its row taller, which is what the
     pixel size is for. */
  {
    GtkWidget *shown = gtk_stack_get_visible_child (GTK_STACK (icon));

    g_assert_true (GTK_IS_IMAGE (shown));
    g_assert_cmpint (gtk_image_get_pixel_size (GTK_IMAGE (shown)), ==,
                     nuvasha_ICON_SIZE);
  }

  /* A missing answer changes nothing. */
  g_assert_false (nuvasha_icon_widget_set_file (icon, NULL));

  g_object_ref_sink (icon);
  g_object_unref (icon);

  g_unlink (bad_path);
  g_unlink (good_path);
  g_rmdir (tmp);

  g_print ("ok - an undecodable icon keeps the placeholder\n");
}

/* The first widget of @type under @w, or NULL. */
static GtkWidget *
find_by_type (GtkWidget *w, GType type)
{
  if (w == NULL)
    return NULL;

  if (G_TYPE_CHECK_INSTANCE_TYPE (w, type))
    return w;

  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
       c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_by_type (c, type);

      if (found != NULL)
        return found;
    }

  return NULL;
}

/* How many widgets of @type are under @w. */
static guint
count_by_type (GtkWidget *w, GType type)
{
  guint n = 0;

  if (w == NULL)
    return 0;

  if (G_TYPE_CHECK_INSTANCE_TYPE (w, type))
    n++;

  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
       c = gtk_widget_get_next_sibling (c))
    n += count_by_type (c, type);

  return n;
}

/* The image of @icon_name under @w. */
static GtkWidget *
find_by_icon (GtkWidget *w, const char *icon_name)
{
  if (w == NULL)
    return NULL;

  if (GTK_IS_IMAGE (w) &&
      g_strcmp0 (gtk_image_get_icon_name (GTK_IMAGE (w)), icon_name) == 0)
    return w;

  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
       c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_by_icon (c, icon_name);

      if (found != NULL)
        return found;
    }

  return NULL;
}

/* A page of a kind of content opens as the list alone: the project is the
 * other half of a split view, and is only filled when one is opened. This
 * is the state the split has to start in, and moving the split without
 * meaning to would leave two empty columns. The switcher is icons only,
 * because the bar it shares with the search row has no room for words. */
static void
test_content_page_starts_single (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmp = g_dir_make_tmp ("nuvasha-ui-split-XXXXXX", &error);
  g_autofree char *data_dir = NULL;
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  NuvashaInstance *instance = NULL;
  NuvashaContentPage *page = NULL;
  GtkWidget *split = NULL;
  GtkWidget *installed = NULL;

  g_assert_no_error (error);
  g_assert_nonnull (tmp);

  data_dir = g_build_filename (tmp, "data", NULL);
  manager = nuvasha_instance_manager_new (data_dir);
  instance = nuvasha_instance_manager_create (manager, "Split", NULL, "1.21.1",
                                          nuvasha_LOADER_FABRIC, "0.15.0",
                                          &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  page = nuvasha_content_page_new (instance, nuvasha_CONTENT_MOD);
  g_assert_nonnull (page);

  split = find_by_type (GTK_WIDGET (page), ADW_TYPE_NAVIGATION_SPLIT_VIEW);

  g_assert_nonnull (split);
  g_assert_true (adw_navigation_split_view_get_collapsed (
    ADW_NAVIGATION_SPLIT_VIEW (split)));
  g_assert_null (adw_navigation_split_view_get_content (
    ADW_NAVIGATION_SPLIT_VIEW (split)));

  /* The switcher is icons only: the two buttons carry an icon and no
     label, which is what keeps the row one line, and the Installed one
     is the one on. */
  g_assert_cmpuint (count_by_type (GTK_WIDGET (page), GTK_TYPE_TOGGLE_BUTTON),
                    ==, 2);
  installed = find_by_type (GTK_WIDGET (page), GTK_TYPE_TOGGLE_BUTTON);
  g_assert_nonnull (installed);
  g_assert_null (gtk_button_get_label (GTK_BUTTON (installed)));
  g_assert_nonnull (gtk_button_get_icon_name (GTK_BUTTON (installed)));
  g_assert_true (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (installed)));

  g_object_unref (instance);
  g_print ("ok - a content page opens as the list alone\n");
}

/* A page whose folder scan is still running when the page goes away must
 * not be touched by the answer. Local only: the scan reads a directory,
 * it never asks the network. */
static void
test_page_disposed_while_scanning (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmp = g_dir_make_tmp ("nuvasha-ui-page-XXXXXX", &error);
  g_autofree char *data_dir = NULL;
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  NuvashaInstance *instance = NULL;
  NuvashaContentPage *page = NULL;
  gint64 deadline = 0;

  g_assert_no_error (error);
  g_assert_nonnull (tmp);

  data_dir = g_build_filename (tmp, "data", NULL);
  manager = nuvasha_instance_manager_new (data_dir);
  instance = nuvasha_instance_manager_create (manager, "Dispose", NULL, "1.21.1",
                                          nuvasha_LOADER_FABRIC, "0.15.0",
                                          &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  /* Start the lazy scan before the page is dropped. */
  page = nuvasha_content_page_new (instance, nuvasha_CONTENT_MOD);
  nuvasha_content_page_reset_for_tab_switch (page);
  g_object_ref_sink (page);
  g_object_unref (page);

  /* Let the cancelled scan answer to a page that is no longer there. */
  deadline = g_get_monotonic_time () + 300 * G_TIME_SPAN_MILLISECOND;

  while (g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);

  g_object_unref (instance);
  g_print ("ok - a page disposed while scanning is not touched afterwards\n");
}

/* The foot of the list of a content page: the button that installs every
 * version the checkmarks have on, at the left of the row, with the number
 * of them on it. It is there from the start and has nothing to install
 * until something is picked, so it starts insensitive and bare. */
static void
test_pick_button_starts_empty (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmp = g_dir_make_tmp ("nuvasha-ui-pick-XXXXXX", &error);
  g_autofree char *data_dir = NULL;
  g_autoptr (NuvashaInstanceManager) manager = NULL;
  NuvashaInstance *instance = NULL;
  NuvashaContentPage *page = NULL;
  GtkWidget *icon = NULL;
  GtkWidget *button = NULL;
  GtkWidget *badge = NULL;

  g_assert_no_error (error);
  g_assert_nonnull (tmp);

  data_dir = g_build_filename (tmp, "data", NULL);
  manager = nuvasha_instance_manager_new (data_dir);
  instance = nuvasha_instance_manager_create (manager, "Pick", NULL, "1.21.1",
                                          nuvasha_LOADER_FABRIC, "0.15.0",
                                          &error);
  g_assert_no_error (error);
  g_assert_nonnull (instance);

  page = nuvasha_content_page_new (instance, nuvasha_CONTENT_MOD);
  g_assert_nonnull (page);

  /* The download icon of the foot is the button itself, and the number
     that stands over its corner is beside it: what lights up under the
     pointer is the icon and not the count with it. */
  icon = find_by_icon (GTK_WIDGET (page), "folder-download-symbolic");
  g_assert_nonnull (icon);

  button = gtk_widget_get_parent (icon);
  g_assert_true (GTK_IS_BUTTON (button));

  /* Nothing is picked: there is nothing to install and no number to
     show, which is what an insensitive button says on its own. */
  g_assert_false (gtk_widget_get_sensitive (button));

  badge = gtk_widget_get_parent (button);
  g_assert_true (GTK_IS_OVERLAY (badge));
  badge = find_by_type (badge, GTK_TYPE_LABEL);
  g_assert_nonnull (badge);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (badge)), ==, "");
  g_assert_false (gtk_widget_get_visible (badge));

  g_object_unref (instance);
  g_print ("ok - the button that installs what is selected starts empty\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Browser UI\n");

  if (!gtk_init_check ())
    {
      g_print ("skipped - no display, cannot build the widgets\n");
      return 77;
    }

  /* The markup audit counts through the structured log writer. */
  g_log_set_writer_func (count_markup_warning, NULL, NULL);

  test_markup ();
  test_row_activation ();
  test_icon_decode ();
  test_feedback_ignores_unattached_pages ();
  test_content_page_starts_single ();
  test_page_disposed_while_scanning ();
  test_pick_button_starts_empty ();

  return EXIT_SUCCESS;
}
