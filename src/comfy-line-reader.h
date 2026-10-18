/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_LINE_READER_H
#define comfy_LINE_READER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef void (*ComfyLineReaderLineFunc) (const char *line,
                                       gboolean    stderr_line,
                                       gpointer    user_data);
typedef void (*ComfyLineReaderDoneFunc) (GError  *error,
                                       gpointer user_data);

typedef struct _ComfyLineReader ComfyLineReader;

/* The reader owns @stream and @user_data until EOF or an error. It calls
 * @done and frees itself after the stream ends. */
ComfyLineReader *comfy_line_reader_new (GInputStream          *stream,
                                    gboolean               stderr_line,
                                    ComfyLineReaderLineFunc  line,
                                    ComfyLineReaderDoneFunc  done,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free);

void comfy_line_reader_start (ComfyLineReader *reader);

G_END_DECLS

#endif /* comfy_LINE_READER_H */
