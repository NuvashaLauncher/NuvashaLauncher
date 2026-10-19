/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLogsPage: output from a running game.
 *
 * One page of the instance view that follows one NuvashaLauncher: the state
 * of the launch and its detail, the buttons that clear the log and open
 * the folder the game writes its own log into, and the lines themselves
 * as they arrive. The launcher does the timestamping, the tagging and
 * the trimming; this is only what is shown of it.
 *
 * The view is read only and stays at the end of the log as long as the
 * user has not scrolled up to read something. */

#ifndef nuvasha_LOGS_PAGE_H
#define nuvasha_LOGS_PAGE_H

#include <adwaita.h>

#include "nuvasha-launcher.h"

G_BEGIN_DECLS

#define nuvasha_TYPE_LOGS_PAGE (nuvasha_logs_page_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLogsPage, nuvasha_logs_page, nuvasha, LOGS_PAGE, GtkBox)

/* The page of @launcher. It follows that launcher, so two views of the
 * same instance show the same lines. */
NuvashaLogsPage *nuvasha_logs_page_new (NuvashaLauncher *launcher);

G_END_DECLS

#endif /* nuvasha_LOGS_PAGE_H */
