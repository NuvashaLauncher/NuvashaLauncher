/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_CONTENT_BROWSER_MODEL_H
#define AML_CONTENT_BROWSER_MODEL_H

#include <gio/gio.h>

#include "aml-modrinth.h"

typedef struct _AmlContentBrowserModel AmlContentBrowserModel;

AmlContentBrowserModel *aml_content_browser_model_new (void);
void aml_content_browser_model_free (AmlContentBrowserModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlContentBrowserModel,
                               aml_content_browser_model_free)

struct _AmlContentBrowserModel
{
  GHashTable *projects;             /* project id -> NULL */
  GHashTable *project_icons;        /* project id -> icon URL */
  GHashTable *project_descriptions; /* project id -> description */
  GHashTable *project_titles;       /* project id -> title */
  char *query;
  GPtrArray *categories;            /* char* */
  GPtrArray *results;               /* AmlModrinthProject* */
  guint offset;
  guint64 total_hits;
  gboolean categories_loaded;
  gboolean searched;
  gboolean loading_page;
  gboolean pending_append;
  GHashTable *seen_ids;
  GCancellable *more;
  guint debounce_source;
  GCancellable *cancellable;
};

#endif /* AML_CONTENT_BROWSER_MODEL_H */
