/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef comfy_INSTANCE_MANAGER_PRIVATE_H
#define comfy_INSTANCE_MANAGER_PRIVATE_H

#include "comfy-instance-manager.h"

struct _ComfyInstanceManager
{
  GObject parent_instance;

  GListStore *instances;
  char *data_dir;
  char *metadata_path;
  char *instances_dir;
  guint directory_check_source;
};

char *instance_directory (ComfyInstance *instance);
char *instance_name (ComfyInstance *instance);
char *unique_instance_name (ComfyInstanceManager *self, const char *name);
char *default_instance_directory (ComfyInstanceManager *self, const char *name);
gboolean check_instance_directory (ComfyInstanceManager *self,
                                   const char *directory,
                                   ComfyInstance *moved,
                                   char **canonical, GError **error);
gboolean remove_instance_and_save (ComfyInstanceManager *self,
                                   ComfyInstance *instance, GError **error);
ComfyTaskScope *copy_tree_async (GFile *source, GFile *destination,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean copy_tree_finish (GAsyncResult *result, GError **error);

#endif
