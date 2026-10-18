/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaDownloadModpackDialog: browsing Modrinth for modpacks and putting
 * one into a new instance.
 *
 * The flow is a navigation view with a Browse page, a Details page and
 * an Installing page, plus an error page the flow falls onto and a
 * page with the pack's optional files when it lists any. Nothing the
 * pack needs for its Minecraft version or its loader is asked of the
 * user: both come from what the pack says about itself. */

#ifndef leppoisa_DOWNLOAD_MODPACK_DIALOG_H
#define leppoisa_DOWNLOAD_MODPACK_DIALOG_H

#include <adwaita.h>

#include "leppoisa-instance-manager.h"

G_BEGIN_DECLS

#define leppoisa_TYPE_DOWNLOAD_MODPACK_DIALOG \
  (leppoisa_download_modpack_dialog_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaDownloadModpackDialog,
                      leppoisa_download_modpack_dialog, leppoisa,
                      DOWNLOAD_MODPACK_DIALOG, AdwDialog)

/* The dialog is handed the manager; it keeps its own reference so that
 * an install that outlives the closing of the window is still talking
 * to a live manager. */
LeppoisaDownloadModpackDialog *leppoisa_download_modpack_dialog_new (
    LeppoisaInstanceManager *manager);

LeppoisaDownloadModpackDialog *leppoisa_download_modpack_dialog_new_for_file (
    LeppoisaInstanceManager *manager, const char *path);

G_END_DECLS

#endif /* leppoisa_DOWNLOAD_MODPACK_DIALOG_H */
