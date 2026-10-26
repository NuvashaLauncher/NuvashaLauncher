/* Cancellation-to-main-loop source bridge. */

#include "nuvashalauncher-task-private.h"

/* The state behind nuvashalauncher_cancellable_watch(). It lives as long as
 * the source does, and the source's destroy notify tears it down. */
typedef struct
{
  GCancellable *cancellable;
  GSourceFunc callback;
  gpointer user_data;
  GDestroyNotify notify;
  gulong handler;
} CancellableWatch;

static gboolean
watch_dispatch (gpointer user_data)
{
  CancellableWatch *watch = user_data;

  return watch->callback (watch->user_data);
}

static void
watch_free (gpointer user_data)
{
  CancellableWatch *watch = user_data;

  if (watch->handler != 0)
    g_cancellable_disconnect (watch->cancellable, watch->handler);
  g_clear_object (&watch->cancellable);

  if (watch->notify != NULL)
    watch->notify (watch->user_data);
  g_free (watch);
}

static void
watch_cancelled (GCancellable *cancellable G_GNUC_UNUSED,
                 gpointer      user_data)
{
  g_source_set_ready_time (user_data, 0);
}

guint
nuvashalauncher_cancellable_watch (GCancellable  *cancellable,
                                   GSourceFunc    callback,
                                   gpointer       user_data,
                                   GDestroyNotify notify)
{
  CancellableWatch *watch = g_new0 (CancellableWatch, 1);
  GSource *source;
  guint source_id;

  g_return_val_if_fail (G_IS_CANCELLABLE (cancellable), 0);
  g_return_val_if_fail (callback != NULL, 0);

  watch->callback = callback;
  watch->user_data = user_data;
  watch->notify = notify;
  watch->cancellable = g_object_ref (cancellable);

  source = g_timeout_source_new (0);
  g_source_set_ready_time (source, G_MAXINT64);
  g_source_set_callback (source, watch_dispatch, watch, watch_free);
  watch->handler =
    g_cancellable_connect (cancellable, G_CALLBACK (watch_cancelled), source,
                           NULL);

  source_id = g_source_attach (source, NULL);
  g_source_unref (source);

  return source_id;
}
