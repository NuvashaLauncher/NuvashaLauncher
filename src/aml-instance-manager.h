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

/* Copies the files of @source into a fresh directory, registers the
 * copy as "Copy of <name>" with a new id and saves. The copy appears in
 * the list only after its files were copied. */
void aml_instance_manager_duplicate_async (AmlInstanceManager *self,
                                           AmlInstance *source,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

AmlInstance *aml_instance_manager_duplicate_finish (AmlInstanceManager *self,
                                                    GAsyncResult *result,
                                                    GError **error);

/* Removes the instance. The directory is only removed when
 * @delete_directory is TRUE; the metadata is removed after the
 * directory went away, so a failed deletion keeps the instance
 * registered. */
void aml_instance_manager_delete_async (AmlInstanceManager *self,
                                        AmlInstance *instance,
                                        gboolean delete_directory,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

gboolean aml_instance_manager_delete_finish (AmlInstanceManager *self,
                                             GAsyncResult *result,
                                             GError **error);

/* Opens the instance directory with the default file manager. */
void aml_instance_manager_open_folder_async (AmlInstanceManager *self,
                                             AmlInstance *instance,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

gboolean aml_instance_manager_open_folder_finish (AmlInstanceManager *self,
                                                  GAsyncResult *result,
                                                  GError **error);

G_END_DECLS

#endif /* AML_INSTANCE_MANAGER_H */
