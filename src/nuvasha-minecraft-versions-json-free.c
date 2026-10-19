/* Minecraft version JSON object cleanup. */

#include "nuvasha-minecraft-versions.h"

void
nuvasha_mc_free_download_info (gpointer data)
{
  NuvashaDownloadInfo *info = data;

  if (info == NULL)
    return;

  g_free (info->id);
  g_free (info->url);
  g_free (info->sha1);
  g_free (info->path);
  g_free (info);
}

void
nuvasha_mc_free_rule (gpointer data)
{
  NuvashaRule *rule = data;

  if (rule == NULL)
    return;

  g_free (rule->os_name);
  g_free (rule->os_arch);
  g_free (rule->os_version);
  g_clear_pointer (&rule->features, g_hash_table_unref);
  g_free (rule);
}

void
nuvasha_mc_free_library (gpointer data)
{
  NuvashaLibrary *library = data;

  if (library == NULL)
    return;

  g_free (library->name);
  g_clear_pointer (&library->artifact, nuvasha_mc_free_download_info);
  g_clear_pointer (&library->classifiers, g_hash_table_unref);
  g_clear_pointer (&library->natives, g_hash_table_unref);
  g_clear_pointer (&library->rules, g_ptr_array_unref);
  g_clear_pointer (&library->exclude, g_ptr_array_unref);
  g_free (library);
}

void
nuvasha_mc_free_argument (gpointer data)
{
  NuvashaArgument *argument = data;

  if (argument == NULL)
    return;

  g_clear_pointer (&argument->rules, g_ptr_array_unref);
  g_clear_pointer (&argument->values, g_ptr_array_unref);
  g_free (argument);
}

void
nuvasha_mc_free_logging (gpointer data)
{
  NuvashaLogging *logging = data;

  if (logging == NULL)
    return;

  g_free (logging->argument);
  g_free (logging->type);
  g_clear_pointer (&logging->file, nuvasha_mc_free_download_info);
  g_free (logging);
}

void
nuvasha_mc_free_java_version (gpointer data)
{
  NuvashaJavaVersion *version = data;

  if (version == NULL)
    return;

  g_free (version->component);
  g_free (version);
}

void
nuvasha_mc_free_entry (gpointer data)
{
  NuvashaVersionEntry *entry = data;

  if (entry == NULL)
    return;

  g_free (entry->id);
  g_free (entry->url);
  g_free (entry->sha1);
  g_free (entry->release_time);
  g_free (entry);
}
