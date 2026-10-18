/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-instance-components.h"

#include <string.h>

const char *
nuvasha_instance_component_id_for_loader (NuvashaLoader loader)
{
  switch (loader)
    {
    case nuvasha_LOADER_FABRIC: return nuvasha_COMPONENT_FABRIC;
    case nuvasha_LOADER_FORGE: return nuvasha_COMPONENT_FORGE;
    case nuvasha_LOADER_NEOFORGE: return nuvasha_COMPONENT_NEOFORGE;
    case nuvasha_LOADER_QUILT: return nuvasha_COMPONENT_QUILT;
    case nuvasha_LOADER_VANILLA: default: return NULL;
    }
}

char *
nuvasha_instance_component_get_version (NuvashaInstance *instance,
                                    const char *component_id)
{
  NuvashaLoader loader = nuvasha_LOADER_VANILLA;
  GHashTable *components = NULL;
  const char *version = NULL;
  const char *loader_id = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (component_id != NULL, NULL);

  components = nuvasha_instance_get_components (instance);
  version = g_hash_table_lookup (components, component_id);
  if (version != NULL)
    return g_strdup (version);

  if (g_str_equal (component_id, nuvasha_COMPONENT_MINECRAFT))
    {
      return NULL;
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = nuvasha_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return NULL;
    }

  return NULL;
}

gboolean
nuvasha_instance_component_set_version (NuvashaInstance *instance,
                                    const char *component_id,
                                    const char *version,
                                    GError **error)
{
  NuvashaLoader loader = nuvasha_LOADER_VANILLA;
  const char *loader_id = NULL;

  g_return_val_if_fail (nuvasha_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (version == NULL || *version == '\0')
    {
      g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
                   "component \"%s\" has no version", component_id);
      return FALSE;
    }

  if (g_str_equal (component_id, nuvasha_COMPONENT_MINECRAFT))
    {
      return nuvasha_instance_set_string (instance, "minecraft-version", version,
                                      error);
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = nuvasha_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return nuvasha_instance_set_string (instance, "loader-version", version,
                                      error);
    }

  g_set_error (error, nuvasha_INSTANCE_ERROR, nuvasha_INSTANCE_ERROR_INVALID,
               "component \"%s\" is not part of this instance", component_id);
  return FALSE;
}
