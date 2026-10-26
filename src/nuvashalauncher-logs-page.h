/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherLogsPage: output from a running game.
 *
 * One page of the instance view that follows one nuvashalauncher: the state
 * of the launch and its detail, the buttons that clear the log and open
 * the folder the game writes its own log into, and the lines themselves
 * as they arrive. The launcher does the timestamping, the tagging and
 * the trimming; this is only what is shown of it.
 *
 * The view is read only and stays at the end of the log as long as the
 * user has not scrolled up to read something. */

#ifndef nuvashalauncher_LOGS_PAGE_H
#define nuvashalauncher_LOGS_PAGE_H

#include <adwaita.h>

#include "nuvashalauncher-launcher.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_LOGS_PAGE (nuvashalauncher_logs_page_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherLogsPage, nuvashalauncher_logs_page, nuvashalauncher, LOGS_PAGE, GtkBox)

/* The page of @launcher. It follows that launcher, so two views of the
 * same instance show the same lines. */
nuvashalauncherLogsPage *nuvashalauncher_logs_page_new (nuvashalauncher *launcher);

G_END_DECLS

#endif /* nuvashalauncher_LOGS_PAGE_H */
