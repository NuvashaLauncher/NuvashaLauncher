/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <gio/gio.h>
#include <glib.h>

#include "aml-task.h"

typedef struct {
  AmlTaskSequence *sequence;
  guint calls;
  gboolean data_freed;
} SequenceData;

typedef struct {
  GMainLoop *loop;
  char *answer;
} SequenceResult;

static void
sequence_data_free (gpointer user_data)
{
  SequenceData *data = user_data;

  data->data_freed = TRUE;
}

static void
sequence_step (GTask *task G_GNUC_UNUSED, gpointer user_data)
{
  SequenceData *data = user_data;

  data->calls++;

  if (data->calls == 1)
    {
      aml_task_sequence_next (data->sequence);
      return;
    }

  aml_task_sequence_return_pointer (data->sequence, g_strdup ("complete"),
                                    g_free);
}

static void
sequence_done (GObject *source G_GNUC_UNUSED, GAsyncResult *result,
               gpointer user_data)
{
  SequenceResult *result_data = user_data;
  g_autoptr (GError) error = NULL;

  result_data->answer =
    g_task_propagate_pointer (G_TASK (result), &error);
  g_assert_no_error (error);
  g_main_loop_quit (result_data->loop);
}

static void
test_sequence_progresses_and_cleans_up (void)
{
  SequenceData data = { 0 };
  SequenceResult result = { 0 };

  result.loop = g_main_loop_new (NULL, FALSE);

  data.sequence = aml_task_sequence_new (NULL, sequence_done, &result,
                                          sequence_step, &data,
                                          sequence_data_free);
  aml_task_sequence_start (data.sequence);
  g_main_loop_run (result.loop);

  g_assert_cmpuint (data.calls, ==, 2);
  g_assert_true (data.data_freed);
  g_assert_cmpstr (result.answer, ==, "complete");
  g_free (result.answer);
  g_main_loop_unref (result.loop);
  g_print ("ok - task sequence advances, returns results and frees state\n");
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("# Task sequence\n");
  test_sequence_progresses_and_cleans_up ();
  return 0;
}
