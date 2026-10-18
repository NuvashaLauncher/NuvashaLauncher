/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_LINE_READER_H
#define nuvasha_LINE_READER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef void (*NuvashaLineReaderLineFunc) (const char *line,
                                       gboolean    stderr_line,
                                       gpointer    user_data);
typedef void (*NuvashaLineReaderDoneFunc) (GError  *error,
                                       gpointer user_data);

typedef struct _NuvashaLineReader NuvashaLineReader;

/* The reader owns @stream and @user_data until EOF or an error. It calls
 * @done and frees itself after the stream ends. */
NuvashaLineReader *nuvasha_line_reader_new (GInputStream          *stream,
                                    gboolean               stderr_line,
                                    NuvashaLineReaderLineFunc  line,
                                    NuvashaLineReaderDoneFunc  done,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free);

void nuvasha_line_reader_start (NuvashaLineReader *reader);

G_END_DECLS

#endif /* nuvasha_LINE_READER_H */
