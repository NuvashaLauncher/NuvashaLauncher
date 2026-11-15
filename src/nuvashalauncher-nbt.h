/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_NBT_H
#define NUVASHALAUNCHER_NBT_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NUVASHALAUNCHER_NBT_END = 0,
  NUVASHALAUNCHER_NBT_BYTE = 1,
  NUVASHALAUNCHER_NBT_SHORT = 2,
  NUVASHALAUNCHER_NBT_INT = 3,
  NUVASHALAUNCHER_NBT_LONG = 4,
  NUVASHALAUNCHER_NBT_FLOAT = 5,
  NUVASHALAUNCHER_NBT_DOUBLE = 6,
  NUVASHALAUNCHER_NBT_BYTE_ARRAY = 7,
  NUVASHALAUNCHER_NBT_STRING = 8,
  NUVASHALAUNCHER_NBT_LIST = 9,
  NUVASHALAUNCHER_NBT_COMPOUND = 10,
  NUVASHALAUNCHER_NBT_INT_ARRAY = 11,
  NUVASHALAUNCHER_NBT_LONG_ARRAY = 12,
} NuvashaLauncherNbtType;

typedef struct _NuvashaLauncherNbtTag NuvashaLauncherNbtTag;

struct _NuvashaLauncherNbtTag {
  NuvashaLauncherNbtType type;
  char *name;
  union {
    gint8 v_byte;
    gint16 v_short;
    gint32 v_int;
    gint64 v_long;
    float v_float;
    double v_double;
    char *v_string;
    struct {
      guint8 *data;
      gsize len;
    } v_byte_array;
    struct {
      NuvashaLauncherNbtType elem_type;
      GPtrArray *items; /* NuvashaLauncherNbtTag* */
    } v_list;
    GPtrArray *v_compound; /* NuvashaLauncherNbtTag* */
    struct {
      gint32 *data;
      gsize len;
    } v_int_array;
    struct {
      gint64 *data;
      gsize len;
    } v_long_array;
  };
};

NuvashaLauncherNbtTag *nuvashalauncher_nbt_tag_new (NuvashaLauncherNbtType type,
                                                  const char *name);
void nuvashalauncher_nbt_tag_free (NuvashaLauncherNbtTag *tag);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherNbtTag, nuvashalauncher_nbt_tag_free)

NuvashaLauncherNbtTag *nuvashalauncher_nbt_new_byte (const char *name, gint8 value);
NuvashaLauncherNbtTag *nuvashalauncher_nbt_new_int (const char *name, gint32 value);
NuvashaLauncherNbtTag *nuvashalauncher_nbt_new_long (const char *name, gint64 value);
NuvashaLauncherNbtTag *nuvashalauncher_nbt_new_string (const char *name, const char *value);
NuvashaLauncherNbtTag *nuvashalauncher_nbt_new_list (const char *name,
                                                   NuvashaLauncherNbtType elem_type);

NuvashaLauncherNbtTag *nuvashalauncher_nbt_compound_get (
    const NuvashaLauncherNbtTag *compound, const char *name);
void nuvashalauncher_nbt_compound_set (NuvashaLauncherNbtTag *compound,
                                       NuvashaLauncherNbtTag *child);
const char *nuvashalauncher_nbt_compound_get_string (
    const NuvashaLauncherNbtTag *compound, const char *name);
gint8 nuvashalauncher_nbt_compound_get_byte (
    const NuvashaLauncherNbtTag *compound, const char *name, gint8 fallback);
gint32 nuvashalauncher_nbt_compound_get_int (
    const NuvashaLauncherNbtTag *compound, const char *name, gint32 fallback);
gint64 nuvashalauncher_nbt_compound_get_long (
    const NuvashaLauncherNbtTag *compound, const char *name, gint64 fallback);

NuvashaLauncherNbtTag *nuvashalauncher_nbt_parse (const guint8 *data,
                                                  gsize len,
                                                  GError **error);
NuvashaLauncherNbtTag *nuvashalauncher_nbt_parse_file (const char *path,
                                                       GError **error);
GBytes *nuvashalauncher_nbt_serialize (const NuvashaLauncherNbtTag *root,
                                       gboolean gzip,
                                       GError **error);
gboolean nuvashalauncher_nbt_write_file (const NuvashaLauncherNbtTag *root,
                                         const char *path,
                                         gboolean gzip,
                                         GError **error);

G_END_DECLS

#endif
