/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_LINE_READER_H
#define AML_LINE_READER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef void (*AmlLineReaderLineFunc) (const char *line,
                                       gboolean    stderr_line,
                                       gpointer    user_data);
typedef void (*AmlLineReaderDoneFunc) (GError  *error,
                                       gpointer user_data);

typedef struct _AmlLineReader AmlLineReader;

/* The reader owns @stream and @user_data until EOF or an error. It calls
 * @done and frees itself after the stream ends. */
AmlLineReader *aml_line_reader_new (GInputStream          *stream,
                                    gboolean               stderr_line,
                                    AmlLineReaderLineFunc  line,
                                    AmlLineReaderDoneFunc  done,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free);

void aml_line_reader_start (AmlLineReader *reader);

G_END_DECLS

#endif /* AML_LINE_READER_H */
