/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_I18N_H
#define nuvasha_I18N_H

#include <glib.h>

#include <libintl.h>

G_BEGIN_DECLS

#define _(message) gettext (message)

typedef struct {
  const char *code;
  const char *name;
} NuvashaLanguage;

const NuvashaLanguage *nuvasha_i18n_languages (gsize *length);
const char *nuvasha_i18n_language_name (const char *code);
void nuvasha_i18n_init (const char *override_code);

G_END_DECLS

#endif
