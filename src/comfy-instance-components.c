/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-instance-components.h"

#include <string.h>

const char *
comfy_instance_component_id_for_loader (ComfyLoader loader)
{
  switch (loader)
    {
    case comfy_LOADER_FABRIC: return comfy_COMPONENT_FABRIC;
    case comfy_LOADER_FORGE: return comfy_COMPONENT_FORGE;
    case comfy_LOADER_NEOFORGE: return comfy_COMPONENT_NEOFORGE;
    case comfy_LOADER_QUILT: return comfy_COMPONENT_QUILT;
    case comfy_LOADER_VANILLA: default: return NULL;
    }
}

char *
comfy_instance_component_get_version (ComfyInstance *instance,
                                    const char *component_id)
{
  ComfyLoader loader = comfy_LOADER_VANILLA;
  GHashTable *components = NULL;
  const char *version = NULL;
  const char *loader_id = NULL;

  g_return_val_if_fail (comfy_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (component_id != NULL, NULL);

  components = comfy_instance_get_components (instance);
  version = g_hash_table_lookup (components, component_id);
  if (version != NULL)
    return g_strdup (version);

  if (g_str_equal (component_id, comfy_COMPONENT_MINECRAFT))
    {
      return NULL;
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = comfy_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return NULL;
    }

  return NULL;
}

gboolean
comfy_instance_component_set_version (ComfyInstance *instance,
                                    const char *component_id,
                                    const char *version,
                                    GError **error)
{
  ComfyLoader loader = comfy_LOADER_VANILLA;
  const char *loader_id = NULL;

  g_return_val_if_fail (comfy_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (version == NULL || *version == '\0')
    {
      g_set_error (error, comfy_INSTANCE_ERROR, comfy_INSTANCE_ERROR_INVALID,
                   "component \"%s\" has no version", component_id);
      return FALSE;
    }

  if (g_str_equal (component_id, comfy_COMPONENT_MINECRAFT))
    {
      return comfy_instance_set_string (instance, "minecraft-version", version,
                                      error);
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = comfy_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return comfy_instance_set_string (instance, "loader-version", version,
                                      error);
    }

  g_set_error (error, comfy_INSTANCE_ERROR, comfy_INSTANCE_ERROR_INVALID,
               "component \"%s\" is not part of this instance", component_id);
  return FALSE;
}
