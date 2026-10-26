/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherInstancePage: the contents of one instance dialog.
 *
 * It is a dialog page, not a navigation page: the dialog is already
 * named after the instance and has its own close button, so there is no
 * back button here and nothing in it takes the user back anywhere.
 *
 * Its own top bar holds the view switcher over the five pages of the
 * instance: Settings, Resource Packs, Shader Packs, Mods and Logs. When
 * the page is put in an AdwDialog, that bar becomes the header of the
 * dialog. */

#ifndef nuvashalauncher_INSTANCE_PAGE_H
#define nuvashalauncher_INSTANCE_PAGE_H

#include <adwaita.h>

#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-settings.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_INSTANCE_PAGE (nuvashalauncher_instance_page_get_type ())

/* A box holding the toolbar view of the instance. AdwToolbarView is a
 * final type and cannot be subclassed, so it is a child here rather than
 * the parent. */
G_DECLARE_FINAL_TYPE (nuvashalauncherInstancePage, nuvashalauncher_instance_page, nuvashalauncher, INSTANCE_PAGE,
                      GtkBox)

/* Builds the page for @instance. @launcher is the launcher of that same
 * instance, which the Logs page follows. The page keeps its own
 * references on @manager, @instance, @settings and @launcher until it is
 * destroyed. Like every widget it starts with a floating reference:
 * adw_tab_view_append() takes that reference. */
nuvashalauncherInstancePage *nuvashalauncher_instance_page_new (nuvashalauncherInstanceManager *manager,
                                         nuvashalauncherInstance       *instance,
                                         nuvashalauncherSettings        *settings,
                                         nuvashalauncher       *launcher);

/* Selects the instance's live log view. */
void nuvashalauncher_instance_page_show_logs (nuvashalauncherInstancePage *self);
/* Invoke the existing instance action flows from an external toolbar. */
void nuvashalauncher_instance_page_trigger_export (nuvashalauncherInstancePage *self);
void nuvashalauncher_instance_page_trigger_delete (nuvashalauncherInstancePage *self);
void nuvashalauncher_instance_page_trigger_duplicate (nuvashalauncherInstancePage *self);
void nuvashalauncher_instance_page_export_to_path (nuvashalauncherInstancePage *self,
                                           const char *path);

G_END_DECLS

#endif /* nuvashalauncher_INSTANCE_PAGE_H */
