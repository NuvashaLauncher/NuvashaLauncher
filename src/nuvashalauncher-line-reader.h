/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_LINE_READER_H
#define nuvashalauncher_LINE_READER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef void (*NuvashaLauncherLineReaderLineFunc) (const char *line,
                                       gboolean    stderr_line,
                                       gpointer    user_data);
typedef void (*NuvashaLauncherLineReaderDoneFunc) (GError  *error,
                                       gpointer user_data);

typedef struct _NuvashaLauncherLineReader NuvashaLauncherLineReader;

/* The reader owns @stream and @user_data until EOF or an error. It calls
 * @done and frees itself after the stream ends. */
NuvashaLauncherLineReader *nuvashalauncher_line_reader_new (GInputStream          *stream,
                                    gboolean               stderr_line,
                                    NuvashaLauncherLineReaderLineFunc  line,
                                    NuvashaLauncherLineReaderDoneFunc  done,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free);

void nuvashalauncher_line_reader_start (NuvashaLauncherLineReader *reader);

G_END_DECLS

#endif /* nuvashalauncher_LINE_READER_H */
