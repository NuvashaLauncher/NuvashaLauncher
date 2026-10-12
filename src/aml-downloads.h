/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlDownloads: the downloads that are running right now.
 *
 * A download is started by a dialog or a content page, but it is owned
 * here, not there: closing the dialog that began an install does not
 * stop it, and the header of the main window can show everything that is
 * still on its way, from anywhere in the app.
 *
 * The manager is a process wide singleton. Every entry carries what a
 * row needs (a title, a detail line and a fraction) and a GCancellable,
 * so cancelling is one call away from the UI that shows it. */

#ifndef AML_DOWNLOADS_H
#define AML_DOWNLOADS_H

#include <gio/gio.h>
#include <glib-object.h>

typedef struct _AmlDownloadService AmlDownloadService;

G_BEGIN_DECLS

typedef enum {
  AML_DOWNLOAD_MODPACK,
  AML_DOWNLOAD_MOD,
  AML_DOWNLOAD_RESOURCE_PACK,
  AML_DOWNLOAD_SHADER,
  AML_DOWNLOAD_UPDATE
} AmlDownloadKind;

/* What the progress counts: files for an install, bytes for a single
 * transfer. The UI formats each differently. */
typedef enum {
  AML_DOWNLOAD_UNIT_COUNT,
  AML_DOWNLOAD_UNIT_BYTES
} AmlDownloadUnit;

#define AML_TYPE_DOWNLOAD (aml_download_get_type ())

G_DECLARE_FINAL_TYPE (AmlDownload, aml_download, AML, DOWNLOAD, GObject)

#define AML_TYPE_DOWNLOADS (aml_downloads_get_type ())

G_DECLARE_FINAL_TYPE (AmlDownloads, aml_downloads, AML, DOWNLOADS, GObject)

/* The one manager of the process. */
AmlDownloads *aml_downloads_get_default (void);
AmlDownloadService *aml_downloads_get_service (void);

/* Starts an entry and returns it. The manager owns it while it is
 * running: @title is copied. (transfer none) */
AmlDownload *aml_downloads_add (AmlDownloadKind kind, const char *title);

/* The detail line under the title, e.g. "12 of 40 files". */
void aml_download_set_detail (AmlDownload *self, const char *detail);

/* How far along the download is, counted in files. @total 0 means "not
 * known yet", which the UI shows as an activity spinner rather than a
 * bar. */
void aml_download_set_progress (AmlDownload *self, guint64 done,
                                guint64 total);

/* How far along a single transfer is, counted in bytes. @total 0 means
 * the server did not announce a length. */
void aml_download_set_bytes (AmlDownload *self, guint64 done, guint64 total);

/* The cancellable the running operation holds: cancelling the row stops
 * the work. (transfer none) */
GCancellable *aml_download_get_cancellable (AmlDownload *self);

void aml_download_cancel (AmlDownload *self);

/* Takes the entry out of the manager and unrefs the manager's hold on
 * it. Whoever still holds a reference (a completion callback) owns it
 * from then on. */
void aml_download_finish (AmlDownload *self);

guint aml_downloads_count (void);

/* How many files the user has picked across every content page for a
 * later install: what the header's download icon carries a small number
 * of. A page says its part with aml_downloads_picked_add(), adding when
 * its list grows and passing the negative of what it added when its list
 * shrinks or the page goes away, so that what is shown is what the open
 * pages have on. */
guint aml_downloads_picked (void);

void aml_downloads_picked_add (gint delta);

/* (transfer none) every running download, in the order it started. Never
 * NULL; do not modify. */
GPtrArray *aml_downloads_get_all (void);

const char *aml_download_get_title (AmlDownload *self);
const char *aml_download_get_detail (AmlDownload *self);
AmlDownloadKind aml_download_get_kind (AmlDownload *self);
AmlDownloadUnit aml_download_get_unit (AmlDownload *self);
guint64 aml_download_get_done (AmlDownload *self);
guint64 aml_download_get_total (AmlDownload *self);

G_END_DECLS

#endif /* AML_DOWNLOADS_H */
