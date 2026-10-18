/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* LeppoisaInstanceManager: owns the instance list and keeps it in
 * <data dir>/instances.json.
 *
 * The object exposes a GListStore so that a UI can bind to it later.
 * No widgets are created here. */

#ifndef leppoisa_INSTANCE_MANAGER_H
#define leppoisa_INSTANCE_MANAGER_H

#include "leppoisa-instance.h"
#include "leppoisa-task.h"

G_BEGIN_DECLS

#define leppoisa_TYPE_INSTANCE_MANAGER (leppoisa_instance_manager_get_type ())

G_DECLARE_FINAL_TYPE (LeppoisaInstanceManager, leppoisa_instance_manager,
                      leppoisa, INSTANCE_MANAGER, GObject)

#define leppoisa_INSTANCE_MANAGER_ERROR (leppoisa_instance_manager_error_quark ())

typedef enum {
  leppoisa_INSTANCE_MANAGER_ERROR_NOT_FOUND,  /* unknown instance */
  leppoisa_INSTANCE_MANAGER_ERROR_INVALID_PATH, /* unusable directory */
  leppoisa_INSTANCE_MANAGER_ERROR_CORRUPT,    /* instances.json unreadable */
  leppoisa_INSTANCE_MANAGER_ERROR_IO          /* file could not be read or written */
} LeppoisaInstanceManagerError;

GQuark leppoisa_instance_manager_error_quark (void);

/* @data_dir is the directory holding instances.json and the "instances"
 * subdirectory; NULL means g_get_user_data_dir()/leppoisa. Nothing is read
 * yet: call leppoisa_instance_manager_load() to fill the list. */
LeppoisaInstanceManager *leppoisa_instance_manager_new (const char *data_dir);

/* Reads instances.json. A missing file starts with an empty list and
 * succeeds; a corrupt file is renamed to instances.json.corrupt (it is
 * never deleted), the list starts empty and the error is reported. */
gboolean leppoisa_instance_manager_load (LeppoisaInstanceManager *self,
                                    GError **error);

/* Writes instances.json atomically: the data goes to a temporary file
 * in the same directory, is flushed and fsynced and then renamed over
 * the old file, so a failed or interrupted save cannot corrupt it. */
gboolean leppoisa_instance_manager_save (LeppoisaInstanceManager *self,
                                    GError **error);

/* Owned by the manager, do not unref: it is a GListStore of
 * LeppoisaInstance. */
GListStore *leppoisa_instance_manager_get_instances (LeppoisaInstanceManager *self);

 /* The directory a new instance of @name would land in when the caller
  * names none: <data dir>/instances/<unique name>. If @name is already in
  * use, the next available numeric suffix is used. (transfer full) */
char *leppoisa_instance_manager_default_directory (LeppoisaInstanceManager *self,
                                              const char *name);

/* The registered instance with this id, or NULL. Borrowed from the
 * store. */
LeppoisaInstance *leppoisa_instance_manager_get_by_id (LeppoisaInstanceManager *self,
                                             const char *id);

 /* Creates an instance and registers it. @directory is the directory the
  * user picked; NULL derives it from the unique name under
  * <data dir>/instances/<name>. If the requested name is already in use,
  * a numeric suffix is added. The directory must be
 * absolute, must not contain ".." and must neither be, contain nor lie
 * inside the directory of another instance. Returns the new instance
 * (transfer full). */
LeppoisaInstance *leppoisa_instance_manager_create (LeppoisaInstanceManager *self,
                                          const char *name,
                                          const char *directory,
                                          const char *minecraft_version,
                                          LeppoisaLoader loader,
                                          const char *loader_version,
                                          GError **error);

/* Changes the display name and saves. Other fields can be edited by
 * setting the properties of the LeppoisaInstance and calling
 * leppoisa_instance_manager_save(). */
gboolean leppoisa_instance_manager_rename (LeppoisaInstanceManager *self,
                                      LeppoisaInstance *instance,
                                      const char *new_name,
                                      GError **error);

/* Atomically updates one validated string property and persists it. */
gboolean leppoisa_instance_manager_set_string (LeppoisaInstanceManager *self,
                                          LeppoisaInstance *instance,
                                          const char *property,
                                          const char *value,
                                          GError **error);
gboolean leppoisa_instance_manager_set_loader (LeppoisaInstanceManager *self,
                                          LeppoisaInstance *instance,
                                          LeppoisaLoader loader,
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
gboolean leppoisa_instance_manager_check_new_directory (LeppoisaInstanceManager *self,
                                                   LeppoisaInstance *instance,
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
LeppoisaTaskScope *leppoisa_instance_manager_change_directory_async (LeppoisaInstanceManager *self,
                                                            LeppoisaInstance *instance,
                                                            const char *directory,
                                                            GAsyncReadyCallback callback,
                                                            gpointer user_data);

gboolean leppoisa_instance_manager_change_directory_finish (LeppoisaInstanceManager *self,
                                                        GAsyncResult *result,
                                                        GError **error);

/* Copies the files of @source into a fresh directory, registers the
 * copy as "Copy of <name>" with a new id and saves. The copy appears in
 * the list only after its files were copied. */
LeppoisaTaskScope *leppoisa_instance_manager_duplicate_async (LeppoisaInstanceManager *self,
                                                     LeppoisaInstance *source,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);

LeppoisaInstance *leppoisa_instance_manager_duplicate_finish (LeppoisaInstanceManager *self,
                                                    GAsyncResult *result,
                                                    GError **error);

/* Removes the instance. The directory is only removed when
 * @delete_directory is TRUE; the metadata is removed after the
 * directory went away, so a failed deletion keeps the instance
 * registered. */
LeppoisaTaskScope *leppoisa_instance_manager_delete_async (LeppoisaInstanceManager *self,
                                                  LeppoisaInstance *instance,
                                                  gboolean delete_directory,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);

gboolean leppoisa_instance_manager_delete_finish (LeppoisaInstanceManager *self,
                                             GAsyncResult *result,
                                             GError **error);

/* Opens the instance directory with the default file manager. */
LeppoisaTaskScope *leppoisa_instance_manager_open_folder_async (LeppoisaInstanceManager *self,
                                                       LeppoisaInstance *instance,
                                                       GAsyncReadyCallback callback,
                                                       gpointer user_data);

gboolean leppoisa_instance_manager_open_folder_finish (LeppoisaInstanceManager *self,
                                                  GAsyncResult *result,
                                                  GError **error);

G_END_DECLS

#endif /* leppoisa_INSTANCE_MANAGER_H */
