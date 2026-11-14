/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef NUVASHALAUNCHER_FILTER_ENTRY_H
#define NUVASHALAUNCHER_FILTER_ENTRY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NUVASHALAUNCHER_TYPE_FILTER_ENTRY (nuvashalauncher_filter_entry_get_type ())
G_DECLARE_FINAL_TYPE (NuvashaLauncherFilterEntry, nuvashalauncher_filter_entry,
                      NUVASHALAUNCHER, FILTER_ENTRY, GtkEntry)

void nuvashalauncher_filter_entry_set_popover (NuvashaLauncherFilterEntry *self,
                                              GtkPopover *popover,
                                              const char *label);

G_END_DECLS

#endif
