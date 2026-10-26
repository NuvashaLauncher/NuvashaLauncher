/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_CONTENT_BROWSER_MODEL_H
#define nuvashalauncher_CONTENT_BROWSER_MODEL_H

#include <gio/gio.h>

#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-task.h"

typedef struct _nuvashalauncherContentBrowserModel nuvashalauncherContentBrowserModel;

nuvashalauncherContentBrowserModel *nuvashalauncher_content_browser_model_new (void);
void nuvashalauncher_content_browser_model_free (nuvashalauncherContentBrowserModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (nuvashalauncherContentBrowserModel,
                               nuvashalauncher_content_browser_model_free)

struct _nuvashalauncherContentBrowserModel
{
  GHashTable *projects;             /* project id -> NULL */
  GHashTable *project_icons;        /* project id -> icon URL */
  GHashTable *project_descriptions; /* project id -> description */
  GHashTable *project_titles;       /* project id -> title */
  char *query;
  GPtrArray *categories;            /* char* */
  GPtrArray *results;               /* nuvashalauncherModrinthProject* */
  guint offset;
  guint64 total_hits;
  gboolean categories_loaded;
  gboolean searched;
  gboolean loading_page;
  gboolean pending_append;
  GHashTable *seen_ids;
  GCancellable *more;
  guint debounce_source;
  nuvashalauncherTaskScope *scope;
  GCancellable *cancellable;
};

#endif /* nuvashalauncher_CONTENT_BROWSER_MODEL_H */
