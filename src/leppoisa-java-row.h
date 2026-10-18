/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaJavaRow: the Java picker, used by an instance's Settings page.
 * Like LeppoisaVersionRow it shows when it is fetching, what it failed to
 * find, how to try again, and which installations exist once the scan
 * is there. The list ends in an entry that asks for another runtime:
 * picking it opens the file chooser, adds what comes back and selects
 * it, so a runtime the scan did not report is added from the row
 * itself. The public contract of get_selected_path(): NULL while
 * loading, after a failure, while the ask is picked, or when no runtime is
 * selected; "" while the automatic entry is picked; the executable path
 * otherwise. */

#ifndef leppoisa_JAVA_ROW_H
#define leppoisa_JAVA_ROW_H

#include <adwaita.h>

#include "leppoisa-java.h"

G_BEGIN_DECLS

#define leppoisa_TYPE_JAVA_ROW (leppoisa_java_row_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaJavaRow, leppoisa_java_row, leppoisa, JAVA_ROW,
                      AdwComboRow)

LeppoisaJavaRow *leppoisa_java_row_new (void);

 /* The Java version the instance's game needs. When the exact
  * @required_major is detected, it becomes the automatic choice; otherwise
  * the best detected runtime is shown while the launch remains automatic.
  * Set before or after load(); a change after load() fills the list again. */
void leppoisa_java_row_set_required_major (LeppoisaJavaRow *self, int required_major);

/* Fetches the detected installations and puts them in the list,
 * followed by whatever custom path @wanted names when detection did not
 * report it. @wanted == NULL leaves the automatic runtime selected. Loading
 * again cancels the running load. */
void leppoisa_java_row_load (LeppoisaJavaRow *self, const char *wanted);

/* The detected installations list, borrowed; NULL until the row loads.
 * The array and its elements stay alive with the row. */
GPtrArray *leppoisa_java_row_get_detected (LeppoisaJavaRow *self);

/* TRUE once the list finished filling. */
gboolean leppoisa_java_row_get_ready (LeppoisaJavaRow *self);

/* NULL while the row is loading or failed. "" for the first entry (the
 * automatic choice), the executable path for everything else. */
const char *leppoisa_java_row_get_selected_path (LeppoisaJavaRow *self);

/* Selects the entry holding @path; an empty or NULL @path selects the
 * automatic entry when one exists, otherwise it clears the selection. */
void leppoisa_java_row_select_path (LeppoisaJavaRow *self, const char *path);

/* Adds an entry for @path that detection did not report and selects it.
 * @probed may be NULL: the title then shows the raw path. *Does not
 * start another detection scan*. */
void leppoisa_java_row_set_custom (LeppoisaJavaRow *self,
                              const char *path,
                              LeppoisaJava    *probed);

G_END_DECLS

#endif /* leppoisa_JAVA_ROW_H */
