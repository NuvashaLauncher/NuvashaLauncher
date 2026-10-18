/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_CONTENT_BROWSER_MODEL_H
#define comfy_CONTENT_BROWSER_MODEL_H

#include <gio/gio.h>

#include "comfy-modrinth.h"
#include "comfy-task.h"

typedef struct _ComfyContentBrowserModel ComfyContentBrowserModel;

ComfyContentBrowserModel *comfy_content_browser_model_new (void);
void comfy_content_browser_model_free (ComfyContentBrowserModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyContentBrowserModel,
                               comfy_content_browser_model_free)

struct _ComfyContentBrowserModel
{
  GHashTable *projects;             /* project id -> NULL */
  GHashTable *project_icons;        /* project id -> icon URL */
  GHashTable *project_descriptions; /* project id -> description */
  GHashTable *project_titles;       /* project id -> title */
  char *query;
  GPtrArray *categories;            /* char* */
  GPtrArray *results;               /* ComfyModrinthProject* */
  guint offset;
  guint64 total_hits;
  gboolean categories_loaded;
  gboolean searched;
  gboolean loading_page;
  gboolean pending_append;
  GHashTable *seen_ids;
  GCancellable *more;
  guint debounce_source;
  ComfyTaskScope *scope;
  GCancellable *cancellable;
};

#endif /* comfy_CONTENT_BROWSER_MODEL_H */
