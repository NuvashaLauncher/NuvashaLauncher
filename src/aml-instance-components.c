/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-instance-components.h"

#include <string.h>

const char *
aml_instance_component_id_for_loader (AmlLoader loader)
{
  switch (loader)
    {
    case AML_LOADER_FABRIC: return AML_COMPONENT_FABRIC;
    case AML_LOADER_FORGE: return AML_COMPONENT_FORGE;
    case AML_LOADER_NEOFORGE: return AML_COMPONENT_NEOFORGE;
    case AML_LOADER_QUILT: return AML_COMPONENT_QUILT;
    case AML_LOADER_VANILLA: default: return NULL;
    }
}

char *
aml_instance_component_get_version (AmlInstance *instance,
                                    const char *component_id)
{
  AmlLoader loader = AML_LOADER_VANILLA;
  GHashTable *components = NULL;
  const char *version = NULL;
  const char *loader_id = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE (instance), NULL);
  g_return_val_if_fail (component_id != NULL, NULL);

  components = aml_instance_get_components (instance);
  version = g_hash_table_lookup (components, component_id);
  if (version != NULL)
    return g_strdup (version);

  if (g_str_equal (component_id, AML_COMPONENT_MINECRAFT))
    {
      return NULL;
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = aml_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return NULL;
    }

  return NULL;
}

gboolean
aml_instance_component_set_version (AmlInstance *instance,
                                    const char *component_id,
                                    const char *version,
                                    GError **error)
{
  AmlLoader loader = AML_LOADER_VANILLA;
  const char *loader_id = NULL;

  g_return_val_if_fail (AML_IS_INSTANCE (instance), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (version == NULL || *version == '\0')
    {
      g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
                   "component \"%s\" has no version", component_id);
      return FALSE;
    }

  if (g_str_equal (component_id, AML_COMPONENT_MINECRAFT))
    {
      return aml_instance_set_string (instance, "minecraft-version", version,
                                      error);
    }

  g_object_get (instance, "loader", &loader, NULL);
  loader_id = aml_instance_component_id_for_loader (loader);
  if (loader_id != NULL && g_str_equal (loader_id, component_id))
    {
      return aml_instance_set_string (instance, "loader-version", version,
                                      error);
    }

  g_set_error (error, AML_INSTANCE_ERROR, AML_INSTANCE_ERROR_INVALID,
               "component \"%s\" is not part of this instance", component_id);
  return FALSE;
}
