/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyLogsPage: what a running game has to say.
 *
 * One page of the instance view that follows one ComfyLauncher: the state
 * of the launch and its detail, the buttons that clear the log and open
 * the folder the game writes its own log into, and the lines themselves
 * as they arrive. The launcher does the timestamping, the tagging and
 * the trimming; this is only what is shown of it.
 *
 * The view is read only and stays at the end of the log as long as the
 * user has not scrolled up to read something. */

#ifndef comfy_LOGS_PAGE_H
#define comfy_LOGS_PAGE_H

#include <adwaita.h>

#include "comfy-launcher.h"

G_BEGIN_DECLS

#define comfy_TYPE_LOGS_PAGE (comfy_logs_page_get_type ())

G_DECLARE_FINAL_TYPE (ComfyLogsPage, comfy_logs_page, comfy, LOGS_PAGE, GtkBox)

/* The page of @launcher. It follows that launcher, so two views of the
 * same instance show the same lines. */
ComfyLogsPage *comfy_logs_page_new (ComfyLauncher *launcher);

G_END_DECLS

#endif /* comfy_LOGS_PAGE_H */
