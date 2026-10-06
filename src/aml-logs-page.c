/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-logs-page.h"

struct _AmlLogsPage
{
  GtkBox parent_instance;

  AmlLauncher *launcher;

  GtkWidget *state;
  GtkWidget *detail;
  GtkWidget *clear;
  GtkWidget *open;
  GtkWidget *view;
  GtkWidget *scroller;

  /* How many lines the text view holds, and whether the user is looking
     at the end of the log: a view that has been scrolled up to read
     something stays where it is. */
  guint rendered;
  gboolean following;
};

G_DEFINE_FINAL_TYPE (AmlLogsPage, aml_logs_page, GTK_TYPE_BOX)

static void scroll_to_end (AmlLogsPage *self);

/* ---------------------------------------------------------- *
 * The lines
 * ---------------------------------------------------------- */

/* The whole log as it is now: what the page shows when it appears and
   after a clear. New lines are appended instead of read back.

   The buffer holds one line per line of the log and nothing else, so a
   line is written after a newline and the view never grows an empty row
   at the end. */
static void
reload_log (AmlLogsPage *self)
{
  g_autofree char *text = aml_launcher_get_log_text (self->launcher);
  GtkTextBuffer *buffer =
    gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->view));
  guint lines = 0;

  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '\n')
        lines++;
    }

  if (text[0] != '\0')
    lines++;

  gtk_text_buffer_set_text (buffer, text, -1);
  self->rendered = lines;
  self->following = TRUE;
  scroll_to_end (self);
}

static void
append_line (AmlLogsPage *self, const char *line)
{
  GtkTextBuffer *buffer =
    gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->view));
  GtkTextIter start;
  GtkTextIter end;
  guint limit = aml_launcher_get_line_limit (self->launcher);

  gtk_text_buffer_get_start_iter (buffer, &start);
  gtk_text_buffer_get_end_iter (buffer, &end);

  /* One newline in front of every line but the first. */
  if (!gtk_text_iter_equal (&start, &end))
    {
      gtk_text_buffer_insert (buffer, &end, "\n", 1);
      gtk_text_buffer_get_end_iter (buffer, &end);
    }

  gtk_text_buffer_insert (buffer, &end, line, -1);
  self->rendered++;

  /* The launcher drops the oldest line when its log is full, so the view
     drops the same one and stays the same log. */
  while (self->rendered > limit)
    {
      GtkTextIter after;

      gtk_text_buffer_get_start_iter (buffer, &start);
      after = start;

      /* The whole first line, with the newline that ends it. */
      if (!gtk_text_iter_forward_to_line_end (&after) ||
          !gtk_text_iter_forward_char (&after))
        break;

      gtk_text_buffer_delete (buffer, &start, &after);
      self->rendered--;
    }

  if (self->following)
    scroll_to_end (self);
}

/* ---------------------------------------------------------- *
 * The state line and the buttons
 * ---------------------------------------------------------- */

static void
update_controls (AmlLogsPage *self)
{
  g_autofree char *directory =
    aml_launcher_get_log_directory (self->launcher);
  const char *detail = aml_launcher_get_state_detail (self->launcher);

  gtk_label_set_text (
    GTK_LABEL (self->state),
    aml_launch_state_to_string (aml_launcher_get_state (self->launcher)));
  gtk_label_set_text (GTK_LABEL (self->detail), detail != NULL ? detail : "");

  /* Offering to open a folder that is not there is offering something
     that cannot work. */
  gtk_widget_set_sensitive (self->open,
                            g_file_test (directory, G_FILE_TEST_IS_DIR));
}

static void
on_clear_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlLogsPage *self = user_data;

  aml_launcher_clear_log (self->launcher);
  reload_log (self);
}

static void
open_finished (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_default_for_uri_finish (result, &error))
    g_warning ("the log folder does not open: %s", error->message);

  /* The copy of the address the call needed. */
  g_free (user_data);
}

static void
on_open_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  AmlLogsPage *self = user_data;
  g_autofree char *directory =
    aml_launcher_get_log_directory (self->launcher);
  g_autofree char *uri = NULL;

  uri = g_filename_to_uri (directory, NULL, NULL);

  if (uri == NULL)
    return;

  /* GIO hands the folder to the desktop the way a double click in a file
     manager does: nothing goes through a shell. */
  g_app_info_launch_default_for_uri_async (uri, NULL, NULL, open_finished,
                                           g_strdup (uri));
}

/* ---------------------------------------------------------- *
 * Following the launcher
 * ---------------------------------------------------------- */

static void
on_state_changed (AmlLauncher *launcher G_GNUC_UNUSED,
                  AmlLaunchState state G_GNUC_UNUSED,
                  const char *detail G_GNUC_UNUSED, gpointer user_data)
{
  AmlLogsPage *self = user_data;

  update_controls (self);
}

static void
on_output (AmlLauncher *launcher G_GNUC_UNUSED, const char *line,
           gboolean stderr G_GNUC_UNUSED, gpointer user_data)
{
  AmlLogsPage *self = user_data;

  append_line (self, line);
}

/* ---------------------------------------------------------- *
 * Scrolling
 * ---------------------------------------------------------- */

static void
scroll_to_end (AmlLogsPage *self)
{
  GtkAdjustment *adjustment =
    gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroller));

  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_upper (adjustment) -
                                        gtk_adjustment_get_page_size (
                                          adjustment));
}

static void
on_adjustment_changed (GtkAdjustment *adjustment, gpointer user_data)
{
  AmlLogsPage *self = user_data;
  gdouble at_end;

  at_end = gtk_adjustment_get_upper (adjustment) -
           gtk_adjustment_get_page_size (adjustment) -
           gtk_adjustment_get_step_increment (adjustment);

  self->following = gtk_adjustment_get_value (adjustment) >= at_end;
}

/* The page is built before it is on the screen, so the log of a game
   that is already running can only be scrolled to its end the first time
   the page is mapped. After that the scroll belongs to the user. */
static void
on_first_map (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  AmlLogsPage *self = user_data;

  scroll_to_end (self);
  g_signal_handlers_disconnect_by_data (self, self);
}

/* ---------------------------------------------------------- *
 * Building the page
 * ---------------------------------------------------------- */

/* One flat button with a symbolic icon and a label, the way GNOME writes
   a toolbar action. */
static GtkWidget *
toolbar_button (const char *icon_name, const char *label, GCallback callback,
                gpointer user_data)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 16);
  gtk_box_append (GTK_BOX (content), icon);
  gtk_box_append (GTK_BOX (content), gtk_label_new (label));

  gtk_button_set_child (GTK_BUTTON (button), content);
  gtk_widget_add_css_class (button, "flat");
  g_signal_connect (button, "clicked", callback, user_data);

  return button;
}

AmlLogsPage *
aml_logs_page_new (AmlLauncher *launcher)
{
  AmlLogsPage *self;
  GtkWidget *row;
  GtkWidget *card;
  GtkWidget *text;
  GtkAdjustment *adjustment;

  g_return_val_if_fail (AML_IS_LAUNCHER (launcher), NULL);

  self = g_object_new (AML_TYPE_LOGS_PAGE, NULL);
  self->launcher = g_object_ref (launcher);

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* What the launch is doing, and the buttons that act on the log. */
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top (row, 6);
  gtk_widget_set_margin_bottom (row, 6);
  gtk_widget_set_margin_start (row, 12);
  gtk_widget_set_margin_end (row, 12);

  self->state = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->state, "heading");
  gtk_box_append (GTK_BOX (row), self->state);

  self->detail = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (self->detail), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->detail, TRUE);
  gtk_widget_add_css_class (self->detail, "dim-label");
  gtk_box_append (GTK_BOX (row), self->detail);

  self->clear = toolbar_button ("edit-clear-symbolic", "Clear",
                                G_CALLBACK (on_clear_clicked), self);
  gtk_box_append (GTK_BOX (row), self->clear);

  self->open = toolbar_button ("folder-open-symbolic", "Open Folder",
                               G_CALLBACK (on_open_clicked), self);
  gtk_box_append (GTK_BOX (row), self->open);

  gtk_box_append (GTK_BOX (self), row);

  /* The lines themselves. Read only: this is what the game printed,
     nothing to write into. */
  text = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (text), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (text), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (text), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (text), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (text), 12);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (text), 6);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (text), 6);
  self->view = text;

  self->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), text);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (self->scroller, TRUE);

  card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (card, "card");
  gtk_box_append (GTK_BOX (card), self->scroller);
  gtk_widget_set_margin_start (card, 12);
  gtk_widget_set_margin_end (card, 12);
  gtk_widget_set_margin_bottom (card, 12);
  gtk_widget_set_vexpand (card, TRUE);
  gtk_box_append (GTK_BOX (self), card);

  adjustment =
    gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroller));
  g_signal_connect (adjustment, "value-changed",
                    G_CALLBACK (on_adjustment_changed), self);

  g_signal_connect (self->launcher, "state", G_CALLBACK (on_state_changed),
                    self);
  g_signal_connect (self->launcher, "output", G_CALLBACK (on_output), self);

  update_controls (self);
  reload_log (self);
  g_signal_connect (self, "map", G_CALLBACK (on_first_map), self);

  return self;
}

static void
aml_logs_page_dispose (GObject *object)
{
  AmlLogsPage *self = AML_LOGS_PAGE (object);

  /* The launcher outlives this page while a game is still running, so
     the connections go before anything else can arrive. */
  if (self->launcher != NULL)
    g_signal_handlers_disconnect_by_data (self->launcher, self);

  G_OBJECT_CLASS (aml_logs_page_parent_class)->dispose (object);
}

static void
aml_logs_page_finalize (GObject *object)
{
  AmlLogsPage *self = AML_LOGS_PAGE (object);

  g_clear_object (&self->launcher);

  G_OBJECT_CLASS (aml_logs_page_parent_class)->finalize (object);
}

static void
aml_logs_page_class_init (AmlLogsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_logs_page_dispose;
  object_class->finalize = aml_logs_page_finalize;
}

static void
aml_logs_page_init (AmlLogsPage *self)
{
  /* Everything is built in aml_logs_page_new(), which knows the launcher
     the page follows. */
  (void) self;
}