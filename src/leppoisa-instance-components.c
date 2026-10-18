/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "leppoisa-instance-components.h"

#include <string.h>

const char *
leppoisa_instance_component_id_for_loader (LeppoisaLoader loader)
{
  switch (loader)
    {
    case leppoisa_LOADER_FABRIC: return leppoisa_COMPONENT_FABRIC;
    case leppoisa_LOADER_FORGE: return leppoisa_COMPONENT_FORGE;
    case leppoisa_LOADER_NEOFORGE: return leppoisa_COMPONENT_NEOFORGE;
    case leppoisa_LOADER_QUILT: return leppoisa_COMPONENT_QUILT;
    case leppoisa_LOADER_VANILLA: default: return NULL;
    }
}

char *
leppoisa_instance_component_get_version (LeppoisaInstance *instance,
                                    const char *component_id)
{
  LeppoisaLoader loader = leppoisa_LOADER_VANILLA;
  GHashTable *components = NULL;
  const char *version = NULL;
  const char *loader_id = NULL;

  g_return_val_if_fail (leppoisa_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (component_id != NULL, NULL);

  components = leppoisa_instance_get_components (instance);
  version = g_hash_table_lookup (components, component_id);
  if (version != NULL)
    return g_strdup (version);

  if (g_str_equal (component_id, leppoisa_COMPONENT_MINECRAFT))
    {
      return NULL;
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = leppoisa_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return NULL;
    }

  return NULL;
}

gboolean
leppoisa_instance_component_set_version (LeppoisaInstance *instance,
                                    const char *component_id,
                                    const char *version,
                                    GError **error)
{
  LeppoisaLoader loader = leppoisa_LOADER_VANILLA;
  const char *loader_id = NULL;

  g_return_val_if_fail (leppoisa_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (version == NULL || *version == '\0')
    {
      g_set_error (error, leppoisa_INSTANCE_ERROR, leppoisa_INSTANCE_ERROR_INVALID,
                   "component \"%s\" has no version", component_id);
      return FALSE;
    }

  if (g_str_equal (component_id, leppoisa_COMPONENT_MINECRAFT))
    {
      return leppoisa_instance_set_string (instance, "minecraft-version", version,
                                      error);
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = leppoisa_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return leppoisa_instance_set_string (instance, "loader-version", version,
                                      error);
    }

  g_set_error (error, leppoisa_INSTANCE_ERROR, leppoisa_INSTANCE_ERROR_INVALID,
               "component \"%s\" is not part of this instance", component_id);
  return FALSE;
}
