/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyMainWindow: the one window of the launcher. It shows the instances
 * of its ComfyInstanceManager and holds the header bar actions. */

#ifndef comfy_MAIN_WINDOW_H
#define comfy_MAIN_WINDOW_H

#include <adwaita.h>

#include "comfy-instance-manager.h"

G_BEGIN_DECLS

#define comfy_TYPE_MAIN_WINDOW (comfy_main_window_get_type ())

G_DECLARE_FINAL_TYPE (ComfyMainWindow, comfy_main_window, comfy, MAIN_WINDOW,
                      AdwApplicationWindow)

/* Shows the instances of @manager and follows the manager's store. The
 * window takes its own reference on @manager until it is destroyed. */
ComfyMainWindow *comfy_main_window_new (GtkApplication      *application,
                                    ComfyInstanceManager *manager);

G_END_DECLS

#endif /* comfy_MAIN_WINDOW_H */
