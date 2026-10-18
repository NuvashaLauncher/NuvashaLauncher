/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_CONTENT_BROWSER_MODEL_H
#define leppoisa_CONTENT_BROWSER_MODEL_H

#include <gio/gio.h>

#include "leppoisa-modrinth.h"
#include "leppoisa-task.h"

typedef struct _LeppoisaContentBrowserModel LeppoisaContentBrowserModel;

LeppoisaContentBrowserModel *leppoisa_content_browser_model_new (void);
void leppoisa_content_browser_model_free (LeppoisaContentBrowserModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LeppoisaContentBrowserModel,
                               leppoisa_content_browser_model_free)

struct _LeppoisaContentBrowserModel
{
  GHashTable *projects;             /* project id -> NULL */
  GHashTable *project_icons;        /* project id -> icon URL */
  GHashTable *project_descriptions; /* project id -> description */
  GHashTable *project_titles;       /* project id -> title */
  char *query;
  GPtrArray *categories;            /* char* */
  GPtrArray *results;               /* LeppoisaModrinthProject* */
  guint offset;
  guint64 total_hits;
  gboolean categories_loaded;
  gboolean searched;
  gboolean loading_page;
  gboolean pending_append;
  GHashTable *seen_ids;
  GCancellable *more;
  guint debounce_source;
  LeppoisaTaskScope *scope;
  GCancellable *cancellable;
};

#endif /* leppoisa_CONTENT_BROWSER_MODEL_H */
