/* Modrinth category parsing and labels. */

#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-json.h"
#include <glib/gi18n.h>

#include <string.h>

/* ---------------------------------------------------------- *
 * The categories a search can be narrowed by
 * ---------------------------------------------------------- */

void
nuvashalauncher_modrinth_category_free (nuvashalauncherModrinthCategory *category)
{
  if (category == NULL)
    return;

  g_free (category->name);
  g_free (category->header);
  g_free (category);
}

/* Translate known facets without changing their API identifiers. Unknown
 * future categories still get a readable fallback rather than disappearing. */
char *
nuvashalauncher_modrinth_category_label (const char *name)
{
  g_autofree char *spaced = NULL;
  static const struct { const char *slug; const char *label; } labels[] = {
    { "adventure", N_("Adventure") },
    { "atmosphere", N_("Atmosphere") },
    { "audio", N_("Audio") },
    { "blocks", N_("Blocks") },
    { "bloom", N_("Bloom") },
    { "cartoon", N_("Cartoon") },
    { "categories", N_("Categories") },
    { "colored-lighting", N_("Colored Lighting") },
    { "combat", N_("Combat") },
    { "core-shaders", N_("Core Shaders") },
    { "cursed", N_("Cursed") },
    { "decoration", N_("Decoration") },
    { "economy", N_("Economy") },
    { "entities", N_("Entities") },
    { "environment", N_("Environment") },
    { "equipment", N_("Equipment") },
    { "fantasy", N_("Fantasy") },
    { "features", N_("Features") },
    { "foliage", N_("Foliage") },
    { "fonts", N_("Fonts") },
    { "food", N_("Food") },
    { "game-mechanics", N_("Game Mechanics") },
    { "gui", N_("Interface") },
    { "high", N_("High") },
    { "items", N_("Items") },
    { "library", N_("Library") },
    { "locale", N_("Language") },
    { "low", N_("Low") },
    { "magic", N_("Magic") },
    { "management", N_("Management") },
    { "medium", N_("Medium") },
    { "minigame", N_("Minigame") },
    { "mobs", N_("Mobs") },
    { "modded", N_("Modded") },
    { "models", N_("Models") },
    { "optimization", N_("Optimization") },
    { "path-tracing", N_("Path Tracing") },
    { "performance impact", N_("Performance Impact") },
    { "potato", N_("Potato") },
    { "realistic", N_("Realistic") },
    { "reflections", N_("Reflections") },
    { "resolutions", N_("Resolutions") },
    { "screenshot", N_("Screenshot") },
    { "semi-realistic", N_("Semi-realistic") },
    { "shadows", N_("Shadows") },
    { "simplistic", N_("Simplistic") },
    { "social", N_("Social") },
    { "storage", N_("Storage") },
    { "technology", N_("Technology") },
    { "themed", N_("Themed") },
    { "transportation", N_("Transportation") },
    { "tweaks", N_("Tweaks") },
    { "utility", N_("Utility") },
    { "vanilla-like", N_("Vanilla-like") },
    { "worldgen", N_("World Generation") },
  };

  if (name == NULL || *name == '\0')
    return g_strdup ("");

  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    if (g_str_equal (name, labels[i].slug))
      return g_strdup (_(labels[i].label));

  /* Resolution identifiers and technical acronyms are language-neutral. */
  if (g_ascii_isdigit (*name))
    return g_strdup (name);
  if (g_str_equal (name, "pbr"))
    return g_strdup ("PBR");

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
nuvashalauncher_modrinth_project_type_for_content (nuvashalauncherContentType type)
{
  return g_strdup (nuvashalauncher_content_type_to_string (type));
}

static int
compare_by_name (gconstpointer a, gconstpointer b)
{
  const nuvashalauncherModrinthCategory *left = *(nuvashalauncherModrinthCategory *const *) a;
  const nuvashalauncherModrinthCategory *right = *(nuvashalauncherModrinthCategory *const *) b;

  return g_strcmp0 (left->name, right->name);
}

GPtrArray *
nuvashalauncher_modrinth_categories_from_json (const char *json, gssize length,
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
      g_set_error_literal (error, nuvashalauncher_MODRINTH_ERROR,
                           nuvashalauncher_MODRINTH_ERROR_PARSE,
                           "the categories answer is not a list");
      return NULL;
    }

  categories =
    g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_modrinth_category_free);
  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      g_autofree char *item_type = NULL;
      g_autofree char *name = NULL;
      g_autofree char *header = NULL;
      nuvashalauncherModrinthCategory *category = NULL;
      JsonNode *item = json_array_get_element (array, i);

      if (nuvashalauncher_json_object_of (item) == NULL)
        continue;

      /* Use categories only for project types that publish them. */
      item_type = nuvashalauncher_json_string (item, "project_type");
      name = nuvashalauncher_json_string (item, "name");

      if (name == NULL || g_strcmp0 (item_type, project_type) != 0)
        continue;

      header = nuvashalauncher_json_string (item, "header");

      category = g_new0 (nuvashalauncherModrinthCategory, 1);
      category->name = g_steal_pointer (&name);
      category->header = g_steal_pointer (&header);
      g_ptr_array_add (categories, category);
    }

  g_ptr_array_sort (categories, compare_by_name);

  return categories;
}
