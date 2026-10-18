/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvasha_CONTENT_BROWSER_MODEL_H
#define nuvasha_CONTENT_BROWSER_MODEL_H

#include <gio/gio.h>

#include "nuvasha-modrinth.h"
#include "nuvasha-task.h"

typedef struct _NuvashaContentBrowserModel NuvashaContentBrowserModel;

NuvashaContentBrowserModel *nuvasha_content_browser_model_new (void);
void nuvasha_content_browser_model_free (NuvashaContentBrowserModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaContentBrowserModel,
                               nuvasha_content_browser_model_free)

struct _NuvashaContentBrowserModel
{
  GHashTable *projects;             /* project id -> NULL */
  GHashTable *project_icons;        /* project id -> icon URL */
  GHashTable *project_descriptions; /* project id -> description */
  GHashTable *project_titles;       /* project id -> title */
  char *query;
  GPtrArray *categories;            /* char* */
  GPtrArray *results;               /* NuvashaModrinthProject* */
  guint offset;
  guint64 total_hits;
  gboolean categories_loaded;
  gboolean searched;
  gboolean loading_page;
  gboolean pending_append;
  GHashTable *seen_ids;
  GCancellable *more;
  guint debounce_source;
  NuvashaTaskScope *scope;
  GCancellable *cancellable;
};

#endif /* nuvasha_CONTENT_BROWSER_MODEL_H */
