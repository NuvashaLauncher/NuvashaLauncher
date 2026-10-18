/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_LINE_READER_H
#define leppoisa_LINE_READER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef void (*LeppoisaLineReaderLineFunc) (const char *line,
                                       gboolean    stderr_line,
                                       gpointer    user_data);
typedef void (*LeppoisaLineReaderDoneFunc) (GError  *error,
                                       gpointer user_data);

typedef struct _LeppoisaLineReader LeppoisaLineReader;

/* The reader owns @stream and @user_data until EOF or an error. It calls
 * @done and frees itself after the stream ends. */
LeppoisaLineReader *leppoisa_line_reader_new (GInputStream          *stream,
                                    gboolean               stderr_line,
                                    LeppoisaLineReaderLineFunc  line,
                                    LeppoisaLineReaderDoneFunc  done,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free);

void leppoisa_line_reader_start (LeppoisaLineReader *reader);

G_END_DECLS

#endif /* leppoisa_LINE_READER_H */
