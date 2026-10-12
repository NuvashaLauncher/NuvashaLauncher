/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlInstanceManager: owns the instance list and keeps it in
 * <data dir>/instances.json.
 *
 * The object exposes a GListStore so that a UI can bind to it later.
 * No widgets are created here. */

#ifndef AML_INSTANCE_MANAGER_H
#define AML_INSTANCE_MANAGER_H

#include "aml-instance.h"
#include "aml-task.h"

G_BEGIN_DECLS

#define AML_TYPE_INSTANCE_MANAGER (aml_instance_manager_get_type ())

G_DECLARE_FINAL_TYPE (AmlInstanceManager, aml_instance_manager,
                      AML, INSTANCE_MANAGER, GObject)

#define AML_INSTANCE_MANAGER_ERROR (aml_instance_manager_error_quark ())

typedef enum {
  AML_INSTANCE_MANAGER_ERROR_NOT_FOUND,  /* unknown instance */
  AML_INSTANCE_MANAGER_ERROR_INVALID_PATH, /* unusable directory */
  AML_INSTANCE_MANAGER_ERROR_CORRUPT,    /* instances.json unreadable */
  AML_INSTANCE_MANAGER_ERROR_IO          /* file could not be read or written */
} AmlInstanceManagerError;

GQuark aml_instance_manager_error_quark (void);

/* @data_dir is the directory holding instances.json and the "instances"
 * subdirectory; NULL means g_get_user_data_dir()/AML. Nothing is read
 * yet: call aml_instance_manager_load() to fill the list. */
AmlInstanceManager *aml_instance_manager_new (const char *data_dir);

/* Reads instances.json. A missing file starts with an empty list and
 * succeeds; a corrupt file is renamed to instances.json.corrupt (it is
 * never deleted), the list starts empty and the error is reported. */
gboolean aml_instance_manager_load (AmlInstanceManager *self,
                                    GError **error);

/* Writes instances.json atomically: the data goes to a temporary file
 * in the same directory, is flushed and fsynced and then renamed over
 * the old file, so a failed or interrupted save cannot corrupt it. */
gboolean aml_instance_manager_save (AmlInstanceManager *self,
                                    GError **error);

/* Owned by the manager, do not unref: it is a GListStore of
 * AmlInstance. */
GListStore *aml_instance_manager_get_instances (AmlInstanceManager *self);

/* The directory a new instance of @name would land in when the caller
 * names none: <data dir>/instances/<name>-<short id>, with a fresh id.
 * (transfer full) */
char *aml_instance_manager_default_directory (AmlInstanceManager *self,
                                              const char *name);

/* The registered instance with this id, or NULL. Borrowed from the
 * store. */
AmlInstance *aml_instance_manager_get_by_id (AmlInstanceManager *self,
                                             const char *id);

/* Creates an instance and registers it. @directory is the directory the
 * user picked; NULL derives it from the name under
 * <data dir>/instances/<name>-<short id>. The directory must be
 * absolute, must not contain ".." and must neither be, contain nor lie
 * inside the directory of another instance. Returns the new instance
 * (transfer full). */
AmlInstance *aml_instance_manager_create (AmlInstanceManager *self,
                                          const char *name,
                                          const char *directory,
                                          const char *minecraft_version,
                                          AmlLoader loader,
                                          const char *loader_version,
                                          GError **error);

/* Changes the display name and saves. Other fields can be edited by
 * setting the properties of the AmlInstance and calling
 * aml_instance_manager_save(). */
gboolean aml_instance_manager_rename (AmlInstanceManager *self,
                                      AmlInstance *instance,
                                      const char *new_name,
                                      GError **error);

/* Atomically updates one validated string property and persists it. */
gboolean aml_instance_manager_set_string (AmlInstanceManager *self,
                                          AmlInstance *instance,
                                          const char *property,
                                          const char *value,
                                          GError **error);
gboolean aml_instance_manager_set_loader (AmlInstanceManager *self,
                                          AmlInstance *instance,
                                          AmlLoader loader,
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
gboolean aml_instance_manager_check_new_directory (AmlInstanceManager *self,
                                                   AmlInstance *instance,
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
AmlTaskScope *aml_instance_manager_change_directory_async (AmlInstanceManager *self,
                                                            AmlInstance *instance,
                                                            const char *directory,
                                                            GAsyncReadyCallback callback,
                                                            gpointer user_data);

gboolean aml_instance_manager_change_directory_finish (AmlInstanceManager *self,
                                                        GAsyncResult *result,
                                                        GError **error);

/* Copies the files of @source into a fresh directory, registers the
 * copy as "Copy of <name>" with a new id and saves. The copy appears in
 * the list only after its files were copied. */
AmlTaskScope *aml_instance_manager_duplicate_async (AmlInstanceManager *self,
                                                     AmlInstance *source,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);

AmlInstance *aml_instance_manager_duplicate_finish (AmlInstanceManager *self,
                                                    GAsyncResult *result,
                                                    GError **error);

/* Removes the instance. The directory is only removed when
 * @delete_directory is TRUE; the metadata is removed after the
 * directory went away, so a failed deletion keeps the instance
 * registered. */
AmlTaskScope *aml_instance_manager_delete_async (AmlInstanceManager *self,
                                                  AmlInstance *instance,
                                                  gboolean delete_directory,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);

gboolean aml_instance_manager_delete_finish (AmlInstanceManager *self,
                                             GAsyncResult *result,
                                             GError **error);

/* Opens the instance directory with the default file manager. */
AmlTaskScope *aml_instance_manager_open_folder_async (AmlInstanceManager *self,
                                                       AmlInstance *instance,
                                                       GAsyncReadyCallback callback,
                                                       gpointer user_data);

gboolean aml_instance_manager_open_folder_finish (AmlInstanceManager *self,
                                                  GAsyncResult *result,
                                                  GError **error);

G_END_DECLS

#endif /* AML_INSTANCE_MANAGER_H */
