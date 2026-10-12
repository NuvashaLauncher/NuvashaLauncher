/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-downloads.h"
#include "aml-download.h"

struct _AmlDownload
{
  GObject parent_instance;

  AmlDownloadKind kind;
  AmlDownloadUnit unit;
  char *title;
  char *detail;
  guint64 done;
  guint64 total;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (AmlDownload, aml_download, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _AmlDownloads
{
  GObject parent_instance;

  GPtrArray *jobs; /* AmlDownload*, owned */
  AmlDownloadService *service;

  /* How many files the user has picked for a later install, summed over
     every content page that is open. It is not a download and it does
     not stop anything: it is what the header counts on the icon of the
     downloads button. */
  guint picked;
};

G_DEFINE_FINAL_TYPE (AmlDownloads, aml_downloads, G_TYPE_OBJECT)

static AmlDownloads *default_downloads = NULL;

AmlDownloads *
aml_downloads_get_default (void)
{
  if (default_downloads == NULL)
    default_downloads = g_object_new (AML_TYPE_DOWNLOADS, NULL);

  return default_downloads;
}

AmlDownloadService *
aml_downloads_get_service (void)
{
  AmlDownloads *self = aml_downloads_get_default ();

  if (self->service == NULL)
    self->service = aml_download_service_new ();
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
aml_download_get_title (AmlDownload *self)
{
  g_return_val_if_fail (AML_IS_DOWNLOAD (self), NULL);

  return self->title;
}

const char *
aml_download_get_detail (AmlDownload *self)
{
  g_return_val_if_fail (AML_IS_DOWNLOAD (self), NULL);

  return self->detail;
}

AmlDownloadKind
aml_download_get_kind (AmlDownload *self)
{
  g_return_val_if_fail (AML_IS_DOWNLOAD (self), AML_DOWNLOAD_MOD);

  return self->kind;
}

AmlDownloadUnit
aml_download_get_unit (AmlDownload *self)
{
  g_return_val_if_fail (AML_IS_DOWNLOAD (self), AML_DOWNLOAD_UNIT_COUNT);

  return self->unit;
}

guint64
aml_download_get_done (AmlDownload *self)
{
  g_return_val_if_fail (AML_IS_DOWNLOAD (self), 0);

  return self->done;
}

guint64
aml_download_get_total (AmlDownload *self)
{
  g_return_val_if_fail (AML_IS_DOWNLOAD (self), 0);

  return self->total;
}

static void
aml_download_dispose (GObject *object)
{
  AmlDownload *self = AML_DOWNLOAD (object);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (aml_download_parent_class)->dispose (object);
}

static void
aml_download_finalize (GObject *object)
{
  AmlDownload *self = AML_DOWNLOAD (object);

  g_free (self->title);
  g_free (self->detail);

  G_OBJECT_CLASS (aml_download_parent_class)->finalize (object);
}

static void
aml_download_class_init (AmlDownloadClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_download_dispose;
  object_class->finalize = aml_download_finalize;
}

static void
aml_download_init (AmlDownload *self)
{
  (void) self;
}

GCancellable *
aml_download_get_cancellable (AmlDownload *self)
{
  g_return_val_if_fail (AML_IS_DOWNLOAD (self), NULL);

  return self->cancellable;
}

void
aml_download_set_detail (AmlDownload *self, const char *detail)
{
  g_return_if_fail (AML_IS_DOWNLOAD (self));

  if (g_strcmp0 (self->detail, detail) == 0)
    return;

  g_free (self->detail);
  self->detail = g_strdup (detail);

  emit_changed ();
}

void
aml_download_set_progress (AmlDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (AML_IS_DOWNLOAD (self));

  if (self->unit == AML_DOWNLOAD_UNIT_COUNT && self->done == done &&
      self->total == total)
    return;

  self->unit = AML_DOWNLOAD_UNIT_COUNT;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
aml_download_set_bytes (AmlDownload *self, guint64 done, guint64 total)
{
  g_return_if_fail (AML_IS_DOWNLOAD (self));

  if (self->unit == AML_DOWNLOAD_UNIT_BYTES && self->done == done &&
      self->total == total)
    return;

  self->unit = AML_DOWNLOAD_UNIT_BYTES;
  self->done = done;
  self->total = total;

  emit_changed ();
}

void
aml_download_cancel (AmlDownload *self)
{
  g_return_if_fail (AML_IS_DOWNLOAD (self));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
}

/* ---------------------------------------------------------- *
 * The manager
 * ---------------------------------------------------------- */

AmlDownload *
aml_downloads_add (AmlDownloadKind kind, const char *title)
{
  AmlDownloads *self = aml_downloads_get_default ();
  AmlDownload *download = g_object_new (AML_TYPE_DOWNLOAD, NULL);

  download->kind = kind;
  download->title = g_strdup (title != NULL ? title : "Download");
  download->cancellable = g_cancellable_new ();

  g_ptr_array_add (self->jobs, download);
  emit_changed ();

  return download;
}

void
aml_download_finish (AmlDownload *self)
{
  g_return_if_fail (AML_IS_DOWNLOAD (self));

  if (default_downloads == NULL)
    return;

  if (g_ptr_array_remove (default_downloads->jobs, self))
    emit_changed ();
}

guint
aml_downloads_count (void)
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
aml_downloads_picked (void)
{
  if (default_downloads == NULL)
    return 0;

  return default_downloads->picked;
}

void
aml_downloads_picked_add (gint delta)
{
  AmlDownloads *self = aml_downloads_get_default ();
  gint picked = (gint) self->picked + delta;

  if (picked < 0)
    picked = 0;

  if ((guint) picked == self->picked)
    return;

  self->picked = (guint) picked;
  emit_changed ();
}

GPtrArray *
aml_downloads_get_all (void)
{
  return aml_downloads_get_default ()->jobs;
}

static void
aml_downloads_dispose (GObject *object)
{
  AmlDownloads *self = AML_DOWNLOADS (object);

  aml_download_service_free (self->service);
  self->service = NULL;
  g_clear_pointer (&self->jobs, g_ptr_array_unref);

  G_OBJECT_CLASS (aml_downloads_parent_class)->dispose (object);
}

static void
aml_downloads_class_init (AmlDownloadsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = aml_downloads_dispose;

  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
aml_downloads_init (AmlDownloads *self)
{
  self->jobs = g_ptr_array_new_with_free_func (g_object_unref);
}
