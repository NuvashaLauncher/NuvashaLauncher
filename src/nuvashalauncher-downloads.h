/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherDownloads: the downloads that are running right now.
 *
 * A download is started by a dialog or a content page, but it is owned
 * here, not there: closing the dialog that began an install does not
 * stop it, and the header of the main window can show everything that is
 * still on its way, from anywhere in the app.
 *
 * The manager is a process-wide singleton. Each entry carries the title,
 * detail, progress, and cancellable needed by its row,
 * so cancelling is one call away from the UI that shows it. */

#ifndef nuvashalauncher_DOWNLOADS_H
#define nuvashalauncher_DOWNLOADS_H

#include <gio/gio.h>
#include <glib-object.h>

typedef struct _NuvashaLauncherDownloadService NuvashaLauncherDownloadService;

G_BEGIN_DECLS

typedef enum {
  nuvashalauncher_DOWNLOAD_MODPACK,
  nuvashalauncher_DOWNLOAD_MOD,
  nuvashalauncher_DOWNLOAD_RESOURCE_PACK,
  nuvashalauncher_DOWNLOAD_SHADER,
  nuvashalauncher_DOWNLOAD_UPDATE
} NuvashaLauncherDownloadKind;

/* Progress units: files for installs, bytes for transfers. */
typedef enum {
  nuvashalauncher_DOWNLOAD_UNIT_COUNT,
  nuvashalauncher_DOWNLOAD_UNIT_BYTES
} NuvashaLauncherDownloadUnit;

#define nuvashalauncher_TYPE_DOWNLOAD (nuvashalauncher_download_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLauncherDownload, nuvashalauncher_download, nuvashalauncher, DOWNLOAD, GObject)

#define nuvashalauncher_TYPE_DOWNLOADS (nuvashalauncher_downloads_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLauncherDownloads, nuvashalauncher_downloads, nuvashalauncher, DOWNLOADS, GObject)

/* Process-wide download manager. */
NuvashaLauncherDownloads *nuvashalauncher_downloads_get_default (void);
NuvashaLauncherDownloadService *nuvashalauncher_downloads_get_service (void);

/* Starts an entry and returns it. The manager owns it while it is
 * running: @title is copied. (transfer none) */
NuvashaLauncherDownload *nuvashalauncher_downloads_add (NuvashaLauncherDownloadKind kind, const char *title);

/* The detail line under the title, e.g. "12 of 40 files". */
void nuvashalauncher_download_set_detail (NuvashaLauncherDownload *self, const char *detail);

/* How far along the download is, counted in files. @total 0 means "not
 * known yet", which the UI shows as an activity spinner rather than a
 * bar. */
void nuvashalauncher_download_set_progress (NuvashaLauncherDownload *self, guint64 done,
                                guint64 total);

/* How far along a single transfer is, counted in bytes. @total 0 means
 * the server did not announce a length. */
void nuvashalauncher_download_set_bytes (NuvashaLauncherDownload *self, guint64 done, guint64 total);

/* The cancellable the running operation holds: cancelling the row stops
 * the work. (transfer none) */
GCancellable *nuvashalauncher_download_get_cancellable (NuvashaLauncherDownload *self);

void nuvashalauncher_download_cancel (NuvashaLauncherDownload *self);

/* Takes the entry out of the manager and unrefs the manager's hold on
 * it. Whoever still holds a reference (a completion callback) owns it
 * from then on. */
void nuvashalauncher_download_finish (NuvashaLauncherDownload *self);

guint nuvashalauncher_downloads_count (void);

/* Number of files selected across content pages for later installation. */
guint nuvashalauncher_downloads_picked (void);

void nuvashalauncher_downloads_picked_add (gint delta);

/* (transfer none) every running download, in the order it started. Never
 * NULL; do not modify. */
GPtrArray *nuvashalauncher_downloads_get_all (void);

const char *nuvashalauncher_download_get_title (NuvashaLauncherDownload *self);
const char *nuvashalauncher_download_get_detail (NuvashaLauncherDownload *self);
NuvashaLauncherDownloadKind nuvashalauncher_download_get_kind (NuvashaLauncherDownload *self);
NuvashaLauncherDownloadUnit nuvashalauncher_download_get_unit (NuvashaLauncherDownload *self);
guint64 nuvashalauncher_download_get_done (NuvashaLauncherDownload *self);
guint64 nuvashalauncher_download_get_total (NuvashaLauncherDownload *self);

G_END_DECLS

#endif /* nuvashalauncher_DOWNLOADS_H */
