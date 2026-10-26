/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_DOWNLOAD_PRIVATE_H
#define nuvashalauncher_DOWNLOAD_PRIVATE_H

#include "nuvashalauncher-download.h"

#include <libsoup/soup.h>

#define MAX_CONCURRENT 8
#define MAX_BULK_CONCURRENT 6
#define DOWNLOAD_PRIORITY_BULK 0
#define DOWNLOAD_PRIORITY_INTERACTIVE 100

struct _nuvashalauncherDownloadService {
  SoupSession *session;
  guint interactive_running;
  guint bulk_running;
  GQueue interactive_waiting;
  GQueue bulk_waiting;
  gboolean prefer_interactive;
  gboolean destroy_requested;
  nuvashalauncherSettings *cache_settings;
};

typedef struct _DownloadRequest
{
  GTask *task; /* the only reference: dropped when the request ends */
  nuvashalauncherDownloadService *service; /* application-owned transfer scheduler */

  /* Set once when the request is created. */
  char *url;
  char *dest_path;       /* NULL for a JSON request */
  char *expected_hash;   /* NULL for an unverified request */
  goffset max_size;      /* -1 when the caller supplied no limit */
  GChecksumType hash_type;
  char *body;            /* NULL: a GET request */
  char *user_agent;      /* NULL: the session's own */
  nuvashalauncherDownloadProgressFunc progress;
  gpointer user_data;
  gboolean json;
  gboolean api_cache;
  char *cache_path;
  char *global_cache_path;
  char *stale_json;
  char *cache_etag;
  char *response_etag;
  nuvashalauncherSettingsCacheExpiration cache_expiration;
  JsonParser *pending_parser;
  char *pending_cache_data;

  /* Start the cache check and transfer after acquiring a scheduler slot. */
  void (*begin) (struct _DownloadRequest *request);

  /* One attempt at a time. */
  SoupMessage *message;
  GInputStream *source;  /* response body, closed when it ends */
  GOutputStream *sink;   /* the temporary file */
  GBytes *chunk;         /* block being written */
  GByteArray *json_body; /* bounded JSON response being streamed */
  GChecksum *checksum;   /* hash of the received bytes */
  char *tmp_path;
  char *tmp_name;
  int parent_fd;
  int fd;
  goffset total;         /* announced length, -1 when unknown */
  goffset done;
  GError *pending_error; /* waits for the response stream to close */
  guint http_status;     /* status of a failed attempt */
  guint attempts;        /* attempts used, MAX_ATTEMPTS at most */
  gboolean hash_retried; /* a wrong hash is retried only once */
  gboolean holds_slot;
  guint priority;
  guint retry_source;    /* pending retry timeout, 0 while there is none */
  guint retry_cancel_source; /* cancellation watch for a pending retry */
  guint queue_cancel_source; /* cancellation watch while queued */
  nuvashalauncherTaskScope *scope;
} DownloadRequest;
nuvashalauncherSettings *nuvashalauncher_download_get_cache_settings (DownloadRequest *request);
char *nuvashalauncher_download_api_cache_path (DownloadRequest *request);
char *nuvashalauncher_download_global_cache_path (const char *hash);
JsonParser *nuvashalauncher_download_cache_parser (const char *contents, gsize length,
                                       char **response, char **etag,
                                       gint64 *created);

nuvashalauncherTaskScope *nuvashalauncher_download_hash_file_async (GFile *file, GChecksumType type,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);
char *nuvashalauncher_download_hash_file_finish (GAsyncResult *result, GError **error);
GError *nuvashalauncher_download_cancelled_error (void);
void nuvashalauncher_download_request_fail (DownloadRequest *request, GError *error);
void nuvashalauncher_download_request_succeed_json (DownloadRequest *request,
                                        JsonParser *parser);
void nuvashalauncher_download_cache_write (DownloadRequest *request, const char *response);

#endif

void nuvashalauncher_download_close_sink (DownloadRequest *request);
void nuvashalauncher_download_discard_temporary (DownloadRequest *request);
gboolean nuvashalauncher_download_open_temporary (DownloadRequest *request, GError **error);

void nuvashalauncher_download_start_transfer (DownloadRequest *request);
void start_attempt (DownloadRequest *request);
SoupSession *get_session (nuvashalauncherDownloadService *service);
char *url_for_message (const char *url);
gboolean request_was_cancelled (DownloadRequest *request, GError *error);
void request_succeed (DownloadRequest *request);
void nuvashalauncher_download_file_cache_start (DownloadRequest *request);
void json_cache_start (DownloadRequest *request);
void global_cache_directory_ready (GObject *, GAsyncResult *, gpointer);
void nuvashalauncher_download_release_slot (DownloadRequest *request);
gint compare_waiting_requests (gconstpointer left, gconstpointer right, gpointer user_data);
gboolean queued_cancelled (gpointer user_data);
void service_destroy_if_requested (nuvashalauncherDownloadService *service);

void nuvashalauncher_download_json_process_body (DownloadRequest *request, GBytes *body);
void nuvashalauncher_download_json_stream_ready (GObject *source, GAsyncResult *result, gpointer user_data);

#define MAX_JSON_RESPONSE_SIZE (64ULL * 1024ULL * 1024ULL)
GError *check_status (DownloadRequest *request);
void end_attempt (DownloadRequest *request, GError *error);
void read_next_chunk (DownloadRequest *request);
