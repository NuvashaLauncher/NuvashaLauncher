/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlJavaRow: the Java picker, used by an instance's Settings page.
 * Like AmlVersionRow it shows when it is fetching, what it failed to
 * find, how to try again, and which installations exist once the scan
 * is there. The list ends in an entry that asks for another runtime:
 * picking it opens the file chooser, adds what comes back and selects
 * it, so a runtime the scan did not report is added from the row
 * itself. The public contract of get_selected_path(): NULL while
 * loading, after a failure, while the ask is picked, or when no runtime is
 * selected; "" while the automatic entry is picked; the executable path
 * otherwise. The row can remain visually blank while GTK internally holds
 * its first model item to keep the dropdown available. */

#ifndef AML_JAVA_ROW_H
#define AML_JAVA_ROW_H

#include <adwaita.h>

#include "aml-java.h"

G_BEGIN_DECLS

#define AML_TYPE_JAVA_ROW (aml_java_row_get_type ())

G_DECLARE_FINAL_TYPE (AmlJavaRow, aml_java_row, AML, JAVA_ROW,
                      AdwComboRow)

AmlJavaRow *aml_java_row_new (void);

 /* The Java version the instance's game needs. When the exact
  * @required_major is detected, it becomes the automatic choice; otherwise
  * no runtime is selected. Set before or after load(); a change after load()
  * fills the list again. */
void aml_java_row_set_required_major (AmlJavaRow *self, int required_major);

/* Fetches the detected installations and puts them in the list,
 * followed by whatever custom path @wanted names when detection did not
 * report it. @wanted == NULL leaves no explicit runtime selected unless an
 * exact automatic runtime exists. Loading again cancels the running load. */
void aml_java_row_load (AmlJavaRow *self, const char *wanted);

/* The detected installations list, borrowed; NULL until the row loads.
 * The array and its elements stay alive with the row. */
GPtrArray *aml_java_row_get_detected (AmlJavaRow *self);

/* TRUE once the list finished filling. */
gboolean aml_java_row_get_ready (AmlJavaRow *self);

/* NULL while the row is loading or failed. "" for the first entry (the
 * automatic choice), the executable path for everything else. */
const char *aml_java_row_get_selected_path (AmlJavaRow *self);

/* Selects the entry holding @path; an empty or NULL @path selects the
 * automatic entry when one exists, otherwise it clears the selection. */
void aml_java_row_select_path (AmlJavaRow *self, const char *path);

/* Adds an entry for @path that detection did not report and selects it.
 * @probed may be NULL: the title then shows the raw path. *Does not
 * start another detection scan*. */
void aml_java_row_set_custom (AmlJavaRow *self,
                              const char *path,
                              AmlJava    *probed);

G_END_DECLS

#endif /* AML_JAVA_ROW_H */
