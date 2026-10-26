/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherMainWindow: the one window of the launcher. It shows the instances
 * of its nuvashalauncherInstanceManager and holds the header bar actions. */

#ifndef nuvashalauncher_MAIN_WINDOW_H
#define nuvashalauncher_MAIN_WINDOW_H

#include <adwaita.h>

#include "nuvashalauncher-instance-manager.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_MAIN_WINDOW (nuvashalauncher_main_window_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherMainWindow, nuvashalauncher_main_window, nuvashalauncher, MAIN_WINDOW,
                      AdwApplicationWindow)

/* Shows the instances of @manager and follows the manager's store. The
 * window takes its own reference on @manager until it is destroyed. */
nuvashalauncherMainWindow *nuvashalauncher_main_window_new (AdwApplication      *application,
                                    nuvashalauncherInstanceManager *manager);

/* Starts an instance by its stable ID without presenting the window. */
gboolean nuvashalauncher_main_window_launch_instance (nuvashalauncherMainWindow *self,
                                              const char *instance_id,
                                              GError **error);
gboolean nuvashalauncher_main_window_launch_instance_as (nuvashalauncherMainWindow *self,
                                                const char *instance_id,
                                                const char *account_id,
                                                GError **error);

/* Starts the normal import flow for a local .mrpack file. */
void nuvashalauncher_main_window_import_file (nuvashalauncherMainWindow *self,
                                      const char *path);

G_END_DECLS

#endif /* nuvashalauncher_MAIN_WINDOW_H */
