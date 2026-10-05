/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlMainWindow: the one window of the launcher. It shows the instances
 * of its AmlInstanceManager and holds the header bar actions. */

#ifndef AML_MAIN_WINDOW_H
#define AML_MAIN_WINDOW_H

#include <adwaita.h>

#include "aml-instance-manager.h"

G_BEGIN_DECLS

#define AML_TYPE_MAIN_WINDOW (aml_main_window_get_type ())

G_DECLARE_FINAL_TYPE (AmlMainWindow, aml_main_window, AML, MAIN_WINDOW,
                      AdwApplicationWindow)

/* Shows the instances of @manager and follows the manager's store. The
 * window takes its own reference on @manager until it is destroyed. */
AmlMainWindow *aml_main_window_new (GtkApplication      *application,
                                    AmlInstanceManager *manager);

G_END_DECLS

#endif /* AML_MAIN_WINDOW_H */
