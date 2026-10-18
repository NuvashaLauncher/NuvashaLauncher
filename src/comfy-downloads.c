/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-downloads.h"
#include "comfy-download.h"

struct _ComfyDownload
{
  GObject parent_instance;

  ComfyDownloadKind kind;
  ComfyDownloadUnit unit;
  char *title;
  char *detail;
  guint64 done;
  guint64 total;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (ComfyDownload, comfy_download, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ComfyDownloads
{
  GObject parent_instance;

  GPtrArray *jobs; /* ComfyDownload*, owned */
  ComfyDownloadService *service;

  /* How many files the user has picked for a later install, summed over
     every content page that is open. It is not a download and it does
     not stop anything: it is what the header counts on the icon of the
     downloads button. */
  guint picked;
};

G_DEFINE_FINAL_TYPE (ComfyDownloads, comfy_downloads, G_TYPE_OBJECT)

static ComfyDownloads *default_downloads = NULL;

ComfyDownloads *
comfy_downloads_get_default (void)
{
  if (default_downloads == NULL)
    default_downloads = g_object_new (comfy_TYPE_DOWNLOADS, NULL);

  return default_downloads;
}

ComfyDownloadService *
comfy_downloads_get_service (void)
{
  ComfyDownloads *self = comfy_downloads_get_default ();

  if (self->service == NULL)
    self->service = comfy_download_service_new ();
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
comfy_download_get_title (ComfyDownload *self)
{
  g_return_val_if_fail (comfy_IS_DOWNLOAD (self), NULL);

  return self->title;
}

const char *
comfy_download_get_detail (ComfyDownload *self)
{
  g_return_val_if_fail (comfy_IS_DOWNLOAD (self), NULL);

  return self->detail;
}

ComfyDownloadKind
comfy_download_get_kind (ComfyDownload *self)
{
  g_return_val_if_fail (comfy_IS_DOWNLOAD (self), comfy_DOWNLOAD_MOD);

  return self->kind;
}

ComfyDownloadUnit
comfy_download_get_unit (ComfyDownload *self)
{
  g_return_val_if_fail (comfy_IS_DOWNLOAD (self), comfy_DOWNLOAD_UNIT_COUNT);

  return self->unit;
}

guint64
comfy_download_get_done (ComfyDownload *self)
{
  g_return_val_if_fail (comfy_IS_DOWNLOAD (self), 0);

  return self->done;
}

guint64
comfy_download_get_total (ComfyDownload *self)
{
  g_return_val_if_fail (comfy_IS_DOWNLOAD (self), 0);

  return self->total;
}

static void
comfy_download_dispose (GObject *object)
{
  ComfyDownload *self = comfy_DOWNLOAD (object);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (comfy_download_parent_class)->dispose (object);
}

static void
comfy_download_finalize (GObject *object)
{
  ComfyDownload *self = comfy_DOWNLOAD (object);

  g_free (self->title);
  g_free (self->detail);

  G_OBJECT_CLASS (comfy_download_parent_class)->finalize (object);
}

static void
comfy_download_class_init (ComfyDownloadClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = comfy_download_dispose;
  object_class->finalize = comfy_download_finalize;
}

static void
comfy_download_init (ComfyDownload *self)
{
  (void) self;
}

GCancellable *
comfy_download_get_cancellable (ComfyDownload *self)
{
  g_return_val_if_fail (comfy_IS_DOWNLOAD (self), NULL);

  return self->cancellable;
}

void
comfy_download_set_detail (ComfyDownload *self, const char *detail)
{
  g_return_if_fail (comfy_IS_DOWNLOAD (self));

  if (g_strcmp0 (self->detail, detail) == 0)
    return;

  g_free (self->detail);
  self->detail = g_strdup (detail);

  emit_changed ();
}

void
comfy_download_set_progress (ComfyDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (comfy_IS_DOWNLOAD (self));

  if (self->unit == comfy_DOWNLOAD_UNIT_COUNT && self->done == done &&
      self->total == total)
    return;

  self->unit = comfy_DOWNLOAD_UNIT_COUNT;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
comfy_download_set_bytes (ComfyDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (comfy_IS_DOWNLOAD (self));

  if (self->unit == comfy_DOWNLOAD_UNIT_BYTES && self->done == done &&
      self->total == total)
    return;

  self->unit = comfy_DOWNLOAD_UNIT_BYTES;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
comfy_download_cancel (ComfyDownload *self)
{
  g_return_if_fail (comfy_IS_DOWNLOAD (self));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
}

/* ---------------------------------------------------------- *
 * The manager
 * ---------------------------------------------------------- */

ComfyDownload *
comfy_downloads_add (ComfyDownloadKind kind, const char *title)
{
  ComfyDownloads *self = comfy_downloads_get_default ();
  ComfyDownload *download = g_object_new (comfy_TYPE_DOWNLOAD, NULL);

  download->kind = kind;
  download->title = g_strdup (title != NULL ? title : "Download");
  download->cancellable = g_cancellable_new ();

  g_ptr_array_add (self->jobs, download);
  emit_changed ();

  return download;
}

void
comfy_download_finish (ComfyDownload *self)
{
  g_return_if_fail (comfy_IS_DOWNLOAD (self));

  if (default_downloads == NULL)
    return;

  if (g_ptr_array_remove (default_downloads->jobs, self))
    emit_changed ();
}

guint
comfy_downloads_count (void)
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
comfy_downloads_picked (void)
{
  if (default_downloads == NULL)
    return 0;

  return default_downloads->picked;
}

void
comfy_downloads_picked_add (gint delta)
{
  ComfyDownloads *self = comfy_downloads_get_default ();
  gint picked = (gint) self->picked + delta;

  if (picked < 0)
    picked = 0;

  if ((guint) picked == self->picked)
    return;

  self->picked = (guint) picked;
  emit_changed ();
}

GPtrArray *
comfy_downloads_get_all (void)
{
  return comfy_downloads_get_default ()->jobs;
}

static void
comfy_downloads_dispose (GObject *object)
{
  ComfyDownloads *self = comfy_DOWNLOADS (object);

  comfy_download_service_free (self->service);
  self->service = NULL;
  g_clear_pointer (&self->jobs, g_ptr_array_unref);

  G_OBJECT_CLASS (comfy_downloads_parent_class)->dispose (object);
}

static void
comfy_downloads_class_init (ComfyDownloadsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = comfy_downloads_dispose;

  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
comfy_downloads_init (ComfyDownloads *self)
{
  self->jobs = g_ptr_array_new_with_free_func (g_object_unref);
}
