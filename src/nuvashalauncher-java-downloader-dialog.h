/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherJavaDownloaderDialog: dialog for downloading Eclipse Temurin
 * JDK builds matching the host Linux architecture into the launcher's Java
 * directory. */

#ifndef NUVASHALAUNCHER_JAVA_DOWNLOADER_DIALOG_H
#define NUVASHALAUNCHER_JAVA_DOWNLOADER_DIALOG_H

#include <adwaita.h>

#include "nuvashalauncher-java-row.h"

G_BEGIN_DECLS

#define NUVASHALAUNCHER_TYPE_JAVA_DOWNLOADER_DIALOG (nuvashalauncher_java_downloader_dialog_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaLauncherJavaDownloaderDialog,
                      nuvashalauncher_java_downloader_dialog,
                      NUVASHALAUNCHER, JAVA_DOWNLOADER_DIALOG,
                      AdwDialog)

/* Creates the Java downloader dialog. If @java_row is non-NULL, it is
 * reloaded automatically when a JDK finishes installing (even if the dialog
 * was closed while the download continued in the background). */
NuvashaLauncherJavaDownloaderDialog *nuvashalauncher_java_downloader_dialog_new (
  NuvashaLauncherJavaRow *java_row);

G_END_DECLS

#endif /* NUVASHALAUNCHER_JAVA_DOWNLOADER_DIALOG_H */
