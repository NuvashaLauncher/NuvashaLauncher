/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-line-reader.h"

#define READ_SIZE (64 * 1024)
#define MAX_LINE_SIZE (1024 * 1024)

struct _nuvashalauncherLineReader
{
  GInputStream *stream;
  gboolean stderr_line;
  nuvashalauncherLineReaderLineFunc line;
  nuvashalauncherLineReaderDoneFunc done;
  gpointer user_data;
  GDestroyNotify user_data_free;
  GByteArray *line_buffer;
  gboolean truncated;
};

static void
nuvashalauncher_line_reader_free (nuvashalauncherLineReader *reader)
{
  g_clear_object (&reader->stream);
  g_clear_pointer (&reader->line_buffer, g_byte_array_unref);

  if (reader->user_data_free != NULL)
    reader->user_data_free (reader->user_data);

  g_free (reader);
}

static void nuvashalauncher_line_reader_read (nuvashalauncherLineReader *reader);

static void
emit_line (nuvashalauncherLineReader *reader)
{
  g_autofree char *line = NULL;

  if (reader->line_buffer->len > 0 &&
      reader->line_buffer->data[reader->line_buffer->len - 1] == '\r')
    g_byte_array_set_size (reader->line_buffer, reader->line_buffer->len - 1);

  line = g_strndup ((const char *) reader->line_buffer->data,
                    reader->line_buffer->len);
  if (reader->truncated)
    {
      g_autofree char *limited = g_strconcat (line, " [line truncated]", NULL);
      reader->line (limited, reader->stderr_line, reader->user_data);
    }
  else
    reader->line (line, reader->stderr_line, reader->user_data);

  g_byte_array_set_size (reader->line_buffer, 0);
  reader->truncated = FALSE;
}

static void
consume_bytes (nuvashalauncherLineReader *reader, const guint8 *data, gsize size)
{
  for (gsize i = 0; i < size; i++)
    {
      if (data[i] == '\n')
        {
          emit_line (reader);
          continue;
        }

      if (reader->line_buffer->len < MAX_LINE_SIZE)
        g_byte_array_append (reader->line_buffer, &data[i], 1);
      else
        reader->truncated = TRUE;
    }
}

static void
nuvashalauncher_line_reader_read_ready (GObject *source,
                                        GAsyncResult *result,
                                        gpointer user_data)
{
  nuvashalauncherLineReader *reader = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
    g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (bytes == NULL)
    {
      reader->done (error, reader->user_data);
      nuvashalauncher_line_reader_free (reader);
      return;
    }

  if (g_bytes_get_size (bytes) == 0)
    {
      if (reader->line_buffer->len > 0 || reader->truncated)
        emit_line (reader);
      reader->done (NULL, reader->user_data);
      nuvashalauncher_line_reader_free (reader);
      return;
    }

  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  consume_bytes (reader, data, size);
  nuvashalauncher_line_reader_read (reader);
}

static void
nuvashalauncher_line_reader_read (nuvashalauncherLineReader *reader)
{
  g_input_stream_read_bytes_async (reader->stream, READ_SIZE, G_PRIORITY_DEFAULT,
                                   NULL, nuvashalauncher_line_reader_read_ready,
                                   reader);
}

nuvashalauncherLineReader *
nuvashalauncher_line_reader_new (GInputStream *stream,
                                 gboolean stderr_line,
                                 nuvashalauncherLineReaderLineFunc line,
                                 nuvashalauncherLineReaderDoneFunc done,
                                 gpointer user_data,
                                 GDestroyNotify user_data_free)
{
  nuvashalauncherLineReader *reader = g_new0 (nuvashalauncherLineReader, 1);

  reader->stream = g_object_ref (stream);
  reader->stderr_line = stderr_line;
  reader->line = line;
  reader->done = done;
  reader->user_data = user_data;
  reader->user_data_free = user_data_free;
  reader->line_buffer = g_byte_array_new ();

  return reader;
}

void
nuvashalauncher_line_reader_start (nuvashalauncherLineReader *reader)
{
  g_return_if_fail (reader != NULL);
  nuvashalauncher_line_reader_read (reader);
}
