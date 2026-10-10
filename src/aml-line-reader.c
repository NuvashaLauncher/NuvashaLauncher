/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-line-reader.h"

struct _AmlLineReader
{
  GDataInputStream *stream;
  gboolean stderr_line;
  AmlLineReaderLineFunc line;
  AmlLineReaderDoneFunc done;
  gpointer user_data;
  GDestroyNotify user_data_free;
};

static void
aml_line_reader_free (AmlLineReader *reader)
{
  g_clear_object (&reader->stream);

  if (reader->user_data_free != NULL)
    reader->user_data_free (reader->user_data);

  g_free (reader);
}

static void aml_line_reader_read (AmlLineReader *reader);

static void
aml_line_reader_read_ready (GObject *source,
                            GAsyncResult *result,
                            gpointer user_data)
{
  AmlLineReader *reader = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *line = g_data_input_stream_read_line_finish (
    G_DATA_INPUT_STREAM (source), result, NULL, &error);

  if (line != NULL)
    {
      reader->line (line, reader->stderr_line, reader->user_data);
      aml_line_reader_read (reader);
      return;
    }

  reader->done (error, reader->user_data);
  aml_line_reader_free (reader);
}

static void
aml_line_reader_read (AmlLineReader *reader)
{
  g_data_input_stream_read_line_async (reader->stream, G_PRIORITY_DEFAULT,
                                       NULL, aml_line_reader_read_ready,
                                       reader);
}

AmlLineReader *
aml_line_reader_new (GInputStream         *stream,
                     gboolean              stderr_line,
                     AmlLineReaderLineFunc line,
                     AmlLineReaderDoneFunc done,
                     gpointer              user_data,
                     GDestroyNotify        user_data_free)
{
  AmlLineReader *reader = g_new0 (AmlLineReader, 1);

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
aml_line_reader_start (AmlLineReader *reader)
{
  g_return_if_fail (reader != NULL);

  aml_line_reader_read (reader);
}
