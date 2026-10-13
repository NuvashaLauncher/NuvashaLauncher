/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_DOWNLOAD_PRIVATE_H
#define AML_DOWNLOAD_PRIVATE_H

#include "aml-download.h"

#include <libsoup/soup.h>

struct _AmlDownloadService {
  SoupSession *session;
  guint interactive_running;
  guint bulk_running;
  GQueue interactive_waiting;
  GQueue bulk_waiting;
  gboolean prefer_interactive;
  AmlSettings *cache_settings;
};

typedef struct _DownloadRequest
{
  GTask *task; /* the only reference: dropped when the request ends */
  AmlDownloadService *service; /* application-owned transfer scheduler */

  /* Set once when the request is created. */
  char *url;
  char *dest_path;       /* NULL for a JSON request */
  char *expected_hash;   /* NULL: nothing to verify */
  GChecksumType hash_type;
  char *body;            /* NULL: a GET request */
  char *user_agent;      /* NULL: the session's own */
  AmlDownloadProgressFunc progress;
  gpointer user_data;
  gboolean json;
  gboolean api_cache;
  char *cache_path;
  char *global_cache_path;
  char *stale_json;
  char *cache_etag;
  char *response_etag;
  AmlSettingsCacheExpiration cache_expiration;
  JsonParser *pending_parser;
  char *pending_cache_data;

  /* What runs once this request holds a slot: the cache check and then
     the transfer for a file, the transfer alone for JSON. Holding the
     slot from the very start is what keeps a whole version's assets from
     opening thousands of files at once to hash them. */
  void (*begin) (struct _DownloadRequest *request);

  /* One attempt at a time. */
  SoupMessage *message;
  GInputStream *source;  /* response body, closed when it ends */
  GOutputStream *sink;   /* the temporary file */
  GBytes *chunk;         /* block being written */
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
  AmlTaskScope *scope;
} DownloadRequest;
AmlSettings *aml_download_get_cache_settings (DownloadRequest *request);
char *aml_download_api_cache_path (DownloadRequest *request);
char *aml_download_global_cache_path (const char *hash);
JsonParser *aml_download_cache_parser (const char *contents, gsize length,
                                       char **response, char **etag,
                                       gint64 *created);

AmlTaskScope *aml_download_hash_file_async (GFile *file, GChecksumType type,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);
char *aml_download_hash_file_finish (GAsyncResult *result, GError **error);
GError *aml_download_cancelled_error (void);
void aml_download_request_fail (DownloadRequest *request, GError *error);
void aml_download_request_succeed_json (DownloadRequest *request,
                                        JsonParser *parser);
void aml_download_cache_write (DownloadRequest *request, const char *response);

#endif
