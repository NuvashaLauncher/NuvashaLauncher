/* Modrinth category parsing and labels. */

#include "nuvasha-modrinth.h"
#include "nuvasha-json.h"

#include <string.h>

/* ---------------------------------------------------------- *
 * The categories a search can be narrowed by
 * ---------------------------------------------------------- */

void
nuvasha_modrinth_category_free (NuvashaModrinthCategory *category)
{
  if (category == NULL)
    return;

  g_free (category->name);
  g_free (category->header);
  g_free (category);
}

/* Format a facet slug as a human-readable label without changing the slug. */
char *
nuvasha_modrinth_category_label (const char *name)
{
  g_autofree char *spaced = NULL;

  if (name == NULL || *name == '\0')
    return g_strdup ("");

  spaced = g_strdelimit (g_strdup (name), "-_", ' ');

  /* Title-case each word: the first letter after a space goes upper
     case, the rest keep their shape, so "512x+" stays "512x+". */
  {
    gboolean word_start = TRUE;

    for (char *p = spaced; *p != '\0'; p++)
      {
        if (*p == ' ')
          {
            word_start = TRUE;
            continue;
          }

        if (word_start && g_ascii_isalpha (*p))
          *p = g_ascii_toupper (*p);

        word_start = FALSE;
      }
  }

  return g_steal_pointer (&spaced);
}

char *
nuvasha_modrinth_project_type_for_content (NuvashaContentType type)
{
  return g_strdup (nuvasha_content_type_to_string (type));
}

static int
compare_by_name (gconstpointer a, gconstpointer b)
{
  const NuvashaModrinthCategory *left = *(NuvashaModrinthCategory *const *) a;
  const NuvashaModrinthCategory *right = *(NuvashaModrinthCategory *const *) b;

  return g_strcmp0 (left->name, right->name);
}

GPtrArray *
nuvasha_modrinth_categories_from_json (const char *json, gssize length,
                                   const char *project_type, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) parse_error = NULL;
  GPtrArray *categories = NULL;
  JsonNode *root = NULL;
  JsonArray *array = NULL;

  if (!json_parser_load_from_data (parser, json != NULL ? json : "",
                                   json != NULL ? length : 0, &parse_error))
    {
      g_propagate_error (error, g_steal_pointer (&parse_error));
      return NULL;
    }

  root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error, nuvasha_MODRINTH_ERROR,
                           nuvasha_MODRINTH_ERROR_PARSE,
                           "the categories answer is not a list");
      return NULL;
    }

  categories =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvasha_modrinth_category_free);
  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      g_autofree char *item_type = NULL;
      g_autofree char *name = NULL;
      g_autofree char *header = NULL;
      NuvashaModrinthCategory *category = NULL;
      JsonNode *item = json_array_get_element (array, i);

      if (nuvasha_json_object_of (item) == NULL)
        continue;

      /* Use categories only for project types that publish them. */
      item_type = nuvasha_json_string (item, "project_type");
      name = nuvasha_json_string (item, "name");

      if (name == NULL || g_strcmp0 (item_type, project_type) != 0)
        continue;

      header = nuvasha_json_string (item, "header");

      category = g_new0 (NuvashaModrinthCategory, 1);
      category->name = g_steal_pointer (&name);
      category->header = g_steal_pointer (&header);
      g_ptr_array_add (categories, category);
    }

  g_ptr_array_sort (categories, compare_by_name);

  return categories;
}
