/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstancePage: what the dialog of one instance shows.
 *
 * It is a dialog page, not a navigation page: the dialog is already
 * named after the instance and has its own close button, so there is no
 * back button here and nothing in it takes the user back anywhere.
 *
 * Its own top bar holds the view switcher over the five pages of the
 * instance: Settings, Resource Packs, Shader Packs, Mods and Logs. When
 * the page is put in an AdwDialog, that bar becomes the header of the
 * dialog. */

#ifndef AML_INSTANCE_PAGE_H
#define AML_INSTANCE_PAGE_H

#include <adwaita.h>

#include "aml-instance-manager.h"
#include "aml-launcher.h"
#include "aml-settings.h"

G_BEGIN_DECLS

#define AML_TYPE_INSTANCE_PAGE (aml_instance_page_get_type ())

/* A box holding the toolbar view of the instance. AdwToolbarView is a
 * final type and cannot be subclassed, so it is a child here rather than
 * the parent. */
G_DECLARE_FINAL_TYPE (AmlInstancePage, aml_instance_page, AML, INSTANCE_PAGE,
                      GtkBox)

/* Builds the page for @instance. @launcher is the launcher of that same
 * instance, which the Logs page follows. The page keeps its own
 * references on @manager, @instance, @settings and @launcher until it is
 * destroyed. Like every widget it starts with a floating reference:
 * adw_tab_view_append() takes that reference. */
AmlInstancePage *aml_instance_page_new (AmlInstanceManager *manager,
                                        AmlInstance       *instance,
                                        AmlSettings        *settings,
                                         AmlLauncher       *launcher);

/* Opens search for the currently selected content/logs tab. */
void aml_instance_page_focus_search (AmlInstancePage *self);

G_END_DECLS

#endif /* AML_INSTANCE_PAGE_H */
