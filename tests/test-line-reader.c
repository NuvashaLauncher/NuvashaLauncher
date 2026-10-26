/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <gio/gio.h>
#include <glib.h>

#include "nuvashalauncher-line-reader.h"

typedef struct {
  GMainLoop *loop;
  GPtrArray *lines;
  gboolean stderr_line;
  gboolean done;
  gboolean data_freed;
} ReaderResult;

static void
on_line (const char *line, gboolean stderr_line, gpointer user_data)
{
  ReaderResult *result = user_data;

  g_ptr_array_add (result->lines, g_strdup (line));
  result->stderr_line = stderr_line;
}

static void
on_done (GError *error, gpointer user_data)
{
  ReaderResult *result = user_data;

  g_assert_no_error (error);
  result->done = TRUE;
  g_main_loop_quit (result->loop);
}

static void
on_result_free (gpointer user_data)
{
  ReaderResult *result = user_data;

  result->data_freed = TRUE;
}

static void
test_reads_lines_until_eof (void)
{
  static const char contents[] = "first line\nsecond line\n";
  ReaderResult result = { 0 };
  g_autoptr (GInputStream) stream = NULL;
  nuvashalauncherLineReader *reader = NULL;

  result.loop = g_main_loop_new (NULL, FALSE);
  result.lines = g_ptr_array_new_with_free_func (g_free);
  stream = g_memory_input_stream_new_from_data (contents, -1, NULL);
  reader = nuvashalauncher_line_reader_new (stream, TRUE, on_line, on_done, &result,
                                on_result_free);
  nuvashalauncher_line_reader_start (reader);
  g_main_loop_run (result.loop);

  g_assert_true (result.done);
  g_assert_true (result.data_freed);
  g_assert_true (result.stderr_line);
  g_assert_cmpuint (result.lines->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (result.lines, 0), ==, "first line");
  g_assert_cmpstr (g_ptr_array_index (result.lines, 1), ==, "second line");

  g_ptr_array_unref (result.lines);
  g_main_loop_unref (result.loop);
  g_print ("ok - line reader emits lines, tags stderr and cleans up at EOF\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Line reader\n");
  test_reads_lines_until_eof ();
  return 0;
}
