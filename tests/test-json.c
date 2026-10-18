/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-json.h"

#include <json-glib/json-glib.h>

int
main (void)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root = NULL;

  g_assert_true (json_parser_load_from_data (
    parser, "{\"integer\":7,\"fraction\":2.5,\"huge\":1e300}", -1,
    NULL));
  root = json_parser_get_root (parser);
  g_assert_cmpint (comfy_json_int (root, "integer", -1), ==, 7);
  g_assert_cmpint (comfy_json_int (root, "fraction", -1), ==, 2);
  g_assert_cmpint (comfy_json_int (root, "huge", 99), ==, 99);
  g_print ("ok - JSON integer conversion is range checked\n");
  return 0;
}
