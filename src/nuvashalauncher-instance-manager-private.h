/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef nuvashalauncher_INSTANCE_MANAGER_PRIVATE_H
#define nuvashalauncher_INSTANCE_MANAGER_PRIVATE_H

#include "nuvashalauncher-instance-manager.h"

struct _NuvashaLauncherInstanceManager
{
  GObject parent_instance;

  GListStore *instances;
  char *data_dir;
  char *metadata_path;
  char *instances_dir;
  guint directory_check_source;
};

char *instance_directory (NuvashaLauncherInstance *instance);
char *instance_name (NuvashaLauncherInstance *instance);
char *instance_name_key (const char *name);
char *unique_instance_name (NuvashaLauncherInstanceManager *self, const char *name);
char *default_instance_directory (NuvashaLauncherInstanceManager *self, const char *name);
gboolean check_instance_directory (NuvashaLauncherInstanceManager *self,
                                   const char *directory,
                                   NuvashaLauncherInstance *moved,
                                   char **canonical, GError **error);
gboolean remove_instance_and_save (NuvashaLauncherInstanceManager *self,
                                   NuvashaLauncherInstance *instance, GError **error);
NuvashaLauncherTaskScope *copy_tree_async (GFile *source, GFile *destination,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean copy_tree_finish (GAsyncResult *result, GError **error);
gboolean check_instance_directories (gpointer user_data);

#endif
