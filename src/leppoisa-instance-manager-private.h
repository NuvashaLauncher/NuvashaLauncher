/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef leppoisa_INSTANCE_MANAGER_PRIVATE_H
#define leppoisa_INSTANCE_MANAGER_PRIVATE_H

#include "leppoisa-instance-manager.h"

struct _LeppoisaInstanceManager
{
  GObject parent_instance;

  GListStore *instances;
  char *data_dir;
  char *metadata_path;
  char *instances_dir;
  guint directory_check_source;
};

char *instance_directory (LeppoisaInstance *instance);
char *instance_name (LeppoisaInstance *instance);
char *unique_instance_name (LeppoisaInstanceManager *self, const char *name);
char *default_instance_directory (LeppoisaInstanceManager *self, const char *name);
gboolean check_instance_directory (LeppoisaInstanceManager *self,
                                   const char *directory,
                                   LeppoisaInstance *moved,
                                   char **canonical, GError **error);
gboolean remove_instance_and_save (LeppoisaInstanceManager *self,
                                   LeppoisaInstance *instance, GError **error);
LeppoisaTaskScope *copy_tree_async (GFile *source, GFile *destination,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean copy_tree_finish (GAsyncResult *result, GError **error);

#endif
