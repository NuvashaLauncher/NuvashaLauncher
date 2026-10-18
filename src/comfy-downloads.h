/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyDownloads: the downloads that are running right now.
 *
 * A download is started by a dialog or a content page, but it is owned
 * here, not there: closing the dialog that began an install does not
 * stop it, and the header of the main window can show everything that is
 * still on its way, from anywhere in the app.
 *
 * The manager is a process wide singleton. Every entry carries what a
 * row needs (a title, a detail line and a fraction) and a GCancellable,
 * so cancelling is one call away from the UI that shows it. */

#ifndef comfy_DOWNLOADS_H
#define comfy_DOWNLOADS_H

#include <gio/gio.h>
#include <glib-object.h>

typedef struct _ComfyDownloadService ComfyDownloadService;

G_BEGIN_DECLS

typedef enum {
  comfy_DOWNLOAD_MODPACK,
  comfy_DOWNLOAD_MOD,
  comfy_DOWNLOAD_RESOURCE_PACK,
  comfy_DOWNLOAD_SHADER,
  comfy_DOWNLOAD_UPDATE
} ComfyDownloadKind;

/* What the progress counts: files for an install, bytes for a single
 * transfer. The UI formats each differently. */
typedef enum {
  comfy_DOWNLOAD_UNIT_COUNT,
  comfy_DOWNLOAD_UNIT_BYTES
} ComfyDownloadUnit;

#define comfy_TYPE_DOWNLOAD (comfy_download_get_type ())

G_DECLARE_FINAL_TYPE (ComfyDownload, comfy_download, comfy, DOWNLOAD, GObject)

#define comfy_TYPE_DOWNLOADS (comfy_downloads_get_type ())

G_DECLARE_FINAL_TYPE (ComfyDownloads, comfy_downloads, comfy, DOWNLOADS, GObject)

/* The one manager of the process. */
ComfyDownloads *comfy_downloads_get_default (void);
ComfyDownloadService *comfy_downloads_get_service (void);

/* Starts an entry and returns it. The manager owns it while it is
 * running: @title is copied. (transfer none) */
ComfyDownload *comfy_downloads_add (ComfyDownloadKind kind, const char *title);

/* The detail line under the title, e.g. "12 of 40 files". */
void comfy_download_set_detail (ComfyDownload *self, const char *detail);

/* How far along the download is, counted in files. @total 0 means "not
 * known yet", which the UI shows as an activity spinner rather than a
 * bar. */
void comfy_download_set_progress (ComfyDownload *self, guint64 done,
                                guint64 total);

/* How far along a single transfer is, counted in bytes. @total 0 means
 * the server did not announce a length. */
void comfy_download_set_bytes (ComfyDownload *self, guint64 done, guint64 total);

/* The cancellable the running operation holds: cancelling the row stops
 * the work. (transfer none) */
GCancellable *comfy_download_get_cancellable (ComfyDownload *self);

void comfy_download_cancel (ComfyDownload *self);

/* Takes the entry out of the manager and unrefs the manager's hold on
 * it. Whoever still holds a reference (a completion callback) owns it
 * from then on. */
void comfy_download_finish (ComfyDownload *self);

guint comfy_downloads_count (void);

/* How many files the user has picked across every content page for a
 * later install: what the header's download icon carries a small number
 * of. A page says its part with comfy_downloads_picked_add(), adding when
 * its list grows and passing the negative of what it added when its list
 * shrinks or the page goes away, so that what is shown is what the open
 * pages have on. */
guint comfy_downloads_picked (void);

void comfy_downloads_picked_add (gint delta);

/* (transfer none) every running download, in the order it started. Never
 * NULL; do not modify. */
GPtrArray *comfy_downloads_get_all (void);

const char *comfy_download_get_title (ComfyDownload *self);
const char *comfy_download_get_detail (ComfyDownload *self);
ComfyDownloadKind comfy_download_get_kind (ComfyDownload *self);
ComfyDownloadUnit comfy_download_get_unit (ComfyDownload *self);
guint64 comfy_download_get_done (ComfyDownload *self);
guint64 comfy_download_get_total (ComfyDownload *self);

G_END_DECLS

#endif /* comfy_DOWNLOADS_H */
