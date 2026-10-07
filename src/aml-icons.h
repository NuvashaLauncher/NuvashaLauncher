/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlIcons: the icon of a project, fetched once and kept in a cache.
 *
 * A list of search results shows an icon for every row. Fetching them
 * with the same download module the rest of AML uses keeps the retry and
 * rate limit rules in one place, and keeping them under the user's cache
 * directory means a project is downloaded once and then read from disk.
 *
 * Nothing here blocks: a row gets a widget at once and the image appears
 * in it when it arrives. A project with no icon, or one that could not
 * be fetched, keeps the placeholder the caller put there. */

#ifndef AML_ICONS_H
#define AML_ICONS_H

#include <adwaita.h>

G_BEGIN_DECLS

/* The square every icon of a list is shown in. */
#define AML_ICON_SIZE 64

/* A square widget for one row: it shows @placeholder until an icon is
 * put in it. Nothing is downloaded here. */
GtkWidget *aml_icon_widget_new (const char *placeholder_icon_name);

/* Puts @path in @widget, scaled to fit the square and never stretched. */
void aml_icon_widget_set_file (GtkWidget *widget, const char *path);

/* Fetches @url into the icon cache and calls @callback when it is there.
 * The answer is the path of the cached file, or NULL when there is no
 * url, the download failed or it was cancelled. The callback is handed
 * the widget to put it in and may be called after the widget is gone;
 * the caller must therefore pass a widget it keeps alive for the length
 * of the call, or check it in the callback.
 *
 * (transfer full) the path, which the caller frees. */
void aml_icon_load_async (const char        *url,
                          GCancellable      *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer           user_data);

/* (transfer full). NULL when there is nothing to show. */
char *aml_icon_load_finish (GAsyncResult *result, GError **error);

/* The directory the icons are kept in, for a test to look at.
 * (transfer full) */
char *aml_icon_cache_directory (void);

G_END_DECLS

#endif /* AML_ICONS_H */
