/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherJavaDownload: discovers and installs Eclipse Temurin JDK
 * releases from the Adoptium v3 API for the host Linux architecture. */

#ifndef NUVASHALAUNCHER_JAVA_DOWNLOAD_H
#define NUVASHALAUNCHER_JAVA_DOWNLOAD_H

#include <gio/gio.h>

#include "nuvashalauncher-download.h"
#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

#define NUVASHALAUNCHER_ADOPTIUM_API_BASE "https://api.adoptium.net/v3"

#define NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR (nuvashalauncher_java_download_error_quark ())

typedef enum {
  NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_UNSUPPORTED_ARCH,
  NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_FORMAT,
  NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
  NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_IO,
  NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_CANCELLED
} NuvashaLauncherJavaDownloadError;

GQuark nuvashalauncher_java_download_error_quark (void);

typedef enum {
  NUVASHALAUNCHER_JAVA_DOWNLOAD_STAGE_DOWNLOADING,
  NUVASHALAUNCHER_JAVA_DOWNLOAD_STAGE_EXTRACTING
} NuvashaLauncherJavaDownloadStage;

typedef struct {
  int major_version;
  gboolean lts;
  char *release_name;    /* e.g. "jdk-21.0.5+11" */
  char *version_display; /* e.g. "21.0.5+11" */
  char *package_name;    /* e.g. "OpenJDK21U-jdk_x64_linux_hotspot_21.0.5_11.tar.gz" */
  char *download_url;
  char *sha256;
  guint64 size;
  char *arch;            /* Adoptium architecture string, e.g. "x64", "aarch64" */
} NuvashaLauncherJavaRelease;

NuvashaLauncherJavaRelease *nuvashalauncher_java_release_copy (const NuvashaLauncherJavaRelease *release);
void nuvashalauncher_java_release_free (NuvashaLauncherJavaRelease *release);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherJavaRelease, nuvashalauncher_java_release_free)

/* Returns the Adoptium architecture string for the host Linux OS
 * ("x64" on x86_64 or "aarch64" on ARM64), or NULL when the host
 * architecture is unsupported. */
const char *nuvashalauncher_java_download_host_arch (void);

/* Returns the sanitized directory name under the launcher's Java directory
 * where @release is installed, e.g. "temurin-jdk-21.0.5+11". Caller owns. */
char *nuvashalauncher_java_download_install_dir_name (const NuvashaLauncherJavaRelease *release);

/* TRUE when @release is already installed in @target_dir (or
 * nuvashalauncher_java_get_dir() when @target_dir is NULL). */
gboolean nuvashalauncher_java_download_is_installed (const NuvashaLauncherJavaRelease *release,
                                                     const char *target_dir);

/* Fetches the latest Eclipse Temurin JDK release for each available major
 * version matching the host Linux architecture. @api_base may be NULL to use
 * NUVASHALAUNCHER_ADOPTIUM_API_BASE. */
NuvashaLauncherTaskScope *nuvashalauncher_java_download_list_async (
  NuvashaLauncherDownloadService *service,
  const char *api_base,
  GAsyncReadyCallback callback,
  gpointer user_data);

/* (transfer full) GPtrArray of NuvashaLauncherJavaRelease*, sorted by
 * major_version descending. NULL on error. */
GPtrArray *nuvashalauncher_java_download_list_finish (GAsyncResult *result,
                                                      GError **error);

typedef void (*NuvashaLauncherJavaDownloadProgressFunc) (
  NuvashaLauncherJavaDownloadStage stage,
  goffset bytes_done,
  goffset bytes_total,
  gpointer user_data);

/* Downloads, verifies (SHA256), and extracts @release into @target_dir (or
 * nuvashalauncher_java_get_dir() when @target_dir is NULL). */
NuvashaLauncherTaskScope *nuvashalauncher_java_download_install_async (
  NuvashaLauncherDownloadService *service,
  const NuvashaLauncherJavaRelease *release,
  const char *target_dir,
  NuvashaLauncherJavaDownloadProgressFunc progress,
  GAsyncReadyCallback callback,
  gpointer user_data);

/* (transfer full) path to the installed java executable, or NULL on error. */
char *nuvashalauncher_java_download_install_finish (GAsyncResult *result,
                                                    GError **error);

G_END_DECLS

#endif /* NUVASHALAUNCHER_JAVA_DOWNLOAD_H */
