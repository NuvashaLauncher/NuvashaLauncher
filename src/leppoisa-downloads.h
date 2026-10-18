/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaDownloads: the downloads that are running right now.
 *
 * A download is started by a dialog or a content page, but it is owned
 * here, not there: closing the dialog that began an install does not
 * stop it, and the header of the main window can show everything that is
 * still on its way, from anywhere in the app.
 *
 * The manager is a process wide singleton. Every entry carries what a
 * row needs (a title, a detail line and a fraction) and a GCancellable,
 * so cancelling is one call away from the UI that shows it. */

#ifndef leppoisa_DOWNLOADS_H
#define leppoisa_DOWNLOADS_H

#include <gio/gio.h>
#include <glib-object.h>

typedef struct _LeppoisaDownloadService LeppoisaDownloadService;

G_BEGIN_DECLS

typedef enum {
  leppoisa_DOWNLOAD_MODPACK,
  leppoisa_DOWNLOAD_MOD,
  leppoisa_DOWNLOAD_RESOURCE_PACK,
  leppoisa_DOWNLOAD_SHADER,
  leppoisa_DOWNLOAD_UPDATE
} LeppoisaDownloadKind;

/* What the progress counts: files for an install, bytes for a single
 * transfer. The UI formats each differently. */
typedef enum {
  leppoisa_DOWNLOAD_UNIT_COUNT,
  leppoisa_DOWNLOAD_UNIT_BYTES
} LeppoisaDownloadUnit;

#define leppoisa_TYPE_DOWNLOAD (leppoisa_download_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaDownload, leppoisa_download, leppoisa, DOWNLOAD, GObject)

#define leppoisa_TYPE_DOWNLOADS (leppoisa_downloads_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaDownloads, leppoisa_downloads, leppoisa, DOWNLOADS, GObject)

/* The one manager of the process. */
LeppoisaDownloads *leppoisa_downloads_get_default (void);
LeppoisaDownloadService *leppoisa_downloads_get_service (void);

/* Starts an entry and returns it. The manager owns it while it is
 * running: @title is copied. (transfer none) */
LeppoisaDownload *leppoisa_downloads_add (LeppoisaDownloadKind kind, const char *title);

/* The detail line under the title, e.g. "12 of 40 files". */
void leppoisa_download_set_detail (LeppoisaDownload *self, const char *detail);

/* How far along the download is, counted in files. @total 0 means "not
 * known yet", which the UI shows as an activity spinner rather than a
 * bar. */
void leppoisa_download_set_progress (LeppoisaDownload *self, guint64 done,
                                guint64 total);

/* How far along a single transfer is, counted in bytes. @total 0 means
 * the server did not announce a length. */
void leppoisa_download_set_bytes (LeppoisaDownload *self, guint64 done, guint64 total);

/* The cancellable the running operation holds: cancelling the row stops
 * the work. (transfer none) */
GCancellable *leppoisa_download_get_cancellable (LeppoisaDownload *self);

void leppoisa_download_cancel (LeppoisaDownload *self);

/* Takes the entry out of the manager and unrefs the manager's hold on
 * it. Whoever still holds a reference (a completion callback) owns it
 * from then on. */
void leppoisa_download_finish (LeppoisaDownload *self);

guint leppoisa_downloads_count (void);

/* How many files the user has picked across every content page for a
 * later install: what the header's download icon carries a small number
 * of. A page says its part with leppoisa_downloads_picked_add(), adding when
 * its list grows and passing the negative of what it added when its list
 * shrinks or the page goes away, so that what is shown is what the open
 * pages have on. */
guint leppoisa_downloads_picked (void);

void leppoisa_downloads_picked_add (gint delta);

/* (transfer none) every running download, in the order it started. Never
 * NULL; do not modify. */
GPtrArray *leppoisa_downloads_get_all (void);

const char *leppoisa_download_get_title (LeppoisaDownload *self);
const char *leppoisa_download_get_detail (LeppoisaDownload *self);
LeppoisaDownloadKind leppoisa_download_get_kind (LeppoisaDownload *self);
LeppoisaDownloadUnit leppoisa_download_get_unit (LeppoisaDownload *self);
guint64 leppoisa_download_get_done (LeppoisaDownload *self);
guint64 leppoisa_download_get_total (LeppoisaDownload *self);

G_END_DECLS

#endif /* leppoisa_DOWNLOADS_H */
