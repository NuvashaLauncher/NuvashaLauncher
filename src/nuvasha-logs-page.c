/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-logs-page.h"
#include "nuvasha-i18n.h"

#include <gdk/gdkevents.h>

struct _NuvashaLogsPage
{
  GtkBox parent_instance;

  NuvashaLauncher *launcher;

  GtkLabel *state;
  GtkLabel *detail;
  GtkButton *clear;
  GtkButton *open;
  GtkTextView *view;
  GtkScrolledWindow *scroller;
  GtkSearchEntry *search_bar;
  GtkSearchEntry *search_entry;

  /* Number of rendered lines and whether the view follows the end. */
  guint rendered;
  gboolean following;

  /* Keep all lines; filtering only changes which lines are displayed. */
  GPtrArray *lines; /* char*, one per line, in order */
  gboolean filtered;
};

G_DEFINE_FINAL_TYPE (NuvashaLogsPage, nuvasha_logs_page, GTK_TYPE_BOX)

static void scroll_to_end (NuvashaLogsPage *self);
static void show_lines (NuvashaLogsPage *self);

/* Launcher lines carry internal stream tags and some Minecraft versions
 * emit Log4j XML records. Keep those details for diagnostics internally,
 * but show the readable message in the log view. */
static char *
display_line (const char *line)
{
  const char *body = NULL;
  const char *tag_start = NULL;
  const char *tag_end = NULL;
  g_autofree char *formatted = NULL;

  if (line == NULL)
    return NULL;

  formatted = g_strdup (line);

  if (strlen (line) >= 10 && line[2] == ':' && line[5] == ':' &&
      line[8] == ' ' && line[9] == '[')
    {
      tag_start = line + 9;
      tag_end = strchr (tag_start, ']');

      if (tag_end != NULL && tag_end[1] == ' ')
        {
          body = tag_end + 2;

          if (g_str_has_prefix (tag_start, "[stdout]"))
            {
              g_free (g_steal_pointer (&formatted));
              formatted = g_strdup_printf ("%.8s %s", line, body);
            }
          else if (g_str_has_prefix (tag_start, "[stderr]"))
            {
              g_free (g_steal_pointer (&formatted));
              formatted = g_strdup_printf ("%.8s [error] %s", line, body);
            }
        }
    }

  body = formatted;
  if (strlen (formatted) >= 10 && formatted[8] == ' ')
    body = formatted + 9;

  if (g_str_has_prefix (body, "<log4j:Event ") ||
      g_str_equal (body, "</log4j:Event>") ||
      g_str_equal (body, "</log4j:Message>"))
    return NULL;

  if (g_str_has_prefix (body, "<log4j:Message><![CDATA["))
    body += strlen ("<log4j:Message><![CDATA[");

  {
    const char *suffix = strstr (body, "]]></log4j:Message>");

    if (suffix != NULL)
      return g_strdup_printf ("%.8s %.*s", formatted, (int) (suffix - body),
                              body);
  }

  return g_strdup (formatted);
}

/* ---------------------------------------------------------- *
 * The lines
 *
 * The lines live in self->lines and the text view is only a picture of
 * them. That is what lets a search hide lines without losing any of
 * them: clearing the search puts them all back, and a line that arrives
 * while a search is on is kept even when it does not match.
 * ---------------------------------------------------------- */

/* Writes the lines that pass the current search into the view: one line
   per row and nothing else, so the view never grows an empty row at the
   end. A search that matches nothing leaves the view empty rather than
   showing the last line it happened to have. */
static void
show_lines (NuvashaLogsPage *self)
{
  GtkTextBuffer *buffer =
    gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->view));
  g_autofree char *needle = NULL;
  GString *built = NULL;
  guint shown = 0;

  if (self->search_entry != NULL)
    needle = g_strdup (gtk_editable_get_text (
                         GTK_EDITABLE (self->search_entry)));

  built = g_string_new (NULL);

  for (guint i = 0; i < self->lines->len; i++)
    {
       const char *line = g_ptr_array_index (self->lines, i);

      if (needle != NULL && *needle != '\0' &&
          strstr (line, needle) == NULL)
        continue;

      if (built->len > 0)
        g_string_append_c (built, '\n');

      g_string_append (built, line);
      shown++;
    }

  gtk_text_buffer_set_text (buffer, built->str, -1);
  self->rendered = shown;

  g_string_free (built, TRUE);
}

/* The whole log as it is now: what the page shows when it appears and
   after a clear. */
static void
reload_log (NuvashaLogsPage *self)
{
  g_autofree char *text = nuvasha_launcher_get_log_text (self->launcher);

  g_ptr_array_set_size (self->lines, 0);

  if (text != NULL && *text != '\0')
    {
      g_auto (GStrv) split = g_strsplit (text, "\n", -1);

      for (guint i = 0; split != NULL && split[i] != NULL; i++)
        {
          g_autofree char *line = display_line (split[i]);

          if (line != NULL && *line != '\0')
            g_ptr_array_add (self->lines, g_steal_pointer (&line));
        }
    }

  self->filtered = FALSE;
  show_lines (self);
  self->following = TRUE;
  scroll_to_end (self);
}

static void
append_line (NuvashaLogsPage *self, const char *line)
{
  g_autofree char *display = display_line (line);

  if (display == NULL || *display == '\0')
    return;

  line = display;
  guint limit = nuvasha_launcher_get_line_limit (self->launcher);
  gboolean removed = FALSE;

  g_ptr_array_add (self->lines, g_strdup (line));

  /* The launcher drops the oldest line when its log is full, so the page
     drops the same one and stays the same log. */
  while (self->lines->len > limit)
    {
      g_ptr_array_remove_index (self->lines, 0);
      removed = TRUE;
    }

  /* With no search on, only the tail of the view changed: appending it
     avoids rebuilding the whole buffer for every line. A filtered view
     still has to be rebuilt, because a new line may or may not match. */
  if (!self->filtered && self->view != NULL)
    {
      GtkTextBuffer *buffer =
        gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->view));
      GtkTextIter start;
      GtkTextIter end;

      if (removed)
        {
          gtk_text_buffer_get_start_iter (buffer, &start);
          gtk_text_buffer_get_iter_at_line (buffer, &end, 1);
          gtk_text_buffer_delete (buffer, &start, &end);
        }

      gtk_text_buffer_get_end_iter (buffer, &end);

      if (gtk_text_buffer_get_char_count (buffer) > 0)
        gtk_text_buffer_insert (buffer, &end, "\n", 1);

      gtk_text_buffer_get_end_iter (buffer, &end);
      gtk_text_buffer_insert (buffer, &end, line, -1);

      self->rendered = self->lines->len;

      if (self->following)
        scroll_to_end (self);

      return;
    }

  show_lines (self);

  if (self->following)
    scroll_to_end (self);
}

/* ---------------------------------------------------------- *
 * Searching the lines
 * ---------------------------------------------------------- */

/* What is typed only ever changes what the view shows. Nothing is
   removed from self->lines, so an empty entry brings back every line
   there ever was. */
static void
on_search_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);
  gboolean had_search = self->filtered;

  self->filtered =
    self->search_entry != NULL &&
    *gtk_editable_get_text (GTK_EDITABLE (self->search_entry)) != '\0';

  show_lines (self);

  /* Coming out of a search the end is not where the view was left, so it
     is scrolled there again: after clearing a search the user wants the
     live tail, not the one line they had found. */
  if (had_search && !self->filtered)
    {
      self->following = TRUE;
      scroll_to_end (self);
    }
}

/* Escape means "forget what I typed". This bar can be closed as well, so
 * Escape does both and the focus goes back to the page: a bar closed with
 * words still in it looks like a search that is still on.
 *
 * GTK4 has no key-press-event signal and no stop-searching signal on
 * GtkSearchEntry, so the key is taken with a controller of its own. It is
 * added after the one inside the search entry and both run on the bubble
 * phase, so this one is asked first. */
static gboolean
on_search_key (GtkEventControllerKey *controller G_GNUC_UNUSED, guint keyval,
               guint keycode G_GNUC_UNUSED, GdkModifierType state
                 G_GNUC_UNUSED,
               gpointer user_data)
{
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);

  if (keyval == GDK_KEY_f && (state & GDK_CONTROL_MASK))
    {
      gtk_widget_set_visible (GTK_WIDGET (self->search_bar), TRUE);
      gtk_widget_grab_focus (GTK_WIDGET (self->search_entry));
      return GDK_EVENT_STOP;
    }

  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;

  if (*gtk_editable_get_text (GTK_EDITABLE (self->search_entry)) != '\0')
    {
      gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
      self->filtered = FALSE;
      show_lines (self);
      self->following = TRUE;
      scroll_to_end (self);
    }

  gtk_widget_set_visible (GTK_WIDGET (self->search_bar), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self->search_entry));
  gtk_widget_grab_focus (GTK_WIDGET (self));

  return GDK_EVENT_STOP;
}

/* ---------------------------------------------------------- *
 * The state line and the buttons
 * ---------------------------------------------------------- */

static void
update_controls (NuvashaLogsPage *self)
{
  g_autofree char *directory =
    nuvasha_launcher_get_log_directory (self->launcher);
  const char *detail = nuvasha_launcher_get_state_detail (self->launcher);

  /* A game that ended by itself says nothing here: "Stopped" is the
     resting state of the page, not a piece of news. */
  gtk_label_set_text (GTK_LABEL (self->state), "");
  gtk_label_set_text (GTK_LABEL (self->detail), detail != NULL ? detail : "");

  /* Offering to open a folder that is not there is offering something
     that cannot work. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->open),
                            g_file_test (directory, G_FILE_TEST_IS_DIR));
}

static void
on_clear_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);

  nuvasha_launcher_clear_log (self->launcher);
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
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);
  g_autofree char *directory =
    nuvasha_launcher_get_log_directory (self->launcher);
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
on_state_changed (NuvashaLauncher *launcher G_GNUC_UNUSED,
                  NuvashaLaunchState state G_GNUC_UNUSED,
                  const char *detail G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);

  update_controls (self);
}

static void
on_output (NuvashaLauncher *launcher G_GNUC_UNUSED, const char *line,
           gboolean stderr G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);

  append_line (self, line);
}

/* ---------------------------------------------------------- *
 * Scrolling
 * ---------------------------------------------------------- */

static void
scroll_to_end (NuvashaLogsPage *self)
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
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);
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
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (user_data);

  scroll_to_end (self);
  g_signal_handlers_disconnect_by_data (self, self);
}

/* ---------------------------------------------------------- *
 * Building the page
 * ---------------------------------------------------------- */

/* One flat icon button. The tooltip and accessible name keep the action
   discoverable without taking room beside the log. */
static GtkWidget *
toolbar_button (const char *icon_name, const char *label, GCallback callback,
               gpointer user_data)
{
  GtkButton *button = GTK_BUTTON (gtk_button_new ());
  GtkImage *icon = GTK_IMAGE (gtk_image_new_from_icon_name (icon_name));

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 16);
  gtk_button_set_child (GTK_BUTTON (button), GTK_WIDGET (icon));
  gtk_widget_add_css_class (GTK_WIDGET (button), "flat");
  gtk_widget_set_tooltip_text (GTK_WIDGET (button), label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect (button, "clicked", callback, user_data);

  return GTK_WIDGET (button);
}

NuvashaLogsPage *
nuvasha_logs_page_new (NuvashaLauncher *launcher)
{
  NuvashaLogsPage *self;
  GtkWidget *row;
  GtkWidget *search_box;
  GtkWidget *actions;
  GtkWidget *card;
  GtkWidget *text;
  GtkAdjustment *adjustment;

  g_return_val_if_fail (nuvasha_IS_LAUNCHER (launcher), NULL);

  self = g_object_new (nuvasha_TYPE_LOGS_PAGE, NULL);
  self->launcher = g_object_ref (launcher);
  self->lines = g_ptr_array_new_with_free_func (g_free);

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* What the launch is doing, and the buttons that act on the log. */
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top (row, 6);
  gtk_widget_set_margin_bottom (row, 6);
  gtk_widget_set_margin_start (row, 12);
  gtk_widget_set_margin_end (row, 12);

  self->state = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->state), "heading");
  self->detail = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_ellipsize (GTK_LABEL (self->detail), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (self->detail), "dim-label");

  self->clear = GTK_BUTTON (toolbar_button ("edit-clear-symbolic", _("Clear"),
                                G_CALLBACK (on_clear_clicked), self));

  self->open = GTK_BUTTON (toolbar_button ("folder-symbolic", _("Open Folder"),
                               G_CALLBACK (on_open_clicked), self));
  actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->clear));
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->open));

  /* Searching the log: a bar over the lines, closed until Ctrl+F opens
   * it. The lines are kept either way, so nothing here can lose one. */
   self->search_bar = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
   self->search_entry = self->search_bar;
   gtk_widget_set_hexpand (GTK_WIDGET (self->search_entry), TRUE);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->search_entry),
                                         _("Search the log"));
    gtk_widget_set_visible (GTK_WIDGET (self->search_bar), TRUE);
  g_signal_connect (self->search_entry, "changed",
                    G_CALLBACK (on_search_changed), self);
  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_BUBBLE);
     gtk_widget_add_controller (GTK_WIDGET (self->search_entry), keys);
     g_signal_connect (keys, "key-pressed", G_CALLBACK (on_search_key), self);
   }
   {
     GtkEventController *keys = gtk_event_controller_key_new ();

     gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
     gtk_widget_add_controller (GTK_WIDGET (self), keys);
     g_signal_connect (keys, "key-pressed", G_CALLBACK (on_search_key), self);
   }

   gtk_widget_set_hexpand (GTK_WIDGET (self->search_bar), TRUE);
   search_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_hexpand (search_box, TRUE);
   gtk_box_append (GTK_BOX (search_box), GTK_WIDGET (self->search_bar));
   gtk_box_append (GTK_BOX (row), search_box);
   gtk_box_append (GTK_BOX (row), actions);
   gtk_widget_set_hexpand (row, TRUE);
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
  self->view = GTK_TEXT_VIEW (text);

  self->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), text);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scroller), TRUE);

  card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (card, "card");
  gtk_box_append (GTK_BOX (card), GTK_WIDGET (self->scroller));
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
nuvasha_logs_page_dispose (GObject *object)
{
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (object);

  /* The launcher outlives this page while a game is still running, so
     the connections go before anything else can arrive. */
  if (self->launcher != NULL)
    g_signal_handlers_disconnect_by_data (self->launcher, self);

  G_OBJECT_CLASS (nuvasha_logs_page_parent_class)->dispose (object);
}

static void
nuvasha_logs_page_finalize (GObject *object)
{
  NuvashaLogsPage *self = nuvasha_LOGS_PAGE (object);

  g_clear_object (&self->launcher);
  g_clear_pointer (&self->lines, g_ptr_array_unref);

  G_OBJECT_CLASS (nuvasha_logs_page_parent_class)->finalize (object);
}

static void
nuvasha_logs_page_class_init (NuvashaLogsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_logs_page_dispose;
  object_class->finalize = nuvasha_logs_page_finalize;
}

static void
nuvasha_logs_page_init (NuvashaLogsPage *self)
{
  /* Everything is built in nuvasha_logs_page_new(), which knows the launcher
     the page follows. */
  (void) self;
}
