/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaInstanceManager: owns the instance list and keeps it in
 * <data dir>/instances.json.
 *
 * The object exposes a GListStore so that a UI can bind to it later.
 * No widgets are created here. */

#ifndef nuvasha_INSTANCE_MANAGER_H
#define nuvasha_INSTANCE_MANAGER_H

#include "nuvasha-instance.h"
#include "nuvasha-task.h"

G_BEGIN_DECLS

#define nuvasha_TYPE_INSTANCE_MANAGER (nuvasha_instance_manager_get_type ())

G_DECLARE_FINAL_TYPE (NuvashaInstanceManager, nuvasha_instance_manager,
                      nuvasha, INSTANCE_MANAGER, GObject)

#define nuvasha_INSTANCE_MANAGER_ERROR (nuvasha_instance_manager_error_quark ())

typedef enum {
  nuvasha_INSTANCE_MANAGER_ERROR_NOT_FOUND,  /* unknown instance */
  nuvasha_INSTANCE_MANAGER_ERROR_INVALID_PATH, /* unusable directory */
  nuvasha_INSTANCE_MANAGER_ERROR_CORRUPT,    /* instances.json unreadable */
  nuvasha_INSTANCE_MANAGER_ERROR_IO          /* file could not be read or written */
} NuvashaInstanceManagerError;

GQuark nuvasha_instance_manager_error_quark (void);

/* @data_dir is the directory holding instances.json and the "instances"
 * subdirectory; NULL means g_get_user_data_dir()/nuvasha. Nothing is read
 * yet: call nuvasha_instance_manager_load() to fill the list. */
NuvashaInstanceManager *nuvasha_instance_manager_new (const char *data_dir);

/* Reads instances.json. A missing file starts with an empty list and
 * succeeds; a corrupt file is renamed to instances.json.corrupt (it is
 * never deleted), the list starts empty and the error is reported. */
gboolean nuvasha_instance_manager_load (NuvashaInstanceManager *self,
                                    GError **error);

/* Writes instances.json atomically: the data goes to a temporary file
 * in the same directory, is flushed and fsynced and then renamed over
 * the old file, so a failed or interrupted save cannot corrupt it. */
gboolean nuvasha_instance_manager_save (NuvashaInstanceManager *self,
                                    GError **error);

/* Owned by the manager, do not unref: it is a GListStore of
 * NuvashaInstance. */
GListStore *nuvasha_instance_manager_get_instances (NuvashaInstanceManager *self);

 /* The directory a new instance of @name would land in when the caller
  * names none: <data dir>/instances/<unique name>. If @name is already in
  * use, the next available numeric suffix is used. (transfer full) */
char *nuvasha_instance_manager_default_directory (NuvashaInstanceManager *self,
                                              const char *name);

/* The registered instance with this id, or NULL. Borrowed from the
 * store. */
NuvashaInstance *nuvasha_instance_manager_get_by_id (NuvashaInstanceManager *self,
                                             const char *id);

 /* Creates an instance and registers it. @directory is the directory the
  * user picked; NULL derives it from the unique name under
  * <data dir>/instances/<name>. If the requested name is already in use,
  * a numeric suffix is added. The directory must be
 * absolute, must not contain ".." and must neither be, contain nor lie
 * inside the directory of another instance. Returns the new instance
 * (transfer full). */
NuvashaInstance *nuvasha_instance_manager_create (NuvashaInstanceManager *self,
                                          const char *name,
                                          const char *directory,
                                          const char *minecraft_version,
                                          NuvashaLoader loader,
                                          const char *loader_version,
                                          GError **error);

/* Changes the display name and saves. Other fields can be edited by
 * setting the properties of the NuvashaInstance and calling
 * nuvasha_instance_manager_save(). */
gboolean nuvasha_instance_manager_rename (NuvashaInstanceManager *self,
                                      NuvashaInstance *instance,
                                      const char *new_name,
                                      GError **error);

/* Atomically updates one validated string property and persists it. */
gboolean nuvasha_instance_manager_set_string (NuvashaInstanceManager *self,
                                          NuvashaInstance *instance,
                                          const char *property,
                                          const char *value,
                                          GError **error);
gboolean nuvasha_instance_manager_set_loader (NuvashaInstanceManager *self,
                                          NuvashaInstance *instance,
                                          NuvashaLoader loader,
                                          const char *version,
                                          GError **error);

/* Check whether @directory may become the directory of @instance, and
 * puts the canonical form of it in @canonical when that is not NULL.
 *
 * It must be absolute, must not contain "..", must be neither the
 * filesystem root nor the directory the instance already has, must not
 * be, contain or lie inside the directory of any other instance, must
 * not exist yet, and the directory it hangs off must exist. This is the
 * all checks happen before any file is touched. */
gboolean nuvasha_instance_manager_check_new_directory (NuvashaInstanceManager *self,
                                                   NuvashaInstance *instance,
                                                   const char *directory,
                                                   char **canonical,
                                                   GError **error);

/* Moves the files of @instance to @directory and stores the new path.
 *
 * The files are copied first and the metadata is written whole only once
 * they are all in place, so a failure at any point leaves the old
 * directory and the stored path as they were; whatever reached the new
 * directory is removed again. The old directory is deleted last, and
 * failing to delete it leaves a leftover rather than taking anything
 * away, so it does not undo the change.
 *
 * An instance whose old directory does not exist has nothing to move: the
 * stored path is changed and saved, and that is all. */
NuvashaTaskScope *nuvasha_instance_manager_change_directory_async (NuvashaInstanceManager *self,
                                                            NuvashaInstance *instance,
                                                            const char *directory,
                                                            GAsyncReadyCallback callback,
                                                            gpointer user_data);

gboolean nuvasha_instance_manager_change_directory_finish (NuvashaInstanceManager *self,
                                                        GAsyncResult *result,
                                                        GError **error);

/* Copies the files of @source into a fresh directory, registers the
 * copy as "Copy of <name>" with a new id and saves. The copy appears in
 * the list only after its files were copied. */
NuvashaTaskScope *nuvasha_instance_manager_duplicate_async (NuvashaInstanceManager *self,
                                                     NuvashaInstance *source,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);

NuvashaInstance *nuvasha_instance_manager_duplicate_finish (NuvashaInstanceManager *self,
                                                    GAsyncResult *result,
                                                    GError **error);

/* Removes the instance. The directory is only removed when
 * @delete_directory is TRUE; the metadata is removed after the
 * directory went away, so a failed deletion keeps the instance
 * registered. */
NuvashaTaskScope *nuvasha_instance_manager_delete_async (NuvashaInstanceManager *self,
                                                  NuvashaInstance *instance,
                                                  gboolean delete_directory,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);

gboolean nuvasha_instance_manager_delete_finish (NuvashaInstanceManager *self,
                                             GAsyncResult *result,
                                             GError **error);

/* Opens the instance directory with the default file manager. */
NuvashaTaskScope *nuvasha_instance_manager_open_folder_async (NuvashaInstanceManager *self,
                                                       NuvashaInstance *instance,
                                                       GAsyncReadyCallback callback,
                                                       gpointer user_data);

gboolean nuvasha_instance_manager_open_folder_finish (NuvashaInstanceManager *self,
                                                  GAsyncResult *result,
                                                  GError **error);

G_END_DECLS

#endif /* nuvasha_INSTANCE_MANAGER_H */
