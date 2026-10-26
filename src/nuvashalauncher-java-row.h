/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* nuvashalauncherJavaRow: Java picker for an instance's Settings page. It loads
 * detected runtimes and provides an entry for choosing a custom executable.
 * get_selected_path() returns NULL while loading or failed, an empty string
 * for the automatic entry, and the executable path otherwise. */

#ifndef nuvashalauncher_JAVA_ROW_H
#define nuvashalauncher_JAVA_ROW_H

#include <adwaita.h>

#include "nuvashalauncher-java.h"

G_BEGIN_DECLS

#define nuvashalauncher_TYPE_JAVA_ROW (nuvashalauncher_java_row_get_type ())

G_DECLARE_FINAL_TYPE (nuvashalauncherJavaRow, nuvashalauncher_java_row, nuvashalauncher, JAVA_ROW,
                      AdwComboRow)

nuvashalauncherJavaRow *nuvashalauncher_java_row_new (void);

 /* The Java major version required by the instance. When the exact
  * @required_major is detected, it becomes the automatic choice; otherwise
  * the best detected runtime is shown while the launch remains automatic.
  * Set before or after load(); a change after load() fills the list again. */
void nuvashalauncher_java_row_set_required_major (nuvashalauncherJavaRow *self, int required_major);

/* Fetches the detected installations and puts them in the list,
 * followed by whatever custom path @wanted names when detection did not
 * report it. @wanted == NULL leaves the automatic runtime selected. Loading
 * again cancels the running load. */
void nuvashalauncher_java_row_load (nuvashalauncherJavaRow *self, const char *wanted);

 /* Detected installations, borrowed; NULL until the row loads.
 * The array and its elements stay alive with the row. */
GPtrArray *nuvashalauncher_java_row_get_detected (nuvashalauncherJavaRow *self);

/* TRUE once the list finished filling. */
gboolean nuvashalauncher_java_row_get_ready (nuvashalauncherJavaRow *self);

/* NULL while the row is loading or failed. "" for the first entry (the
 * automatic choice), the executable path for everything else. */
const char *nuvashalauncher_java_row_get_selected_path (nuvashalauncherJavaRow *self);

/* Selects the entry holding @path; an empty or NULL @path selects the
 * automatic entry when one exists, otherwise it clears the selection. */
void nuvashalauncher_java_row_select_path (nuvashalauncherJavaRow *self, const char *path);

/* Adds an entry for @path that detection did not report and selects it.
 * @probed may be NULL: the title then shows the raw path. *Does not
 * start another detection scan*. */
void nuvashalauncher_java_row_set_custom (nuvashalauncherJavaRow *self,
                              const char *path,
                              nuvashalauncherJava    *probed);

G_END_DECLS

#endif /* nuvashalauncher_JAVA_ROW_H */
