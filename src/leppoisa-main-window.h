/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaMainWindow: the one window of the launcher. It shows the instances
 * of its LeppoisaInstanceManager and holds the header bar actions. */

#ifndef leppoisa_MAIN_WINDOW_H
#define leppoisa_MAIN_WINDOW_H

#include <adwaita.h>

#include "leppoisa-instance-manager.h"

G_BEGIN_DECLS

#define leppoisa_TYPE_MAIN_WINDOW (leppoisa_main_window_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaMainWindow, leppoisa_main_window, leppoisa, MAIN_WINDOW,
                      AdwApplicationWindow)

/* Shows the instances of @manager and follows the manager's store. The
 * window takes its own reference on @manager until it is destroyed. */
LeppoisaMainWindow *leppoisa_main_window_new (GtkApplication      *application,
                                    LeppoisaInstanceManager *manager);

G_END_DECLS

#endif /* leppoisa_MAIN_WINDOW_H */
