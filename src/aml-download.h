/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Asynchronous HTTP downloads: files with checksum verification and
 * cache reuse, plus JSON bodies for metadata and API calls.
 *
 * Every function is asynchronous in the GIO style (_async/_finish) and
 * must be called from the thread-default main context that runs the
 * callbacks, normally the GTK main loop. Nothing here blocks it. The
 * result passed to the callback has no source object: give it straight
 * to the matching _finish() function. */

#ifndef AML_DOWNLOAD_H
#define AML_DOWNLOAD_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "aml-settings.h"
#include "aml-task.h"

G_BEGIN_DECLS

typedef struct _AmlDownloadService AmlDownloadService;

AmlDownloadService *aml_download_service_new (void);
void aml_download_service_free (AmlDownloadService *service);

#define AML_DOWNLOAD_ERROR (aml_download_error_quark ())

typedef enum {
  AML_DOWNLOAD_ERROR_NETWORK,       /* connection, DNS, socket, timeout */
  AML_DOWNLOAD_ERROR_HTTP,          /* an HTTP status that is not success */
  AML_DOWNLOAD_ERROR_RATE_LIMITED,  /* HTTP 429 */
  AML_DOWNLOAD_ERROR_CHECKSUM,      /* received bytes have the wrong hash */
  AML_DOWNLOAD_ERROR_IO,            /* local file system failure */
  AML_DOWNLOAD_ERROR_CANCELLED      /* the caller cancelled the request */
} AmlDownloadError;

GQuark aml_download_error_quark (void);

void aml_download_set_cache_settings (AmlSettings *settings);

/* Releases the process-wide network service when no transfer is active.
 * Applications call this during orderly shutdown; a busy service is left
 * alive so in-flight callbacks cannot be invalidated. */
void aml_download_shutdown (void);

/* Progress of a running transfer. @bytes_done is what arrived so far,
 * @total is the announced length or -1 when the server did not say.
 * Called on the caller's main context; a download served from the
 * cache reports no progress at all. */
typedef void (*AmlDownloadProgressFunc) (goffset bytes_done,
                                         goffset total,
                                         gpointer user_data);

/* Downloads @url into @dest_path.
 *
 * The body is written to a temporary file next to @dest_path, hashed
 * while it arrives, fsynced and renamed over @dest_path only once it is
 * complete, so a failed, cancelled or wrong download never replaces an
 * existing file and never leaves a temporary file behind.
 *
 * @expected_hash is optional: when it is given, an existing @dest_path
 * with that hash is reused without any request, an existing one with a
 * different hash is treated as corrupt and downloaded again, and the
 * bytes just received are checked against it (a mismatch repeats the
 * transfer at most once). Without @expected_hash nothing can be
 * verified, so the transfer always happens. @hash_type selects the
 * GChecksum used for @expected_hash, normally G_CHECKSUM_SHA1,
 * G_CHECKSUM_SHA256 or G_CHECKSUM_SHA512.
 *
 * Network errors, HTTP 5xx and HTTP 429 are retried up to three times
 * with a short pause, honouring a Retry-After header; HTTP 404 and
 * other statuses are not retried. @dest_path must be an absolute path
 * whose directory already exists.
 *
 * @progress and @callback both receive @user_data. */
AmlTaskScope *aml_download_file_async (const char *url,
                                       const char *dest_path,
                                       const char *expected_hash,
                                       GChecksumType hash_type,
                                       AmlDownloadProgressFunc progress,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);

/* TRUE on success. The error is always one of AmlDownloadError; for
 * AML_DOWNLOAD_ERROR_IO its message is what the file system said. */
gboolean aml_download_file_finish (GAsyncResult *result,
                                   GError     **error);

/* Fetches @url and parses the response body as JSON, with the same
 * retry and rate limit rules as a file download.
 *
 * The returned parser is owned by the caller; a body that does not
 * parse comes back as a JSON_PARSER_ERROR from json-glib and is not
 * retried. */
AmlTaskScope *aml_download_json_async (const char *url,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);

/* (transfer full). NULL on error. */
JsonParser *aml_download_json_finish (GAsyncResult *result,
                                      GError     **error);

/* The hexadecimal digest of the bytes of @file, read in chunks and
 * never blocking the caller's main context. This is what a download is
 * verified against, and what a file already on disk is identified by.
 *
 * G_IO_ERROR_NOT_FOUND when there is no such file.
 *
 * (transfer full) the digest, lowercase and hexadecimal. */
AmlTaskScope *aml_download_file_hash_async (GFile *file,
                                             GChecksumType type,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

/* (transfer full). NULL on error. */
char *aml_download_file_hash_finish (GAsyncResult *result,
                                     GError     **error);

/* Posts @body as "application/json" to @url and parses the answer the
 * way aml_download_json_async() does, with the same retry and rate
 * limit rules.
 *
 * @user_agent replaces the User-Agent of the session for this one
 * request, which is what a service that wants to be told which client
 * it is talking to asks for. NULL keeps the session's own.
 *
 * The returned parser is owned by the caller. */
AmlTaskScope *aml_download_json_post_async (const char *url,
                                            const char *body,
                                            const char *user_agent,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

AmlTaskScope *aml_download_json_cached_async (const char *url,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);

AmlTaskScope *aml_download_json_post_cached_async (const char *url,
                                                   const char *body,
                                                   const char *user_agent,
                                                   GAsyncReadyCallback callback,
                                                   gpointer user_data);

G_END_DECLS

#endif /* AML_DOWNLOAD_H */
