/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef nuvashalauncher_FILTER_ENTRY_H
#define nuvashalauncher_FILTER_ENTRY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_FILTER_ENTRY (nuvashalauncher_filter_entry_get_type ())
G_DECLARE_FINAL_TYPE (nuvashalauncherFilterEntry, nuvashalauncher_filter_entry,
                      nuvashalauncher, FILTER_ENTRY, GtkEntry)

void nuvashalauncher_filter_entry_set_popover (nuvashalauncherFilterEntry *self,
                                              GtkPopover *popover,
                                              const char *label);

G_END_DECLS

#endif
