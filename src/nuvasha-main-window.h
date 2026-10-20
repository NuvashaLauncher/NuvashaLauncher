/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaMainWindow: the one window of the launcher. It shows the instances
 * of its NuvashaInstanceManager and holds the header bar actions. */

#ifndef nuvasha_MAIN_WINDOW_H
#define nuvasha_MAIN_WINDOW_H

#include <adwaita.h>

#include "nuvasha-instance-manager.h"

G_BEGIN_DECLS

#define nuvasha_TYPE_MAIN_WINDOW (nuvasha_main_window_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaMainWindow, nuvasha_main_window, nuvasha, MAIN_WINDOW,
                      AdwApplicationWindow)

/* Shows the instances of @manager and follows the manager's store. The
 * window takes its own reference on @manager until it is destroyed. */
NuvashaMainWindow *nuvasha_main_window_new (AdwApplication      *application,
                                    NuvashaInstanceManager *manager);

G_END_DECLS

#endif /* nuvasha_MAIN_WINDOW_H */
