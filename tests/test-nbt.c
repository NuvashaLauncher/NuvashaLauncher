/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <glib.h>
#include "nuvashalauncher-nbt.h"

static void
test_nbt_roundtrip_uncompressed (void)
{
  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "");
  g_autoptr (NuvashaLauncherNbtTag) list =
    nuvashalauncher_nbt_new_list ("servers", NUVASHALAUNCHER_NBT_COMPOUND);
  NuvashaLauncherNbtTag *entry =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, NULL);

  nuvashalauncher_nbt_compound_set (entry, nuvashalauncher_nbt_new_string ("name", "My Server"));
  nuvashalauncher_nbt_compound_set (entry, nuvashalauncher_nbt_new_string ("ip", "play.example.com"));
  nuvashalauncher_nbt_compound_set (entry, nuvashalauncher_nbt_new_byte ("acceptTextures", 1));
  g_ptr_array_add (list->v_list.items, entry);
  nuvashalauncher_nbt_compound_set (root, g_steal_pointer (&list));

  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = nuvashalauncher_nbt_serialize (root, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);

  gsize len = 0;
  const guint8 *data = g_bytes_get_data (bytes, &len);
  g_autoptr (NuvashaLauncherNbtTag) parsed =
    nuvashalauncher_nbt_parse (data, len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (parsed);

  NuvashaLauncherNbtTag *servers = nuvashalauncher_nbt_compound_get (parsed, "servers");
  g_assert_nonnull (servers);
  g_assert_cmpint (servers->type, ==, NUVASHALAUNCHER_NBT_LIST);
  g_assert_cmpuint (servers->v_list.items->len, ==, 1);

  NuvashaLauncherNbtTag *first = g_ptr_array_index (servers->v_list.items, 0);
  g_assert_cmpstr (nuvashalauncher_nbt_compound_get_string (first, "name"), ==, "My Server");
  g_assert_cmpstr (nuvashalauncher_nbt_compound_get_string (first, "ip"), ==, "play.example.com");
  g_assert_cmpint (nuvashalauncher_nbt_compound_get_byte (first, "acceptTextures", 0), ==, 1);
}

static void
test_nbt_roundtrip_gzip (void)
{
  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "");
  NuvashaLauncherNbtTag *data_tag =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, "Data");

  nuvashalauncher_nbt_compound_set (data_tag, nuvashalauncher_nbt_new_string ("LevelName", "Survival World"));
  nuvashalauncher_nbt_compound_set (data_tag, nuvashalauncher_nbt_new_int ("GameType", 0));
  nuvashalauncher_nbt_compound_set (data_tag, nuvashalauncher_nbt_new_long ("RandomSeed", 1234567890123LL));
  nuvashalauncher_nbt_compound_set (root, data_tag);

  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = nuvashalauncher_nbt_serialize (root, TRUE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);

  gsize len = 0;
  const guint8 *raw = g_bytes_get_data (bytes, &len);
  g_assert_cmpuint (len, >=, 2);
  g_assert_cmpuint (raw[0], ==, 0x1f);
  g_assert_cmpuint (raw[1], ==, 0x8b);

  g_autoptr (NuvashaLauncherNbtTag) parsed =
    nuvashalauncher_nbt_parse (raw, len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (parsed);

  NuvashaLauncherNbtTag *d = nuvashalauncher_nbt_compound_get (parsed, "Data");
  g_assert_nonnull (d);
  g_assert_cmpstr (nuvashalauncher_nbt_compound_get_string (d, "LevelName"), ==, "Survival World");
  g_assert_cmpint (nuvashalauncher_nbt_compound_get_int (d, "GameType", -1), ==, 0);
  g_assert_cmpint (nuvashalauncher_nbt_compound_get_long (d, "RandomSeed", 0), ==, 1234567890123LL);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/nbt/roundtrip-uncompressed", test_nbt_roundtrip_uncompressed);
  g_test_add_func ("/nbt/roundtrip-gzip", test_nbt_roundtrip_gzip);
  return g_test_run ();
}
