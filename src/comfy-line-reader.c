/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-line-reader.h"

struct _ComfyLineReader
{
  GDataInputStream *stream;
  gboolean stderr_line;
  ComfyLineReaderLineFunc line;
  ComfyLineReaderDoneFunc done;
  gpointer user_data;
  GDestroyNotify user_data_free;
};

static void
comfy_line_reader_free (ComfyLineReader *reader)
{
  g_clear_object (&reader->stream);

  if (reader->user_data_free != NULL)
    reader->user_data_free (reader->user_data);

  g_free (reader);
}

static void comfy_line_reader_read (ComfyLineReader *reader);

static void
comfy_line_reader_read_ready (GObject *source,
                            GAsyncResult *result,
                            gpointer user_data)
{
  ComfyLineReader *reader = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *line = g_data_input_stream_read_line_finish (
    G_DATA_INPUT_STREAM (source), result, NULL, &error);

  if (line != NULL)
    {
      reader->line (line, reader->stderr_line, reader->user_data);
      comfy_line_reader_read (reader);
      return;
    }

  reader->done (error, reader->user_data);
  comfy_line_reader_free (reader);
}

static void
comfy_line_reader_read (ComfyLineReader *reader)
{
  g_data_input_stream_read_line_async (reader->stream, G_PRIORITY_DEFAULT,
                                       NULL, comfy_line_reader_read_ready,
                                       reader);
}

ComfyLineReader *
comfy_line_reader_new (GInputStream         *stream,
                     gboolean              stderr_line,
                     ComfyLineReaderLineFunc line,
                     ComfyLineReaderDoneFunc done,
                     gpointer              user_data,
                     GDestroyNotify        user_data_free)
{
  ComfyLineReader *reader = g_new0 (ComfyLineReader, 1);

  reader->stream = g_data_input_stream_new (stream);
  reader->stderr_line = stderr_line;
  reader->line = line;
  reader->done = done;
  reader->user_data = user_data;
  reader->user_data_free = user_data_free;

  g_data_input_stream_set_newline_type (
    reader->stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

  return reader;
}

void
comfy_line_reader_start (ComfyLineReader *reader)
{
  g_return_if_fail (reader != NULL);

  comfy_line_reader_read (reader);
}
