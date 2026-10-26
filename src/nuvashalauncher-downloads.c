/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-downloads.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-i18n.h"

struct _nuvashalauncherDownload
{
  GObject parent_instance;

  nuvashalauncherDownloadKind kind;
  nuvashalauncherDownloadUnit unit;
  char *title;
  char *detail;
  guint64 done;
  guint64 total;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherDownload, nuvashalauncher_download, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _nuvashalauncherDownloads
{
  GObject parent_instance;

  GPtrArray *jobs; /* nuvashalauncherDownload*, owned */
  nuvashalauncherDownloadService *service;

  /* How many files the user has picked for a later install, summed over
     every content page that is open. It is not a download and it does
     not stop anything: it is what the header counts on the icon of the
     downloads button. */
  guint picked;
};

G_DEFINE_FINAL_TYPE (nuvashalauncherDownloads, nuvashalauncher_downloads, G_TYPE_OBJECT)

static nuvashalauncherDownloads *default_downloads = NULL;

nuvashalauncherDownloads *
nuvashalauncher_downloads_get_default (void)
{
  if (default_downloads == NULL)
    default_downloads = g_object_new (nuvashalauncher_TYPE_DOWNLOADS, NULL);

  return default_downloads;
}

nuvashalauncherDownloadService *
nuvashalauncher_downloads_get_service (void)
{
  nuvashalauncherDownloads *self = nuvashalauncher_downloads_get_default ();

  if (self->service == NULL)
    self->service = nuvashalauncher_download_service_new ();
  return self->service;
}

static void
emit_changed (void)
{
  if (default_downloads != NULL)
    g_signal_emit (default_downloads, signals[SIGNAL_CHANGED], 0);
}

/* ---------------------------------------------------------- *
 * One download
 * ---------------------------------------------------------- */

const char *
nuvashalauncher_download_get_title (nuvashalauncherDownload *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_DOWNLOAD (self), NULL);

  return self->title;
}

const char *
nuvashalauncher_download_get_detail (nuvashalauncherDownload *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_DOWNLOAD (self), NULL);

  return self->detail;
}

nuvashalauncherDownloadKind
nuvashalauncher_download_get_kind (nuvashalauncherDownload *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_DOWNLOAD (self), nuvashalauncher_DOWNLOAD_MOD);

  return self->kind;
}

nuvashalauncherDownloadUnit
nuvashalauncher_download_get_unit (nuvashalauncherDownload *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_DOWNLOAD (self), nuvashalauncher_DOWNLOAD_UNIT_COUNT);

  return self->unit;
}

guint64
nuvashalauncher_download_get_done (nuvashalauncherDownload *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_DOWNLOAD (self), 0);

  return self->done;
}

guint64
nuvashalauncher_download_get_total (nuvashalauncherDownload *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_DOWNLOAD (self), 0);

  return self->total;
}

static void
nuvashalauncher_download_dispose (GObject *object)
{
  nuvashalauncherDownload *self = nuvashalauncher_DOWNLOAD (object);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (nuvashalauncher_download_parent_class)->dispose (object);
}

static void
nuvashalauncher_download_finalize (GObject *object)
{
  nuvashalauncherDownload *self = nuvashalauncher_DOWNLOAD (object);

  g_free (self->title);
  g_free (self->detail);

  G_OBJECT_CLASS (nuvashalauncher_download_parent_class)->finalize (object);
}

static void
nuvashalauncher_download_class_init (nuvashalauncherDownloadClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_download_dispose;
  object_class->finalize = nuvashalauncher_download_finalize;
}

static void
nuvashalauncher_download_init (nuvashalauncherDownload *self)
{
  (void) self;
}

GCancellable *
nuvashalauncher_download_get_cancellable (nuvashalauncherDownload *self)
{
  g_return_val_if_fail (nuvashalauncher_IS_DOWNLOAD (self), NULL);

  return self->cancellable;
}

void
nuvashalauncher_download_set_detail (nuvashalauncherDownload *self, const char *detail)
{
  g_return_if_fail (nuvashalauncher_IS_DOWNLOAD (self));

  if (g_strcmp0 (self->detail, detail) == 0)
    return;

  g_free (self->detail);
  self->detail = g_strdup (detail);

  emit_changed ();
}

void
nuvashalauncher_download_set_progress (nuvashalauncherDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (nuvashalauncher_IS_DOWNLOAD (self));

  if (self->unit == nuvashalauncher_DOWNLOAD_UNIT_COUNT && self->done == done &&
      self->total == total)
    return;

  self->unit = nuvashalauncher_DOWNLOAD_UNIT_COUNT;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
nuvashalauncher_download_set_bytes (nuvashalauncherDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (nuvashalauncher_IS_DOWNLOAD (self));

  if (self->unit == nuvashalauncher_DOWNLOAD_UNIT_BYTES && self->done == done &&
      self->total == total)
    return;

  self->unit = nuvashalauncher_DOWNLOAD_UNIT_BYTES;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
nuvashalauncher_download_cancel (nuvashalauncherDownload *self)
{
  g_return_if_fail (nuvashalauncher_IS_DOWNLOAD (self));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
}

/* ---------------------------------------------------------- *
 * The manager
 * ---------------------------------------------------------- */

nuvashalauncherDownload *
nuvashalauncher_downloads_add (nuvashalauncherDownloadKind kind, const char *title)
{
  nuvashalauncherDownloads *self = nuvashalauncher_downloads_get_default ();
  nuvashalauncherDownload *download = g_object_new (nuvashalauncher_TYPE_DOWNLOAD, NULL);

  download->kind = kind;
  download->title = g_strdup (title != NULL ? title : _("Download"));
  download->cancellable = g_cancellable_new ();

  g_ptr_array_add (self->jobs, download);
  emit_changed ();

  return download;
}

void
nuvashalauncher_download_finish (nuvashalauncherDownload *self)
{
  g_return_if_fail (nuvashalauncher_IS_DOWNLOAD (self));

  if (default_downloads == NULL)
    return;

  if (g_ptr_array_remove (default_downloads->jobs, self))
    emit_changed ();
}

guint
nuvashalauncher_downloads_count (void)
{
  if (default_downloads == NULL)
    return 0;

  return default_downloads->jobs->len;
}

/* What the header shows on the icon of the downloads button: how many
   files are picked across every content page and installed with one
   press of the button at the foot of their list. A page says its part
   when what it has on changes and when it goes away, so the count of a
   page that took its picks with it drops out on its own. */
guint
nuvashalauncher_downloads_picked (void)
{
  if (default_downloads == NULL)
    return 0;

  return default_downloads->picked;
}

void
nuvashalauncher_downloads_picked_add (gint delta)
{
  nuvashalauncherDownloads *self = nuvashalauncher_downloads_get_default ();
  gint picked = (gint) self->picked + delta;

  if (picked < 0)
    picked = 0;

  if ((guint) picked == self->picked)
    return;

  self->picked = (guint) picked;
  emit_changed ();
}

GPtrArray *
nuvashalauncher_downloads_get_all (void)
{
  return nuvashalauncher_downloads_get_default ()->jobs;
}

static void
nuvashalauncher_downloads_dispose (GObject *object)
{
  nuvashalauncherDownloads *self = nuvashalauncher_DOWNLOADS (object);

  nuvashalauncher_download_service_free (self->service);
  self->service = NULL;
  g_clear_pointer (&self->jobs, g_ptr_array_unref);

  G_OBJECT_CLASS (nuvashalauncher_downloads_parent_class)->dispose (object);
}

static void
nuvashalauncher_downloads_class_init (nuvashalauncherDownloadsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvashalauncher_downloads_dispose;

  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nuvashalauncher_downloads_init (nuvashalauncherDownloads *self)
{
  self->jobs = g_ptr_array_new_with_free_func (g_object_unref);
}
