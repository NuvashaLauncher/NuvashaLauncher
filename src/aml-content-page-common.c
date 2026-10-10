/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-content-page-private.h"
#include "aml-ui-feedback.h"

PageWatch *
page_watch_new (AmlContentPage *self)
{
  PageWatch *watch = g_new0 (PageWatch, 1);

  watch->page = self;
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &watch->page);
  return watch;
}

void
page_watch_free (PageWatch *watch)
{
  if (watch == NULL)
    return;

  if (watch->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (watch->page),
                                  (gpointer *) &watch->page);

  g_free (watch);
}

/* Points @field at @widget and watches it: when the widget is destroyed
 * the field becomes NULL before anything can touch it. Used for the
 * widgets of the page of a project, which the page owns. */
void
watch_widget (GtkWidget **field, GtkWidget *widget)
{
  if (*field != NULL)
    g_object_remove_weak_pointer (G_OBJECT (*field), (gpointer *) field);

  *field = widget;

  if (widget != NULL)
    g_object_add_weak_pointer (G_OBJECT (widget), (gpointer *) field);
}

void
show_toast (AmlContentPage *self, const char *message)
{
  aml_ui_show_toast (GTK_WIDGET (self), self->toasts, message);
}

GtkWidget *
plain_label (const char *text)
{
  GtkWidget *label = gtk_label_new (text != NULL ? text : "");

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);

  return label;
}

void
on_banner_dismiss (AdwBanner *banner G_GNUC_UNUSED, gpointer user_data)
{
  AmlContentPage *self = user_data;

  if (self->banner != NULL)
    adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
}

void
set_busy (AmlContentPage *self, gboolean busy)
{
  if (self->browse.spinner != NULL)
    gtk_widget_set_visible (self->browse.spinner, busy);
}
