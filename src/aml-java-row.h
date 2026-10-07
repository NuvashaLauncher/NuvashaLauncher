/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlJavaRow: the Java picker, used by the main window Settings
 * dialog and by an instance's Settings page. Like AmlVersionRow it
 * shows when it is fetching, what it failed to find, how to try again,
 * and which installations exist once the scan is there. The public
 * contract of get_selected_path(): NULL while loading or after a
 * failure; "" while the first entry ("Default" / "Automatic") is
 * picked; the executable path otherwise. */

#ifndef AML_JAVA_ROW_H
#define AML_JAVA_ROW_H

#include <adwaita.h>

#include "aml-java.h"
#include "aml-settings.h"

G_BEGIN_DECLS

#define AML_TYPE_JAVA_ROW (aml_java_row_get_type ())

G_DECLARE_FINAL_TYPE (AmlJavaRow, aml_java_row, AML, JAVA_ROW,
                      AdwComboRow)

AmlJavaRow *aml_java_row_new (void);

/* The label of the first entry, explaining what it maps to. Set before
 * load(), a NULL/without sets "Default". */
void aml_java_row_set_default_entry_label (AmlJavaRow *self,
                                           const char *label);

/* The Java the default resolves to: the detected runtime that matches
 * @global_path, or, when there is none, the closest compatible one for
 * @required_major (0 means "not known yet", so nothing is resolved). The
 * first entry is named after it and that same runtime is taken out of
 * the detected list, so it is shown once, as the default. Set before or
 * after load(); a change after load() fills the list again. */
void aml_java_row_set_default_choice (AmlJavaRow *self,
                                      const char *global_path,
                                      int         required_major);

/* Fetches the detected installations and puts them in the list,
 * followed by whatever custom path @wanted names when detection did not
 * report it. @wanted == NULL selects the first entry. Loading again
 * cancels the running load. */
void aml_java_row_load (AmlJavaRow *self, const char *wanted);

/* The detected installations list, borrowed; NULL until the row loads.
 * The array and its elements stay alive with the row. */
GPtrArray *aml_java_row_get_detected (AmlJavaRow *self);

/* TRUE once the list finished filling. */
gboolean aml_java_row_get_ready (AmlJavaRow *self);

/* NULL while the row is loading or failed. "" for the first entry (the
 * global default), the executable path for everything else. */
const char *aml_java_row_get_selected_path (AmlJavaRow *self);

/* Selects the entry holding @path; an empty or NULL @path falls back to
 * the first entry. */
void aml_java_row_select_path (AmlJavaRow *self, const char *path);

/* Adds an entry for @path that detection did not report and selects it.
 * @probed may be NULL: the title then shows the raw path. *Does not
 * start another detection scan*. */
void aml_java_row_set_custom (AmlJavaRow *self,
                              const char *path,
                              AmlJava    *probed);

G_END_DECLS

#endif /* AML_JAVA_ROW_H */
