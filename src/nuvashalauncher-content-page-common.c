/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-content-page-private.h"
#include "nuvashalauncher-ui-feedback.h"
#include "nuvashalauncher-operation.h"

PageWatch *
page_watch_new (nuvashalauncherContentPage *self)
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

  if (watch->operation != NULL)
    nuvashalauncher_operation_unref (watch->operation);

  if (watch->page != NULL)
    g_object_remove_weak_pointer (G_OBJECT (watch->page),
                                  (gpointer *) &watch->page);

  g_free (watch);
}

/* Points @field at @widget and watches it: when the widget is destroyed
 * the field becomes NULL before anything can touch it. Used for the
 * widgets of the page of a project, which the page owns. */
void
watch_widget (gpointer field, GtkWidget *widget)
{
  GtkWidget **target = field;

  if (*target != NULL)
    g_object_remove_weak_pointer (G_OBJECT (*target), field);

  *target = widget;

  if (widget != NULL)
    g_object_add_weak_pointer (G_OBJECT (widget), field);
}

void
show_toast (nuvashalauncherContentPage *self, const char *message)
{
  nuvashalauncher_ui_show_toast (GTK_WIDGET (self), GTK_WIDGET (self->toasts), message);
}

GtkWidget *
plain_label (const char *text)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (text != NULL ? text : ""));

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);

  return GTK_WIDGET (label);
}

void
on_banner_dismiss (AdwBanner *banner G_GNUC_UNUSED, gpointer user_data)
{
  nuvashalauncherContentPage *self = nuvashalauncher_CONTENT_PAGE (user_data);

  if (self->banner != NULL)
    adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
}

void
set_busy (nuvashalauncherContentPage *self, gboolean busy)
{
  if (self->browse.spinner != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->browse.spinner), busy);
}
