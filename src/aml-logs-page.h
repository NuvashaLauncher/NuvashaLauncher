/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlLogsPage: what a running game has to say.
 *
 * One page of the instance view that follows one AmlLauncher: the state
 * of the launch and its detail, the buttons that clear the log and open
 * the folder the game writes its own log into, and the lines themselves
 * as they arrive. The launcher does the timestamping, the tagging and
 * the trimming; this is only what is shown of it.
 *
 * The view is read only and stays at the end of the log as long as the
 * user has not scrolled up to read something. */

#ifndef AML_LOGS_PAGE_H
#define AML_LOGS_PAGE_H

#include <adwaita.h>

#include "aml-launcher.h"

G_BEGIN_DECLS

#define AML_TYPE_LOGS_PAGE (aml_logs_page_get_type ())

G_DECLARE_FINAL_TYPE (AmlLogsPage, aml_logs_page, AML, LOGS_PAGE, GtkBox)

/* The page of @launcher. It follows that launcher, so two views of the
 * same instance show the same lines. */
AmlLogsPage *aml_logs_page_new (AmlLauncher *launcher);
void aml_logs_page_focus_search (AmlLogsPage *self);

G_END_DECLS

#endif /* AML_LOGS_PAGE_H */
