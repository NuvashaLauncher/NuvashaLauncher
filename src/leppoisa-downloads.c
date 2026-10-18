/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-downloads.h"
#include "leppoisa-download.h"

struct _LeppoisaDownload
{
  GObject parent_instance;

  LeppoisaDownloadKind kind;
  LeppoisaDownloadUnit unit;
  char *title;
  char *detail;
  guint64 done;
  guint64 total;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (LeppoisaDownload, leppoisa_download, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _LeppoisaDownloads
{
  GObject parent_instance;

  GPtrArray *jobs; /* LeppoisaDownload*, owned */
  LeppoisaDownloadService *service;

  /* How many files the user has picked for a later install, summed over
     every content page that is open. It is not a download and it does
     not stop anything: it is what the header counts on the icon of the
     downloads button. */
  guint picked;
};

G_DEFINE_FINAL_TYPE (LeppoisaDownloads, leppoisa_downloads, G_TYPE_OBJECT)

static LeppoisaDownloads *default_downloads = NULL;

LeppoisaDownloads *
leppoisa_downloads_get_default (void)
{
  if (default_downloads == NULL)
    default_downloads = g_object_new (leppoisa_TYPE_DOWNLOADS, NULL);

  return default_downloads;
}

LeppoisaDownloadService *
leppoisa_downloads_get_service (void)
{
  LeppoisaDownloads *self = leppoisa_downloads_get_default ();

  if (self->service == NULL)
    self->service = leppoisa_download_service_new ();
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
leppoisa_download_get_title (LeppoisaDownload *self)
{
  g_return_val_if_fail (leppoisa_IS_DOWNLOAD (self), NULL);

  return self->title;
}

const char *
leppoisa_download_get_detail (LeppoisaDownload *self)
{
  g_return_val_if_fail (leppoisa_IS_DOWNLOAD (self), NULL);

  return self->detail;
}

LeppoisaDownloadKind
leppoisa_download_get_kind (LeppoisaDownload *self)
{
  g_return_val_if_fail (leppoisa_IS_DOWNLOAD (self), leppoisa_DOWNLOAD_MOD);

  return self->kind;
}

LeppoisaDownloadUnit
leppoisa_download_get_unit (LeppoisaDownload *self)
{
  g_return_val_if_fail (leppoisa_IS_DOWNLOAD (self), leppoisa_DOWNLOAD_UNIT_COUNT);

  return self->unit;
}

guint64
leppoisa_download_get_done (LeppoisaDownload *self)
{
  g_return_val_if_fail (leppoisa_IS_DOWNLOAD (self), 0);

  return self->done;
}

guint64
leppoisa_download_get_total (LeppoisaDownload *self)
{
  g_return_val_if_fail (leppoisa_IS_DOWNLOAD (self), 0);

  return self->total;
}

static void
leppoisa_download_dispose (GObject *object)
{
  LeppoisaDownload *self = leppoisa_DOWNLOAD (object);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (leppoisa_download_parent_class)->dispose (object);
}

static void
leppoisa_download_finalize (GObject *object)
{
  LeppoisaDownload *self = leppoisa_DOWNLOAD (object);

  g_free (self->title);
  g_free (self->detail);

  G_OBJECT_CLASS (leppoisa_download_parent_class)->finalize (object);
}

static void
leppoisa_download_class_init (LeppoisaDownloadClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = leppoisa_download_dispose;
  object_class->finalize = leppoisa_download_finalize;
}

static void
leppoisa_download_init (LeppoisaDownload *self)
{
  (void) self;
}

GCancellable *
leppoisa_download_get_cancellable (LeppoisaDownload *self)
{
  g_return_val_if_fail (leppoisa_IS_DOWNLOAD (self), NULL);

  return self->cancellable;
}

void
leppoisa_download_set_detail (LeppoisaDownload *self, const char *detail)
{
  g_return_if_fail (leppoisa_IS_DOWNLOAD (self));

  if (g_strcmp0 (self->detail, detail) == 0)
    return;

  g_free (self->detail);
  self->detail = g_strdup (detail);

  emit_changed ();
}

void
leppoisa_download_set_progress (LeppoisaDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (leppoisa_IS_DOWNLOAD (self));

  if (self->unit == leppoisa_DOWNLOAD_UNIT_COUNT && self->done == done &&
      self->total == total)
    return;

  self->unit = leppoisa_DOWNLOAD_UNIT_COUNT;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
leppoisa_download_set_bytes (LeppoisaDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (leppoisa_IS_DOWNLOAD (self));

  if (self->unit == leppoisa_DOWNLOAD_UNIT_BYTES && self->done == done &&
      self->total == total)
    return;

  self->unit = leppoisa_DOWNLOAD_UNIT_BYTES;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
leppoisa_download_cancel (LeppoisaDownload *self)
{
  g_return_if_fail (leppoisa_IS_DOWNLOAD (self));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
}

/* ---------------------------------------------------------- *
 * The manager
 * ---------------------------------------------------------- */

LeppoisaDownload *
leppoisa_downloads_add (LeppoisaDownloadKind kind, const char *title)
{
  LeppoisaDownloads *self = leppoisa_downloads_get_default ();
  LeppoisaDownload *download = g_object_new (leppoisa_TYPE_DOWNLOAD, NULL);

  download->kind = kind;
  download->title = g_strdup (title != NULL ? title : "Download");
  download->cancellable = g_cancellable_new ();

  g_ptr_array_add (self->jobs, download);
  emit_changed ();

  return download;
}

void
leppoisa_download_finish (LeppoisaDownload *self)
{
  g_return_if_fail (leppoisa_IS_DOWNLOAD (self));

  if (default_downloads == NULL)
    return;

  if (g_ptr_array_remove (default_downloads->jobs, self))
    emit_changed ();
}

guint
leppoisa_downloads_count (void)
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
leppoisa_downloads_picked (void)
{
  if (default_downloads == NULL)
    return 0;

  return default_downloads->picked;
}

void
leppoisa_downloads_picked_add (gint delta)
{
  LeppoisaDownloads *self = leppoisa_downloads_get_default ();
  gint picked = (gint) self->picked + delta;

  if (picked < 0)
    picked = 0;

  if ((guint) picked == self->picked)
    return;

  self->picked = (guint) picked;
  emit_changed ();
}

GPtrArray *
leppoisa_downloads_get_all (void)
{
  return leppoisa_downloads_get_default ()->jobs;
}

static void
leppoisa_downloads_dispose (GObject *object)
{
  LeppoisaDownloads *self = leppoisa_DOWNLOADS (object);

  leppoisa_download_service_free (self->service);
  self->service = NULL;
  g_clear_pointer (&self->jobs, g_ptr_array_unref);

  G_OBJECT_CLASS (leppoisa_downloads_parent_class)->dispose (object);
}

static void
leppoisa_downloads_class_init (LeppoisaDownloadsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = leppoisa_downloads_dispose;

  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
leppoisa_downloads_init (LeppoisaDownloads *self)
{
  self->jobs = g_ptr_array_new_with_free_func (g_object_unref);
}
