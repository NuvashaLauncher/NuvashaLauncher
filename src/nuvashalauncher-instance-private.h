/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_INSTANCE_PRIVATE_H
#define NUVASHALAUNCHER_INSTANCE_PRIVATE_H
#include "nuvashalauncher-instance.h"

#define NUVASHALAUNCHER_INSTANCE_DEFAULT_MEMORY_MIB 2048U

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
  guint game_width;
  guint game_height;
  gint game_fullscreen; /* -1 = global default, 0 = windowed, 1 = fullscreen */
  gint use_dedicated_gpu; /* -1 = global default, 0 = off, 1 = on */
  gint use_gamemode;      /* -1 = global default, 0 = off, 1 = on */
  gint use_mangohud;      /* -1 = global default, 0 = off, 1 = on */
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
