/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-downloads.h"
#include "nuvasha-download.h"

struct _NuvashaDownload
{
  GObject parent_instance;

  NuvashaDownloadKind kind;
  NuvashaDownloadUnit unit;
  char *title;
  char *detail;
  guint64 done;
  guint64 total;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (NuvashaDownload, nuvasha_download, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _NuvashaDownloads
{
  GObject parent_instance;

  GPtrArray *jobs; /* NuvashaDownload*, owned */
  NuvashaDownloadService *service;

  /* How many files the user has picked for a later install, summed over
     every content page that is open. It is not a download and it does
     not stop anything: it is what the header counts on the icon of the
     downloads button. */
  guint picked;
};

G_DEFINE_FINAL_TYPE (NuvashaDownloads, nuvasha_downloads, G_TYPE_OBJECT)

static NuvashaDownloads *default_downloads = NULL;

NuvashaDownloads *
nuvasha_downloads_get_default (void)
{
  if (default_downloads == NULL)
    default_downloads = g_object_new (nuvasha_TYPE_DOWNLOADS, NULL);

  return default_downloads;
}

NuvashaDownloadService *
nuvasha_downloads_get_service (void)
{
  NuvashaDownloads *self = nuvasha_downloads_get_default ();

  if (self->service == NULL)
    self->service = nuvasha_download_service_new ();
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
nuvasha_download_get_title (NuvashaDownload *self)
{
  g_return_val_if_fail (nuvasha_IS_DOWNLOAD (self), NULL);

  return self->title;
}

const char *
nuvasha_download_get_detail (NuvashaDownload *self)
{
  g_return_val_if_fail (nuvasha_IS_DOWNLOAD (self), NULL);

  return self->detail;
}

NuvashaDownloadKind
nuvasha_download_get_kind (NuvashaDownload *self)
{
  g_return_val_if_fail (nuvasha_IS_DOWNLOAD (self), nuvasha_DOWNLOAD_MOD);

  return self->kind;
}

NuvashaDownloadUnit
nuvasha_download_get_unit (NuvashaDownload *self)
{
  g_return_val_if_fail (nuvasha_IS_DOWNLOAD (self), nuvasha_DOWNLOAD_UNIT_COUNT);

  return self->unit;
}

guint64
nuvasha_download_get_done (NuvashaDownload *self)
{
  g_return_val_if_fail (nuvasha_IS_DOWNLOAD (self), 0);

  return self->done;
}

guint64
nuvasha_download_get_total (NuvashaDownload *self)
{
  g_return_val_if_fail (nuvasha_IS_DOWNLOAD (self), 0);

  return self->total;
}

static void
nuvasha_download_dispose (GObject *object)
{
  NuvashaDownload *self = nuvasha_DOWNLOAD (object);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (nuvasha_download_parent_class)->dispose (object);
}

static void
nuvasha_download_finalize (GObject *object)
{
  NuvashaDownload *self = nuvasha_DOWNLOAD (object);

  g_free (self->title);
  g_free (self->detail);

  G_OBJECT_CLASS (nuvasha_download_parent_class)->finalize (object);
}

static void
nuvasha_download_class_init (NuvashaDownloadClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_download_dispose;
  object_class->finalize = nuvasha_download_finalize;
}

static void
nuvasha_download_init (NuvashaDownload *self)
{
  (void) self;
}

GCancellable *
nuvasha_download_get_cancellable (NuvashaDownload *self)
{
  g_return_val_if_fail (nuvasha_IS_DOWNLOAD (self), NULL);

  return self->cancellable;
}

void
nuvasha_download_set_detail (NuvashaDownload *self, const char *detail)
{
  g_return_if_fail (nuvasha_IS_DOWNLOAD (self));

  if (g_strcmp0 (self->detail, detail) == 0)
    return;

  g_free (self->detail);
  self->detail = g_strdup (detail);

  emit_changed ();
}

void
nuvasha_download_set_progress (NuvashaDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (nuvasha_IS_DOWNLOAD (self));

  if (self->unit == nuvasha_DOWNLOAD_UNIT_COUNT && self->done == done &&
      self->total == total)
    return;

  self->unit = nuvasha_DOWNLOAD_UNIT_COUNT;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
nuvasha_download_set_bytes (NuvashaDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (nuvasha_IS_DOWNLOAD (self));

  if (self->unit == nuvasha_DOWNLOAD_UNIT_BYTES && self->done == done &&
      self->total == total)
    return;

  self->unit = nuvasha_DOWNLOAD_UNIT_BYTES;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
nuvasha_download_cancel (NuvashaDownload *self)
{
  g_return_if_fail (nuvasha_IS_DOWNLOAD (self));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
}

/* ---------------------------------------------------------- *
 * The manager
 * ---------------------------------------------------------- */

NuvashaDownload *
nuvasha_downloads_add (NuvashaDownloadKind kind, const char *title)
{
  NuvashaDownloads *self = nuvasha_downloads_get_default ();
  NuvashaDownload *download = g_object_new (nuvasha_TYPE_DOWNLOAD, NULL);

  download->kind = kind;
  download->title = g_strdup (title != NULL ? title : "Download");
  download->cancellable = g_cancellable_new ();

  g_ptr_array_add (self->jobs, download);
  emit_changed ();

  return download;
}

void
nuvasha_download_finish (NuvashaDownload *self)
{
  g_return_if_fail (nuvasha_IS_DOWNLOAD (self));

  if (default_downloads == NULL)
    return;

  if (g_ptr_array_remove (default_downloads->jobs, self))
    emit_changed ();
}

guint
nuvasha_downloads_count (void)
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
nuvasha_downloads_picked (void)
{
  if (default_downloads == NULL)
    return 0;

  return default_downloads->picked;
}

void
nuvasha_downloads_picked_add (gint delta)
{
  NuvashaDownloads *self = nuvasha_downloads_get_default ();
  gint picked = (gint) self->picked + delta;

  if (picked < 0)
    picked = 0;

  if ((guint) picked == self->picked)
    return;

  self->picked = (guint) picked;
  emit_changed ();
}

GPtrArray *
nuvasha_downloads_get_all (void)
{
  return nuvasha_downloads_get_default ()->jobs;
}

static void
nuvasha_downloads_dispose (GObject *object)
{
  NuvashaDownloads *self = nuvasha_DOWNLOADS (object);

  nuvasha_download_service_free (self->service);
  self->service = NULL;
  g_clear_pointer (&self->jobs, g_ptr_array_unref);

  G_OBJECT_CLASS (nuvasha_downloads_parent_class)->dispose (object);
}

static void
nuvasha_downloads_class_init (NuvashaDownloadsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nuvasha_downloads_dispose;

  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nuvasha_downloads_init (NuvashaDownloads *self)
{
  self->jobs = g_ptr_array_new_with_free_func (g_object_unref);
}
