/* Cancellation-to-main-loop source bridge. */

#include "nuvashalauncher-task-private.h"

/* The state behind nuvashalauncher_cancellable_watch(). It lives as long as
 * the source does, and the source's destroy notify tears it down. */
typedef struct
{
  GSourceFunc callback;
  gpointer user_data;
  GDestroyNotify notify;
} CancellableWatch;

static gboolean
watch_dispatch (GCancellable *cancellable G_GNUC_UNUSED,
                gpointer      user_data)
{
  CancellableWatch *watch = user_data;

  return watch->callback (watch->user_data);
}

static void
watch_free (gpointer user_data)
{
  CancellableWatch *watch = user_data;

  if (watch->notify != NULL)
    watch->notify (watch->user_data);
  g_free (watch);
}

guint
nuvashalauncher_cancellable_watch (GCancellable  *cancellable,
                                   GSourceFunc    callback,
                                   gpointer       user_data,
                                   GDestroyNotify notify)
{
  CancellableWatch *watch;
  GSource *source;
  guint source_id;

  g_return_val_if_fail (G_IS_CANCELLABLE (cancellable), 0);
  g_return_val_if_fail (callback != NULL, 0);

  watch = g_new0 (CancellableWatch, 1);
  watch->callback = callback;
  watch->user_data = user_data;
  watch->notify = notify;

  source = g_cancellable_source_new (cancellable);
  g_source_set_callback (source, (GSourceFunc) watch_dispatch, watch, watch_free);

  source_id = g_source_attach (source, g_main_context_get_thread_default ());
  g_source_unref (source);

  return source_id;
}
