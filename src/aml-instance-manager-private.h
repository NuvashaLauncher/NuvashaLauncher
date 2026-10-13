/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AML_INSTANCE_MANAGER_PRIVATE_H
#define AML_INSTANCE_MANAGER_PRIVATE_H

#include "aml-instance-manager.h"

struct _AmlInstanceManager
{
  GObject parent_instance;

  GListStore *instances;
  char *data_dir;
  char *metadata_path;
  char *instances_dir;
  guint directory_check_source;
};

char *instance_directory (AmlInstance *instance);
char *instance_name (AmlInstance *instance);
char *unique_instance_name (AmlInstanceManager *self, const char *name);
char *default_instance_directory (AmlInstanceManager *self, const char *name);
gboolean check_instance_directory (AmlInstanceManager *self,
                                   const char *directory,
                                   AmlInstance *moved,
                                   char **canonical, GError **error);
gboolean remove_instance_and_save (AmlInstanceManager *self,
                                   AmlInstance *instance, GError **error);
AmlTaskScope *copy_tree_async (GFile *source, GFile *destination,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean copy_tree_finish (GAsyncResult *result, GError **error);

#endif
