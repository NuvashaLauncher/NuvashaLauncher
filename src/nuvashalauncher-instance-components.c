/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-instance-components.h"

#include <string.h>

const char *
nuvashalauncher_instance_component_id_for_loader (nuvashalauncherLoader loader)
{
  switch (loader)
    {
    case nuvashalauncher_LOADER_FABRIC: return nuvashalauncher_COMPONENT_FABRIC;
    case nuvashalauncher_LOADER_FORGE: return nuvashalauncher_COMPONENT_FORGE;
    case nuvashalauncher_LOADER_NEOFORGE: return nuvashalauncher_COMPONENT_NEOFORGE;
    case nuvashalauncher_LOADER_QUILT: return nuvashalauncher_COMPONENT_QUILT;
    case nuvashalauncher_LOADER_VANILLA: default: return NULL;
    }
}

char *
nuvashalauncher_instance_component_get_version (nuvashalauncherInstance *instance,
                                    const char *component_id)
{
  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;
  GHashTable *components = NULL;
  const char *version = NULL;
  const char *loader_id = NULL;

  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (component_id != NULL, NULL);

  components = nuvashalauncher_instance_get_components (instance);
  version = g_hash_table_lookup (components, component_id);
  if (version != NULL)
    return g_strdup (version);

  if (g_str_equal (component_id, nuvashalauncher_COMPONENT_MINECRAFT))
    {
      return NULL;
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = nuvashalauncher_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return NULL;
    }

  return NULL;
}

gboolean
nuvashalauncher_instance_component_set_version (nuvashalauncherInstance *instance,
                                    const char *component_id,
                                    const char *version,
                                    GError **error)
{
  nuvashalauncherLoader loader = nuvashalauncher_LOADER_VANILLA;
  const char *loader_id = NULL;

  g_return_val_if_fail (nuvashalauncher_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (version == NULL || *version == '\0')
    {
      g_set_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID,
                   "component \"%s\" has no version", component_id);
      return FALSE;
    }

  if (g_str_equal (component_id, nuvashalauncher_COMPONENT_MINECRAFT))
    {
      return nuvashalauncher_instance_set_string (instance, "minecraft-version", version,
                                      error);
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = nuvashalauncher_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return nuvashalauncher_instance_set_string (instance, "loader-version", version,
                                      error);
    }

  g_set_error (error, nuvashalauncher_INSTANCE_ERROR, nuvashalauncher_INSTANCE_ERROR_INVALID,
               "component \"%s\" is not part of this instance", component_id);
  return FALSE;
}
