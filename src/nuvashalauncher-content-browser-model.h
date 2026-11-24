/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_CONTENT_BROWSER_MODEL_H
#define NUVASHALAUNCHER_CONTENT_BROWSER_MODEL_H

#include <gio/gio.h>

#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

typedef struct _NuvashaLauncherContentBrowserModel NuvashaLauncherContentBrowserModel;

NuvashaLauncherContentBrowserModel *nuvashalauncher_content_browser_model_new (void);
void nuvashalauncher_content_browser_model_free (NuvashaLauncherContentBrowserModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherContentBrowserModel,
                               nuvashalauncher_content_browser_model_free)

struct _NuvashaLauncherContentBrowserModel
{
  GHashTable *projects;             /* project id -> NULL */
  GHashTable *project_icons;        /* project id -> icon URL */
  GHashTable *project_descriptions; /* project id -> description */
  GHashTable *project_titles;       /* project id -> title */
  char *query;
  GPtrArray *categories;            /* char* */
  GPtrArray *results;               /* NuvashaLauncherModrinthProject* */
  guint offset;
  guint64 total_hits;
  gboolean categories_loaded;
  gboolean searched;
  gboolean loading_page;
  gboolean pending_append;
  GHashTable *seen_ids;
  GCancellable *more;
  guint debounce_source;
  NuvashaLauncherTaskScope *scope;
  GCancellable *cancellable;
};

GCancellable *nuvashalauncher_content_browser_model_begin_search (
  NuvashaLauncherContentBrowserModel *model,
  gboolean                            append);
gboolean nuvashalauncher_content_browser_model_finish_page (
  NuvashaLauncherContentBrowserModel *model,
  guint64                             total_hits);
gboolean nuvashalauncher_content_browser_model_fail_page (
  NuvashaLauncherContentBrowserModel *model);
const NuvashaLauncherModrinthProject *nuvashalauncher_content_browser_model_accept_project (
  NuvashaLauncherContentBrowserModel *model,
  NuvashaLauncherModrinthProject     *project);
void nuvashalauncher_content_browser_model_advance_offset (
  NuvashaLauncherContentBrowserModel *model,
  guint                               shown);
gboolean nuvashalauncher_content_browser_model_has_more (
  const NuvashaLauncherContentBrowserModel *model);
void nuvashalauncher_content_browser_model_set_query (
  NuvashaLauncherContentBrowserModel *model,
  const char                         *query);
GCancellable *nuvashalauncher_content_browser_model_reset_cancellable (
  NuvashaLauncherContentBrowserModel *model);

G_END_DECLS

#endif /* NUVASHALAUNCHER_CONTENT_BROWSER_MODEL_H */
