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
} nuvashalauncherLanguage;

const nuvashalauncherLanguage *nuvashalauncher_i18n_languages (gsize *length);
const char *nuvashalauncher_i18n_language_name (const char *code);
void nuvashalauncher_i18n_init (const char *override_code);

G_END_DECLS

#endif
