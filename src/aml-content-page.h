/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlContentPage: the page of one kind of content of one instance.
 *
 * There is one of these for mods, for resource packs and for shader
 * packs, and the content type is what makes them different: the folder
 * of the instance, whether a loader is asked for, the extensions, the
 * title and the icon. Nothing else is written twice.
 *
 * The page has two halves. Installed is what the instance folder holds,
 * read from the folder itself: it needs no network and keeps working
 * without one, except for the names Modrinth knows a file by. Browse is
 * Modrinth, narrowed to the Minecraft version and the loader of the
 * instance, so nothing incompatible can be offered at all.
 *
 * What a user asks for is installed with the download module, which
 * verifies the bytes against the hash Modrinth published and only then
 * puts the file in place. The dependencies a version requires are
 * fetched and shown before anything is written. */

#ifndef AML_CONTENT_PAGE_H
#define AML_CONTENT_PAGE_H

#include <adwaita.h>

#include "aml-instance.h"
#include "aml-modrinth.h"
#include "aml-task.h"

G_BEGIN_DECLS

#define AML_TYPE_CONTENT_PAGE (aml_content_page_get_type ())

G_DECLARE_FINAL_TYPE (AmlContentPage, aml_content_page, AML, CONTENT_PAGE,
                      GtkBox)

void aml_content_page_reset_for_tab_switch (AmlContentPage *self);


/* What the loaders and the game ignore: a file whose name ends with
 * this is not loaded, and taking it off the name loads it again. */
#define AML_CONTENT_DISABLED_SUFFIX ".disabled"

/* One file of the instance folder of this content, and what Modrinth
 * knows about it. */
typedef struct {
  char *path;      /* the file as it is now, suffix included */
  char *filename;  /* without the suffix, which is what a row shows */
  char *sha1;      /* the hash of the bytes, or NULL while unknown */
  char *sha512;    /* Modrinth hash when the index knows it */
  char *download_url; /* Modrinth source URL when the index knows it */
  gboolean enabled;

  /* What a hash lookup answered, all NULL for a file Modrinth does not
   * know or while the network is not there. */
  char *project_id;
  char *local_id;
  char *project_name;
  char *search_key;  /* casefolded title and filename for installed search */
  char *description;
  gboolean local_metadata;
  char *icon_url;
  char *local_icon_path;
  char *version_id;
  char *version_name;
  char *version_number;
  GPtrArray *required_dependencies; /* project ids this file requires */
  GPtrArray *optional_dependencies;
  GPtrArray *incompatible_dependencies;
} AmlContentFile;

void aml_content_file_free (AmlContentFile *file);
void aml_content_file_refresh_search_key (AmlContentFile *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AmlContentFile, aml_content_file_free)

/* The folder of @instance_directory that @type lives in. (transfer
 * full) */
char *aml_content_folder_path (const char    *instance_directory,
                               AmlContentType type);

/* The name @filename has when the content is enabled or disabled, which
 * is @filename with the ".disabled" suffix put on or taken off.
 * (transfer full) */
char *aml_content_disabled_name (const char *filename, gboolean enabled);

/* Renames the file at @path so that it is disabled or enabled again,
 * and hands the name it has afterwards back in @new_path when it is not
 * NULL. (transfer full) */
char *aml_content_set_disabled (const char  *path,
                                gboolean     disabled,
                                GError     **error);
gboolean aml_content_remove_file (const char *path, GError **error);
gboolean aml_content_ensure_folder (const char *path, GError **error);
void aml_content_delete_async (GFile *file, int io_priority,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean aml_content_delete_finish (GFile *file, GAsyncResult *result,
                                    GError **error);

/* The files of @instance_directory for @type, sorted by name, each one
 * hashed. A folder that does not exist holds no files, which is what an
 * instance that never installed anything of this kind looks like.
 *
 * (transfer full) the AmlContentFile*. */
AmlTaskScope *aml_content_scan_async (const char         *instance_directory,
                                      AmlContentType      type,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data);

/* (transfer full) the AmlContentFile*. NULL on error. */
GPtrArray *aml_content_scan_finish (GAsyncResult  *result,
                                    GError       **error);

/* The page for @type of @instance. It keeps its own reference on the
 * instance until it is destroyed. Like every widget it starts with a
 * floating reference. */
AmlContentPage *aml_content_page_new (AmlInstance     *instance,
                                      AmlContentType  type);

G_END_DECLS

#endif /* AML_CONTENT_PAGE_H */
