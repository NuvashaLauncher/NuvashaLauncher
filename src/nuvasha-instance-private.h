#ifndef NUVASHA_INSTANCE_PRIVATE_H
#define NUVASHA_INSTANCE_PRIVATE_H
#include "nuvasha-instance.h"
struct _NuvashaInstance
{
  GObject parent_instance;
  char *id;
  char *name;
  char *directory;
  char *minecraft_version;
  NuvashaLoader loader;
  char *loader_version;
  char *icon;
  char *java_path;
  char *jvm_args;
  char *modpack_name;
  char *modpack_summary;
  char *modpack_version_id;
  GHashTable *components;
};
#endif
