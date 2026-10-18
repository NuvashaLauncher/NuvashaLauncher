/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaDownloads: the downloads that are running right now.
 *
 * A download is started by a dialog or a content page, but it is owned
 * here, not there: closing the dialog that began an install does not
 * stop it, and the header of the main window can show everything that is
 * still on its way, from anywhere in the app.
 *
 * The manager is a process wide singleton. Every entry carries what a
 * row needs (a title, a detail line and a fraction) and a GCancellable,
 * so cancelling is one call away from the UI that shows it. */

#ifndef nuvasha_DOWNLOADS_H
#define nuvasha_DOWNLOADS_H

#include <gio/gio.h>
#include <glib-object.h>

typedef struct _NuvashaDownloadService NuvashaDownloadService;

G_BEGIN_DECLS

typedef enum {
  nuvasha_DOWNLOAD_MODPACK,
  nuvasha_DOWNLOAD_MOD,
  nuvasha_DOWNLOAD_RESOURCE_PACK,
  nuvasha_DOWNLOAD_SHADER,
  nuvasha_DOWNLOAD_UPDATE
} NuvashaDownloadKind;

/* What the progress counts: files for an install, bytes for a single
 * transfer. The UI formats each differently. */
typedef enum {
  nuvasha_DOWNLOAD_UNIT_COUNT,
  nuvasha_DOWNLOAD_UNIT_BYTES
} NuvashaDownloadUnit;

#define nuvasha_TYPE_DOWNLOAD (nuvasha_download_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaDownload, nuvasha_download, nuvasha, DOWNLOAD, GObject)

#define nuvasha_TYPE_DOWNLOADS (nuvasha_downloads_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaDownloads, nuvasha_downloads, nuvasha, DOWNLOADS, GObject)

/* The one manager of the process. */
NuvashaDownloads *nuvasha_downloads_get_default (void);
NuvashaDownloadService *nuvasha_downloads_get_service (void);

/* Starts an entry and returns it. The manager owns it while it is
 * running: @title is copied. (transfer none) */
NuvashaDownload *nuvasha_downloads_add (NuvashaDownloadKind kind, const char *title);

/* The detail line under the title, e.g. "12 of 40 files". */
void nuvasha_download_set_detail (NuvashaDownload *self, const char *detail);

/* How far along the download is, counted in files. @total 0 means "not
 * known yet", which the UI shows as an activity spinner rather than a
 * bar. */
void nuvasha_download_set_progress (NuvashaDownload *self, guint64 done,
                                guint64 total);

/* How far along a single transfer is, counted in bytes. @total 0 means
 * the server did not announce a length. */
void nuvasha_download_set_bytes (NuvashaDownload *self, guint64 done, guint64 total);

/* The cancellable the running operation holds: cancelling the row stops
 * the work. (transfer none) */
GCancellable *nuvasha_download_get_cancellable (NuvashaDownload *self);

void nuvasha_download_cancel (NuvashaDownload *self);

/* Takes the entry out of the manager and unrefs the manager's hold on
 * it. Whoever still holds a reference (a completion callback) owns it
 * from then on. */
void nuvasha_download_finish (NuvashaDownload *self);

guint nuvasha_downloads_count (void);

/* How many files the user has picked across every content page for a
 * later install: what the header's download icon carries a small number
 * of. A page says its part with nuvasha_downloads_picked_add(), adding when
 * its list grows and passing the negative of what it added when its list
 * shrinks or the page goes away, so that what is shown is what the open
 * pages have on. */
guint nuvasha_downloads_picked (void);

void nuvasha_downloads_picked_add (gint delta);

/* (transfer none) every running download, in the order it started. Never
 * NULL; do not modify. */
GPtrArray *nuvasha_downloads_get_all (void);

const char *nuvasha_download_get_title (NuvashaDownload *self);
const char *nuvasha_download_get_detail (NuvashaDownload *self);
NuvashaDownloadKind nuvasha_download_get_kind (NuvashaDownload *self);
NuvashaDownloadUnit nuvasha_download_get_unit (NuvashaDownload *self);
guint64 nuvasha_download_get_done (NuvashaDownload *self);
guint64 nuvasha_download_get_total (NuvashaDownload *self);

G_END_DECLS

#endif /* nuvasha_DOWNLOADS_H */
