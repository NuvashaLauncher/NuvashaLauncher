/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaIcons: the icon of a project, fetched once and kept in a cache.
 *
 * A list of search results shows an icon for every row. Fetching them
 * with the same download module the rest of nuvasha uses keeps the retry and
 * rate limit rules in one place, and keeping them under the user's cache
 * directory means a project is downloaded once and then read from disk.
 *
 * Nothing here blocks: a row gets a widget at once and the image appears
 * in it when it arrives. A project with no icon, or one that could not
 * be fetched, keeps the placeholder the caller put there. */

#ifndef nuvasha_ICONS_H
#define nuvasha_ICONS_H

#include <adwaita.h>

#include "nuvasha-task.h"

G_BEGIN_DECLS

/* The square every icon of a list is shown in. */
#define nuvasha_ICON_SIZE 64

/* A square widget for one row: it shows @placeholder until an icon is
 * put in it. Nothing is downloaded here. */
GtkWidget *nuvasha_icon_widget_new (const char *placeholder_icon_name);

/* Puts @path in @widget, scaled to fit the square and never stretched.
 * TRUE when the file could be decoded and is now shown, FALSE when it
 * could not, in which case @widget keeps the placeholder it already
 * has: an image the toolkit cannot read must not become a blank hole. */
gboolean nuvasha_icon_widget_set_file (GtkWidget *widget, const char *path);

/* Replaces @path with a 64x64-or-smaller PNG suitable for the icon cache. */
gboolean nuvasha_icon_scale_file (const char *path);

/* Fetches @url into the icon cache and calls @callback when it is there.
 * The answer is the path of the cached file, or NULL when there is no
 * url, the download failed or it was cancelled. The callback is handed
 * the widget to put it in and may be called after the widget is gone;
 * the caller must therefore pass a widget it keeps alive for the length
 * of the call, or check it in the callback.
 *
 * The callback never runs before this returns, so the caller can store
 * the operation handle here and release it from the callback.
 *
 * (transfer full) the operation, released with nuvasha_task_scope_unref().
 *
 * (transfer full) the path, which the caller frees. */
NuvashaTaskScope *nuvasha_icon_load_async (const char        *url,
                                   GAsyncReadyCallback callback,
                                   gpointer           user_data);

/* (transfer full). NULL when no icon is available. */
char *nuvasha_icon_load_finish (GAsyncResult *result, GError **error);

/* The directory the icons are kept in, for a test to look at.
 * (transfer full) */
char *nuvasha_icon_cache_directory (void);

G_END_DECLS

#endif /* nuvasha_ICONS_H */
