/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyInstanceManager: owns the instance list and keeps it in
 * <data dir>/instances.json.
 *
 * The object exposes a GListStore so that a UI can bind to it later.
 * No widgets are created here. */

#ifndef comfy_INSTANCE_MANAGER_H
#define comfy_INSTANCE_MANAGER_H

#include "comfy-instance.h"
#include "comfy-task.h"

G_BEGIN_DECLS

#define comfy_TYPE_INSTANCE_MANAGER (comfy_instance_manager_get_type ())

G_DECLARE_FINAL_TYPE (ComfyInstanceManager, comfy_instance_manager,
                      comfy, INSTANCE_MANAGER, GObject)

#define comfy_INSTANCE_MANAGER_ERROR (comfy_instance_manager_error_quark ())

typedef enum {
  comfy_INSTANCE_MANAGER_ERROR_NOT_FOUND,  /* unknown instance */
  comfy_INSTANCE_MANAGER_ERROR_INVALID_PATH, /* unusable directory */
  comfy_INSTANCE_MANAGER_ERROR_CORRUPT,    /* instances.json unreadable */
  comfy_INSTANCE_MANAGER_ERROR_IO          /* file could not be read or written */
} ComfyInstanceManagerError;

GQuark comfy_instance_manager_error_quark (void);

/* @data_dir is the directory holding instances.json and the "instances"
 * subdirectory; NULL means g_get_user_data_dir()/comfy. Nothing is read
 * yet: call comfy_instance_manager_load() to fill the list. */
ComfyInstanceManager *comfy_instance_manager_new (const char *data_dir);

/* Reads instances.json. A missing file starts with an empty list and
 * succeeds; a corrupt file is renamed to instances.json.corrupt (it is
 * never deleted), the list starts empty and the error is reported. */
gboolean comfy_instance_manager_load (ComfyInstanceManager *self,
                                    GError **error);

/* Writes instances.json atomically: the data goes to a temporary file
 * in the same directory, is flushed and fsynced and then renamed over
 * the old file, so a failed or interrupted save cannot corrupt it. */
gboolean comfy_instance_manager_save (ComfyInstanceManager *self,
                                    GError **error);

/* Owned by the manager, do not unref: it is a GListStore of
 * ComfyInstance. */
GListStore *comfy_instance_manager_get_instances (ComfyInstanceManager *self);

 /* The directory a new instance of @name would land in when the caller
  * names none: <data dir>/instances/<unique name>. If @name is already in
  * use, the next available numeric suffix is used. (transfer full) */
char *comfy_instance_manager_default_directory (ComfyInstanceManager *self,
                                              const char *name);

/* The registered instance with this id, or NULL. Borrowed from the
 * store. */
ComfyInstance *comfy_instance_manager_get_by_id (ComfyInstanceManager *self,
                                             const char *id);

 /* Creates an instance and registers it. @directory is the directory the
  * user picked; NULL derives it from the unique name under
  * <data dir>/instances/<name>. If the requested name is already in use,
  * a numeric suffix is added. The directory must be
 * absolute, must not contain ".." and must neither be, contain nor lie
 * inside the directory of another instance. Returns the new instance
 * (transfer full). */
ComfyInstance *comfy_instance_manager_create (ComfyInstanceManager *self,
                                          const char *name,
                                          const char *directory,
                                          const char *minecraft_version,
                                          ComfyLoader loader,
                                          const char *loader_version,
                                          GError **error);

/* Changes the display name and saves. Other fields can be edited by
 * setting the properties of the ComfyInstance and calling
 * comfy_instance_manager_save(). */
gboolean comfy_instance_manager_rename (ComfyInstanceManager *self,
                                      ComfyInstance *instance,
                                      const char *new_name,
                                      GError **error);

/* Atomically updates one validated string property and persists it. */
gboolean comfy_instance_manager_set_string (ComfyInstanceManager *self,
                                          ComfyInstance *instance,
                                          const char *property,
                                          const char *value,
                                          GError **error);
gboolean comfy_instance_manager_set_loader (ComfyInstanceManager *self,
                                          ComfyInstance *instance,
                                          ComfyLoader loader,
                                          const char *version,
                                          GError **error);

/* Says whether @directory may become the directory of @instance, and
 * puts the canonical form of it in @canonical when that is not NULL.
 *
 * It must be absolute, must not contain "..", must be neither the
 * filesystem root nor the directory the instance already has, must not
 * be, contain or lie inside the directory of any other instance, must
 * not exist yet, and the directory it hangs off must exist. This is the
 * whole of what is checked before any file is touched, so a caller can
 * refuse a bad path without starting a move. */
gboolean comfy_instance_manager_check_new_directory (ComfyInstanceManager *self,
                                                   ComfyInstance *instance,
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
ComfyTaskScope *comfy_instance_manager_change_directory_async (ComfyInstanceManager *self,
                                                            ComfyInstance *instance,
                                                            const char *directory,
                                                            GAsyncReadyCallback callback,
                                                            gpointer user_data);

gboolean comfy_instance_manager_change_directory_finish (ComfyInstanceManager *self,
                                                        GAsyncResult *result,
                                                        GError **error);

/* Copies the files of @source into a fresh directory, registers the
 * copy as "Copy of <name>" with a new id and saves. The copy appears in
 * the list only after its files were copied. */
ComfyTaskScope *comfy_instance_manager_duplicate_async (ComfyInstanceManager *self,
                                                     ComfyInstance *source,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);

ComfyInstance *comfy_instance_manager_duplicate_finish (ComfyInstanceManager *self,
                                                    GAsyncResult *result,
                                                    GError **error);

/* Removes the instance. The directory is only removed when
 * @delete_directory is TRUE; the metadata is removed after the
 * directory went away, so a failed deletion keeps the instance
 * registered. */
ComfyTaskScope *comfy_instance_manager_delete_async (ComfyInstanceManager *self,
                                                  ComfyInstance *instance,
                                                  gboolean delete_directory,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);

gboolean comfy_instance_manager_delete_finish (ComfyInstanceManager *self,
                                             GAsyncResult *result,
                                             GError **error);

/* Opens the instance directory with the default file manager. */
ComfyTaskScope *comfy_instance_manager_open_folder_async (ComfyInstanceManager *self,
                                                       ComfyInstance *instance,
                                                       GAsyncReadyCallback callback,
                                                       gpointer user_data);

gboolean comfy_instance_manager_open_folder_finish (ComfyInstanceManager *self,
                                                  GAsyncResult *result,
                                                  GError **error);

G_END_DECLS

#endif /* comfy_INSTANCE_MANAGER_H */
