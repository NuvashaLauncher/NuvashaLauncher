/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_I18N_H
#define nuvashalauncher_I18N_H

#include <glib.h>

#include <libintl.h>

G_BEGIN_DECLS

#define _(message) gettext (message)

typedef struct {
  const char *code;
  const char *name;
} NuvashaLauncherLanguage;

const NuvashaLauncherLanguage *nuvashalauncher_i18n_languages (gsize *length);
void nuvashalauncher_i18n_init (const char *override_code);
void nuvashalauncher_i18n_template_strings (void);

G_END_DECLS

#endif
