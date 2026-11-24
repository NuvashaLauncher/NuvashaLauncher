/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_INSTANCE_PRIVATE_H
#define NUVASHALAUNCHER_INSTANCE_PRIVATE_H
#include "nuvashalauncher-instance.h"
struct _NuvashaLauncherInstance
{
  GObject parent_instance;
  char *id;
  char *name;
  char *directory;
  char *minecraft_version;
  NuvashaLauncherLoader loader;
  char *loader_version;
  char *icon;
  char *java_path;
  char *jvm_args;
  guint memory_mib;
  guint64 play_seconds;
  gint64 last_played_at;
  gint64 created_at;
  guint launch_count;
  guint crash_count;
  char *modpack_name;
  char *modpack_summary;
  char *modpack_version_id;
  GHashTable *components;
};
#endif
