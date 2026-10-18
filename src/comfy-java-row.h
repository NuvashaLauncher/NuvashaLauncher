/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyJavaRow: the Java picker, used by an instance's Settings page.
 * Like ComfyVersionRow it shows when it is fetching, what it failed to
 * find, how to try again, and which installations exist once the scan
 * is there. The list ends in an entry that asks for another runtime:
 * picking it opens the file chooser, adds what comes back and selects
 * it, so a runtime the scan did not report is added from the row
 * itself. The public contract of get_selected_path(): NULL while
 * loading, after a failure, while the ask is picked, or when no runtime is
 * selected; "" while the automatic entry is picked; the executable path
 * otherwise. */

#ifndef comfy_JAVA_ROW_H
#define comfy_JAVA_ROW_H

#include <adwaita.h>

#include "comfy-java.h"

G_BEGIN_DECLS

#define comfy_TYPE_JAVA_ROW (comfy_java_row_get_type ())

G_DECLARE_FINAL_TYPE (ComfyJavaRow, comfy_java_row, comfy, JAVA_ROW,
                      AdwComboRow)

ComfyJavaRow *comfy_java_row_new (void);

 /* The Java version the instance's game needs. When the exact
  * @required_major is detected, it becomes the automatic choice; otherwise
  * the best detected runtime is shown while the launch remains automatic.
  * Set before or after load(); a change after load() fills the list again. */
void comfy_java_row_set_required_major (ComfyJavaRow *self, int required_major);

/* Fetches the detected installations and puts them in the list,
 * followed by whatever custom path @wanted names when detection did not
 * report it. @wanted == NULL leaves the automatic runtime selected. Loading
 * again cancels the running load. */
void comfy_java_row_load (ComfyJavaRow *self, const char *wanted);

/* The detected installations list, borrowed; NULL until the row loads.
 * The array and its elements stay alive with the row. */
GPtrArray *comfy_java_row_get_detected (ComfyJavaRow *self);

/* TRUE once the list finished filling. */
gboolean comfy_java_row_get_ready (ComfyJavaRow *self);

/* NULL while the row is loading or failed. "" for the first entry (the
 * automatic choice), the executable path for everything else. */
const char *comfy_java_row_get_selected_path (ComfyJavaRow *self);

/* Selects the entry holding @path; an empty or NULL @path selects the
 * automatic entry when one exists, otherwise it clears the selection. */
void comfy_java_row_select_path (ComfyJavaRow *self, const char *path);

/* Adds an entry for @path that detection did not report and selects it.
 * @probed may be NULL: the title then shows the raw path. *Does not
 * start another detection scan*. */
void comfy_java_row_set_custom (ComfyJavaRow *self,
                              const char *path,
                              ComfyJava    *probed);

G_END_DECLS

#endif /* comfy_JAVA_ROW_H */
