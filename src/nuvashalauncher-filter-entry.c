/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "nuvashalauncher-filter-entry.h"
#include <glib/gi18n.h>

struct _nuvashalauncherFilterEntry
{
  GtkEntry parent_instance;
  GtkPopover *popover;
};

G_DEFINE_TYPE (nuvashalauncherFilterEntry, nuvashalauncher_filter_entry, GTK_TYPE_ENTRY)

static void
update_clear_icon (GtkEditable *editable, gpointer user_data G_GNUC_UNUSED)
{
  gtk_entry_set_icon_from_icon_name (GTK_ENTRY (editable), GTK_ENTRY_ICON_SECONDARY,
    *gtk_editable_get_text (editable) != '\0' ? "edit-clear-symbolic" : NULL);
}

static void
show_filters (nuvashalauncherFilterEntry *self)
{
  GdkRectangle rect;

  if (self->popover == NULL)
    return;

  gtk_entry_get_icon_area (GTK_ENTRY (self), GTK_ENTRY_ICON_PRIMARY, &rect);
  gtk_popover_set_pointing_to (self->popover, &rect);
  gtk_popover_popup (self->popover);
}

static void
on_icon_pressed (GtkEntry *entry, GtkEntryIconPosition position,
                 gpointer user_data G_GNUC_UNUSED)
{
  if (position == GTK_ENTRY_ICON_PRIMARY)
    show_filters (nuvashalauncher_FILTER_ENTRY (entry));
  else
    {
      gtk_editable_set_text (GTK_EDITABLE (entry), "");
      gtk_widget_grab_focus (GTK_WIDGET (entry));
    }
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller G_GNUC_UNUSED,
                guint keyval, guint keycode G_GNUC_UNUSED,
                GdkModifierType state, gpointer user_data)
{
  nuvashalauncherFilterEntry *self = user_data;

  if (keyval == GDK_KEY_F4 ||
      (keyval == GDK_KEY_Down && (state & GDK_ALT_MASK) != 0))
    show_filters (self);
  else if (keyval == GDK_KEY_Escape)
    gtk_editable_set_text (GTK_EDITABLE (self), "");
  else
    return GDK_EVENT_PROPAGATE;

  return GDK_EVENT_STOP;
}

static void
nuvashalauncher_filter_entry_dispose (GObject *object)
{
  nuvashalauncherFilterEntry *self = nuvashalauncher_FILTER_ENTRY (object);

  if (self->popover != NULL)
    {
      gtk_widget_unparent (GTK_WIDGET (self->popover));
      self->popover = NULL;
    }

  G_OBJECT_CLASS (nuvashalauncher_filter_entry_parent_class)->dispose (object);
}

static void
nuvashalauncher_filter_entry_size_allocate (GtkWidget *widget,
                                           int width, int height, int baseline)
{
  nuvashalauncherFilterEntry *self = nuvashalauncher_FILTER_ENTRY (widget);

  GTK_WIDGET_CLASS (nuvashalauncher_filter_entry_parent_class)->size_allocate (
    widget, width, height, baseline);
  if (self->popover != NULL)
    gtk_popover_present (self->popover);
}

static void
nuvashalauncher_filter_entry_class_init (nuvashalauncherFilterEntryClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = nuvashalauncher_filter_entry_dispose;
  GTK_WIDGET_CLASS (klass)->size_allocate = nuvashalauncher_filter_entry_size_allocate;
}

static void
nuvashalauncher_filter_entry_init (nuvashalauncherFilterEntry *self)
{
  GtkEventController *keys = gtk_event_controller_key_new ();

  gtk_entry_set_icon_from_icon_name (GTK_ENTRY (self), GTK_ENTRY_ICON_PRIMARY,
                                     "view-more-symbolic");
  gtk_entry_set_icon_activatable (GTK_ENTRY (self), GTK_ENTRY_ICON_PRIMARY, TRUE);
  gtk_entry_set_icon_activatable (GTK_ENTRY (self), GTK_ENTRY_ICON_SECONDARY, TRUE);
  gtk_entry_set_icon_tooltip_text (GTK_ENTRY (self), GTK_ENTRY_ICON_SECONDARY,
                                   _("Clear"));
  g_signal_connect (self, "changed", G_CALLBACK (update_clear_icon), NULL);
  g_signal_connect (self, "icon-press", G_CALLBACK (on_icon_pressed), NULL);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

void
nuvashalauncher_filter_entry_set_popover (nuvashalauncherFilterEntry *self,
                                         GtkPopover *popover,
                                         const char *label)
{
  g_return_if_fail (nuvashalauncher_IS_FILTER_ENTRY (self));
  g_return_if_fail (GTK_IS_POPOVER (popover));
  g_return_if_fail (self->popover == NULL);

  self->popover = popover;
  gtk_widget_set_parent (GTK_WIDGET (popover), GTK_WIDGET (self));
  gtk_entry_set_icon_tooltip_text (GTK_ENTRY (self), GTK_ENTRY_ICON_PRIMARY, label);
}
