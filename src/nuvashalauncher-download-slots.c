/* Download scheduler slot management. */

#include "nuvashalauncher-download-private.h"
#include "nuvashalauncher-task-private.h"

void
nuvashalauncher_download_start_transfer (DownloadRequest *request)
{
  nuvashalauncherDownloadService *service = request->service;
  gboolean interactive = request->priority >= DOWNLOAD_PRIORITY_INTERACTIVE;
  guint total = service->interactive_running + service->bulk_running;
  gboolean has_slot = total < MAX_CONCURRENT &&
                      (interactive || service->bulk_running < MAX_BULK_CONCURRENT);

  if (has_slot)
    {
      if (interactive)
        service->interactive_running++;
      else
        service->bulk_running++;
      request->holds_slot = TRUE;
      nuvashalauncher_task_scope_set_state (request->scope, nuvashalauncher_TASK_STATE_RUNNING);
      g_debug ("download operation=%" G_GUINT64_FORMAT " start pool=%s active=%u queued=%u/%u",
               nuvashalauncher_task_scope_get_id (request->scope), interactive ? "interactive" : "bulk",
               nuvashalauncher_download_service_active (service),
               nuvashalauncher_download_service_queued_interactive (service),
               nuvashalauncher_download_service_queued_bulk (service));
      request->begin (request);
      return;
    }

  GQueue *queue = interactive ? &service->interactive_waiting
                              : &service->bulk_waiting;
  GCancellable *cancellable = g_task_get_cancellable (request->task);

  g_queue_insert_sorted (queue, request, compare_waiting_requests, NULL);
  g_debug ("download operation=%" G_GUINT64_FORMAT " queued pool=%s queued=%u/%u",
           nuvashalauncher_task_scope_get_id (request->scope), interactive ? "interactive" : "bulk",
           nuvashalauncher_download_service_queued_interactive (service),
           nuvashalauncher_download_service_queued_bulk (service));

  /* Watched through the scope, so the watch is removed along with every
     other source this request owns. nuvashalauncher_cancellable_watch() hands the
     callback this request and not the cancellable, which a cancellable
     source would have done instead. */
  request->queue_cancel_source =
    nuvashalauncher_cancellable_watch (cancellable, queued_cancelled, request, NULL);
  nuvashalauncher_task_scope_track_source (request->scope, request->queue_cancel_source);
}

void
nuvashalauncher_download_release_slot (DownloadRequest *request)
{
  nuvashalauncherDownloadService *service = request->service;
  if (!request->holds_slot)
    return;

  request->holds_slot = FALSE;
  if (request->priority >= DOWNLOAD_PRIORITY_INTERACTIVE)
    service->interactive_running--;
  else
    service->bulk_running--;

  while (service->interactive_running + service->bulk_running < MAX_CONCURRENT)
    {
      DownloadRequest *next = NULL;
      gboolean interactive_available =
        !g_queue_is_empty (&service->interactive_waiting);
      gboolean bulk_available =
        service->bulk_running < MAX_BULK_CONCURRENT &&
        !g_queue_is_empty (&service->bulk_waiting);

      /* Alternate between classes whenever both have work. This preserves
       * responsiveness without allowing a long stream of interactive/API
       * requests to starve the bulk queue. Priority still applies within
       * each class through g_queue_insert_sorted(). */
      if (interactive_available && bulk_available && service->prefer_interactive)
        next = g_queue_pop_head (&service->interactive_waiting);
      else if (interactive_available && !bulk_available)
        next = g_queue_pop_head (&service->interactive_waiting);
      else if (bulk_available)
        next = g_queue_pop_head (&service->bulk_waiting);
      else if (interactive_available)
        next = g_queue_pop_head (&service->interactive_waiting);

      if (next == NULL)
        break;

      if (next->queue_cancel_source != 0)
        {
          nuvashalauncher_task_scope_remove_source (next->scope,
                                        next->queue_cancel_source);
          next->queue_cancel_source = 0;
        }

      if (next->priority >= DOWNLOAD_PRIORITY_INTERACTIVE)
        {
          service->interactive_running++;
          service->prefer_interactive = FALSE;
        }
      else
        {
          service->bulk_running++;
          service->prefer_interactive = TRUE;
        }
      next->holds_slot = TRUE;
      nuvashalauncher_task_scope_set_state (next->scope, nuvashalauncher_TASK_STATE_RUNNING);
      g_debug ("download operation=%" G_GUINT64_FORMAT " dequeued active=%u queued=%u/%u",
               nuvashalauncher_task_scope_get_id (next->scope), nuvashalauncher_download_service_active (service),
               nuvashalauncher_download_service_queued_interactive (service),
               nuvashalauncher_download_service_queued_bulk (service));
      next->begin (next);
    }

  service_destroy_if_requested (service);
}

void
nuvashalauncher_download_service_cancel_queued (nuvashalauncherDownloadService *owned_service)
{
  if (owned_service == NULL)
    return;
  nuvashalauncherDownloadService *service = owned_service;

  while (!g_queue_is_empty (&service->interactive_waiting))
    {
      DownloadRequest *request =
        g_queue_pop_head (&service->interactive_waiting);
      if (request->queue_cancel_source != 0)
        {
          nuvashalauncher_task_scope_remove_source (request->scope,
                                        request->queue_cancel_source);
          request->queue_cancel_source = 0;
        }
      nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
    }

  while (!g_queue_is_empty (&service->bulk_waiting))
    {
      DownloadRequest *request = g_queue_pop_head (&service->bulk_waiting);
      if (request->queue_cancel_source != 0)
        {
          nuvashalauncher_task_scope_remove_source (request->scope,
                                        request->queue_cancel_source);
          request->queue_cancel_source = 0;
        }
      nuvashalauncher_download_request_fail (request, nuvashalauncher_download_cancelled_error ());
    }
}
