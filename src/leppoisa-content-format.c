/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-content-format.h"
#include "leppoisa-i18n.h"

char *
leppoisa_content_format_downloads (guint64 downloads)
{
  g_autofree char *count = NULL;

  if (downloads >= 1000000)
    return g_strdup_printf (_("%.1fM downloads"), downloads / 1000000.0);

  if (downloads >= 1000)
    return g_strdup_printf (_("%.1fk downloads"), downloads / 1000.0);

  count = g_strdup_printf ("%" G_GUINT64_FORMAT, downloads);
  return g_strdup_printf (_("%s downloads"), count);
}

char *
leppoisa_content_format_author (const char *license, const char *author)
{
  const char *safe_author = author != NULL ? author : "";

  if (license != NULL && *license != '\0')
    return g_strdup_printf (_("%s  ·  by %s"), license, safe_author);

  return g_strdup_printf (_("by %s"), safe_author);
}
