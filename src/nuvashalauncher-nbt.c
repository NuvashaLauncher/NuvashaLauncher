/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-nbt.h"
#include <string.h>

#define NBT_MAX_DEPTH 512

typedef struct {
  const guint8 *data;
  gsize len;
  gsize pos;
} NbtReader;

static gboolean
read_bytes (NbtReader *r, void *out, gsize n, GError **error)
{
  if (n > r->len - r->pos)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Unexpected end of NBT data");
      return FALSE;
    }
  if (n > 0)
    memcpy (out, r->data + r->pos, n);
  r->pos += n;
  return TRUE;
}

static gboolean
read_u8 (NbtReader *r, guint8 *out, GError **error)
{
  return read_bytes (r, out, 1, error);
}

static gboolean
read_u16_be (NbtReader *r, guint16 *out, GError **error)
{
  guint8 buf[2];
  if (!read_bytes (r, buf, 2, error))
    return FALSE;
  *out = ((guint16) buf[0] << 8) | (guint16) buf[1];
  return TRUE;
}

static gboolean
read_u32_be (NbtReader *r, guint32 *out, GError **error)
{
  guint8 buf[4];
  if (!read_bytes (r, buf, 4, error))
    return FALSE;
  *out = ((guint32) buf[0] << 24) | ((guint32) buf[1] << 16) |
         ((guint32) buf[2] << 8) | (guint32) buf[3];
  return TRUE;
}

static gboolean
read_u64_be (NbtReader *r, guint64 *out, GError **error)
{
  guint8 buf[8];
  if (!read_bytes (r, buf, 8, error))
    return FALSE;
  *out = ((guint64) buf[0] << 56) | ((guint64) buf[1] << 48) |
         ((guint64) buf[2] << 40) | ((guint64) buf[3] << 32) |
         ((guint64) buf[4] << 24) | ((guint64) buf[5] << 16) |
         ((guint64) buf[6] << 8) | (guint64) buf[7];
  return TRUE;
}

static char *
read_string (NbtReader *r, GError **error)
{
  guint16 len = 0;
  char *s = NULL;

  if (!read_u16_be (r, &len, error))
    return NULL;
  if (len > r->len - r->pos)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "NBT string exceeds remaining data");
      return NULL;
    }
  s = g_malloc ((gsize) len + 1);
  if (len > 0)
    memcpy (s, r->data + r->pos, len);
  s[len] = '\0';
  r->pos += len;
  return s;
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_tag_new (NuvashaLauncherNbtType type, const char *name)
{
  NuvashaLauncherNbtTag *tag = g_new0 (NuvashaLauncherNbtTag, 1);
  tag->type = type;
  tag->name = g_strdup (name);
  if (type == NUVASHALAUNCHER_NBT_COMPOUND)
    tag->v_compound =
      g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_nbt_tag_free);
  else if (type == NUVASHALAUNCHER_NBT_LIST)
    tag->v_list.items =
      g_ptr_array_new_with_free_func ((GDestroyNotify) nuvashalauncher_nbt_tag_free);
  return tag;
}

void
nuvashalauncher_nbt_tag_free (NuvashaLauncherNbtTag *tag)
{
  if (tag == NULL)
    return;

  g_free (tag->name);
  switch (tag->type)
    {
    case NUVASHALAUNCHER_NBT_STRING:
      g_free (tag->v_string);
      break;
    case NUVASHALAUNCHER_NBT_BYTE_ARRAY:
      g_free (tag->v_byte_array.data);
      break;
    case NUVASHALAUNCHER_NBT_INT_ARRAY:
      g_free (tag->v_int_array.data);
      break;
    case NUVASHALAUNCHER_NBT_LONG_ARRAY:
      g_free (tag->v_long_array.data);
      break;
    case NUVASHALAUNCHER_NBT_LIST:
      g_clear_pointer (&tag->v_list.items, g_ptr_array_unref);
      break;
    case NUVASHALAUNCHER_NBT_COMPOUND:
      g_clear_pointer (&tag->v_compound, g_ptr_array_unref);
      break;
    default:
      break;
    }
  g_free (tag);
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_new_byte (const char *name, gint8 value)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_BYTE, name);
  t->v_byte = value;
  return t;
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_new_int (const char *name, gint32 value)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_INT, name);
  t->v_int = value;
  return t;
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_new_long (const char *name, gint64 value)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_LONG, name);
  t->v_long = value;
  return t;
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_new_string (const char *name, const char *value)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_STRING, name);
  t->v_string = g_strdup (value != NULL ? value : "");
  return t;
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_new_list (const char *name, NuvashaLauncherNbtType elem_type)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_LIST, name);
  t->v_list.elem_type = elem_type;
  return t;
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_compound_get (const NuvashaLauncherNbtTag *compound,
                                  const char *name)
{
  if (compound == NULL || compound->type != NUVASHALAUNCHER_NBT_COMPOUND ||
      compound->v_compound == NULL || name == NULL)
    return NULL;

  for (guint i = 0; i < compound->v_compound->len; i++)
    {
      NuvashaLauncherNbtTag *child = g_ptr_array_index (compound->v_compound, i);
      if (g_strcmp0 (child->name, name) == 0)
        return child;
    }
  return NULL;
}

void
nuvashalauncher_nbt_compound_set (NuvashaLauncherNbtTag *compound,
                                  NuvashaLauncherNbtTag *child)
{
  if (compound == NULL || compound->type != NUVASHALAUNCHER_NBT_COMPOUND ||
      child == NULL)
    return;

  if (child->name != NULL)
    {
      for (guint i = 0; i < compound->v_compound->len; i++)
        {
          NuvashaLauncherNbtTag *existing = g_ptr_array_index (compound->v_compound, i);
          if (g_strcmp0 (existing->name, child->name) == 0)
            {
              g_ptr_array_remove_index (compound->v_compound, i);
              break;
            }
        }
    }
  g_ptr_array_add (compound->v_compound, child);
}

const char *
nuvashalauncher_nbt_compound_get_string (const NuvashaLauncherNbtTag *compound,
                                         const char *name)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_compound_get (compound, name);
  return (t != NULL && t->type == NUVASHALAUNCHER_NBT_STRING) ? t->v_string : NULL;
}

gint8
nuvashalauncher_nbt_compound_get_byte (const NuvashaLauncherNbtTag *compound,
                                       const char *name, gint8 fallback)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_compound_get (compound, name);
  return (t != NULL && t->type == NUVASHALAUNCHER_NBT_BYTE) ? t->v_byte : fallback;
}

gint32
nuvashalauncher_nbt_compound_get_int (const NuvashaLauncherNbtTag *compound,
                                      const char *name, gint32 fallback)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_compound_get (compound, name);
  return (t != NULL && t->type == NUVASHALAUNCHER_NBT_INT) ? t->v_int : fallback;
}

gint64
nuvashalauncher_nbt_compound_get_long (const NuvashaLauncherNbtTag *compound,
                                       const char *name, gint64 fallback)
{
  NuvashaLauncherNbtTag *t = nuvashalauncher_nbt_compound_get (compound, name);
  return (t != NULL && t->type == NUVASHALAUNCHER_NBT_LONG) ? t->v_long : fallback;
}

static gboolean
read_payload (NbtReader *r, NuvashaLauncherNbtTag *tag, guint depth, GError **error)
{
  if (depth > NBT_MAX_DEPTH)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "NBT nesting too deep");
      return FALSE;
    }

  switch (tag->type)
    {
    case NUVASHALAUNCHER_NBT_BYTE:
      {
        guint8 u = 0;
        if (!read_u8 (r, &u, error))
          return FALSE;
        tag->v_byte = (gint8) u;
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_SHORT:
      {
        guint16 u = 0;
        if (!read_u16_be (r, &u, error))
          return FALSE;
        tag->v_short = (gint16) u;
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_INT:
      {
        guint32 u = 0;
        if (!read_u32_be (r, &u, error))
          return FALSE;
        tag->v_int = (gint32) u;
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_LONG:
      {
        guint64 u = 0;
        if (!read_u64_be (r, &u, error))
          return FALSE;
        tag->v_long = (gint64) u;
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_FLOAT:
      {
        guint32 u = 0;
        if (!read_u32_be (r, &u, error))
          return FALSE;
        memcpy (&tag->v_float, &u, sizeof u);
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_DOUBLE:
      {
        guint64 u = 0;
        if (!read_u64_be (r, &u, error))
          return FALSE;
        memcpy (&tag->v_double, &u, sizeof u);
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_BYTE_ARRAY:
      {
        guint32 ulen = 0;
        if (!read_u32_be (r, &ulen, error))
          return FALSE;
        gint32 slen = (gint32) ulen;
        if (slen < 0 || (gsize) slen > r->len - r->pos)
          {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Invalid NBT byte array length");
            return FALSE;
          }
        tag->v_byte_array.len = (gsize) slen;
        tag->v_byte_array.data = slen > 0 ? g_malloc ((gsize) slen) : NULL;
        return read_bytes (r, tag->v_byte_array.data, (gsize) slen, error);
      }
    case NUVASHALAUNCHER_NBT_STRING:
      tag->v_string = read_string (r, error);
      return tag->v_string != NULL;
    case NUVASHALAUNCHER_NBT_LIST:
      {
        guint8 elem_u = 0;
        guint32 ulen = 0;
        if (!read_u8 (r, &elem_u, error) || !read_u32_be (r, &ulen, error))
          return FALSE;
        gint32 slen = (gint32) ulen;
        if (slen < 0 || elem_u > NUVASHALAUNCHER_NBT_LONG_ARRAY)
          {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Invalid NBT list header");
            return FALSE;
          }
        tag->v_list.elem_type = (NuvashaLauncherNbtType) elem_u;
        for (gint32 i = 0; i < slen; i++)
          {
            g_autoptr (NuvashaLauncherNbtTag) elem =
              nuvashalauncher_nbt_tag_new (tag->v_list.elem_type, NULL);
            if (!read_payload (r, elem, depth + 1, error))
              return FALSE;
            g_ptr_array_add (tag->v_list.items, g_steal_pointer (&elem));
          }
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_COMPOUND:
      while (TRUE)
        {
          guint8 child_type = 0;
          if (!read_u8 (r, &child_type, error))
            return FALSE;
          if (child_type == NUVASHALAUNCHER_NBT_END)
            return TRUE;
          if (child_type > NUVASHALAUNCHER_NBT_LONG_ARRAY)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                   "Unknown NBT tag type");
              return FALSE;
            }
          g_autofree char *child_name = read_string (r, error);
          if (child_name == NULL)
            return FALSE;
          g_autoptr (NuvashaLauncherNbtTag) child =
            nuvashalauncher_nbt_tag_new ((NuvashaLauncherNbtType) child_type, child_name);
          if (!read_payload (r, child, depth + 1, error))
            return FALSE;
          g_ptr_array_add (tag->v_compound, g_steal_pointer (&child));
        }
    case NUVASHALAUNCHER_NBT_INT_ARRAY:
      {
        guint32 ulen = 0;
        if (!read_u32_be (r, &ulen, error))
          return FALSE;
        gint32 slen = (gint32) ulen;
        if (slen < 0 || (gsize) slen > (r->len - r->pos) / 4)
          {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Invalid NBT int array length");
            return FALSE;
          }
        tag->v_int_array.len = (gsize) slen;
        tag->v_int_array.data = slen > 0 ? g_new (gint32, (gsize) slen) : NULL;
        for (gint32 i = 0; i < slen; i++)
          {
            guint32 u = 0;
            if (!read_u32_be (r, &u, error))
              return FALSE;
            tag->v_int_array.data[i] = (gint32) u;
          }
        return TRUE;
      }
    case NUVASHALAUNCHER_NBT_LONG_ARRAY:
      {
        guint32 ulen = 0;
        if (!read_u32_be (r, &ulen, error))
          return FALSE;
        gint32 slen = (gint32) ulen;
        if (slen < 0 || (gsize) slen > (r->len - r->pos) / 8)
          {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Invalid NBT long array length");
            return FALSE;
          }
        tag->v_long_array.len = (gsize) slen;
        tag->v_long_array.data = slen > 0 ? g_new (gint64, (gsize) slen) : NULL;
        for (gint32 i = 0; i < slen; i++)
          {
            guint64 u = 0;
            if (!read_u64_be (r, &u, error))
              return FALSE;
            tag->v_long_array.data[i] = (gint64) u;
          }
        return TRUE;
      }
    default:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Unsupported NBT tag type");
      return FALSE;
    }
}

static GBytes *
convert_bytes (const guint8 *data, gsize len, GConverter *conv, GError **error)
{
  g_autoptr (GMemoryOutputStream) mem =
    G_MEMORY_OUTPUT_STREAM (g_memory_output_stream_new_resizable ());
  g_autoptr (GOutputStream) out =
    g_converter_output_stream_new (G_OUTPUT_STREAM (mem), conv);
  gsize written = 0;

  if (!g_output_stream_write_all (out, data, len, &written, NULL, error) ||
      !g_output_stream_close (out, NULL, error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (mem);
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_parse (const guint8 *data, gsize len, GError **error)
{
  g_autoptr (GBytes) decompressed = NULL;
  NbtReader r;
  guint8 root_type = 0;

  if (data == NULL || len == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Empty NBT data");
      return NULL;
    }

  if (len >= 2 && data[0] == 0x1f && data[1] == 0x8b)
    {
      g_autoptr (GZlibDecompressor) decomp =
        g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      decompressed = convert_bytes (data, len, G_CONVERTER (decomp), error);
      if (decompressed == NULL)
        return NULL;
      r.data = g_bytes_get_data (decompressed, &r.len);
    }
  else
    {
      r.data = data;
      r.len = len;
    }
  r.pos = 0;

  if (!read_u8 (&r, &root_type, error))
    return NULL;
  if (root_type != NUVASHALAUNCHER_NBT_COMPOUND)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "NBT root must be a compound tag");
      return NULL;
    }

  g_autofree char *root_name = read_string (&r, error);
  if (root_name == NULL)
    return NULL;

  g_autoptr (NuvashaLauncherNbtTag) root =
    nuvashalauncher_nbt_tag_new (NUVASHALAUNCHER_NBT_COMPOUND, root_name);
  if (!read_payload (&r, root, 0, error))
    return NULL;

  return g_steal_pointer (&root);
}

NuvashaLauncherNbtTag *
nuvashalauncher_nbt_parse_file (const char *path, GError **error)
{
  g_autofree char *contents = NULL;
  gsize len = 0;

  if (!g_file_get_contents (path, &contents, &len, error))
    return NULL;

  return nuvashalauncher_nbt_parse ((const guint8 *) contents, len, error);
}

static void
write_u8 (GByteArray *out, guint8 v)
{
  g_byte_array_append (out, &v, 1);
}

static void
write_u16_be (GByteArray *out, guint16 v)
{
  guint8 buf[2] = { (guint8) (v >> 8), (guint8) v };
  g_byte_array_append (out, buf, 2);
}

static void
write_u32_be (GByteArray *out, guint32 v)
{
  guint8 buf[4] = {
    (guint8) (v >> 24), (guint8) (v >> 16), (guint8) (v >> 8), (guint8) v
  };
  g_byte_array_append (out, buf, 4);
}

static void
write_u64_be (GByteArray *out, guint64 v)
{
  guint8 buf[8] = {
    (guint8) (v >> 56), (guint8) (v >> 48), (guint8) (v >> 40), (guint8) (v >> 32),
    (guint8) (v >> 24), (guint8) (v >> 16), (guint8) (v >> 8), (guint8) v
  };
  g_byte_array_append (out, buf, 8);
}

static void
write_string (GByteArray *out, const char *s)
{
  const char *str = s != NULL ? s : "";
  gsize len = MIN (strlen (str), G_MAXUINT16);
  write_u16_be (out, (guint16) len);
  if (len > 0)
    g_byte_array_append (out, (const guint8 *) str, (guint) len);
}

static void
write_payload (GByteArray *out, const NuvashaLauncherNbtTag *tag)
{
  switch (tag->type)
    {
    case NUVASHALAUNCHER_NBT_BYTE:
      write_u8 (out, (guint8) tag->v_byte);
      break;
    case NUVASHALAUNCHER_NBT_SHORT:
      write_u16_be (out, (guint16) tag->v_short);
      break;
    case NUVASHALAUNCHER_NBT_INT:
      write_u32_be (out, (guint32) tag->v_int);
      break;
    case NUVASHALAUNCHER_NBT_LONG:
      write_u64_be (out, (guint64) tag->v_long);
      break;
    case NUVASHALAUNCHER_NBT_FLOAT:
      {
        guint32 u = 0;
        memcpy (&u, &tag->v_float, sizeof u);
        write_u32_be (out, u);
        break;
      }
    case NUVASHALAUNCHER_NBT_DOUBLE:
      {
        guint64 u = 0;
        memcpy (&u, &tag->v_double, sizeof u);
        write_u64_be (out, u);
        break;
      }
    case NUVASHALAUNCHER_NBT_BYTE_ARRAY:
      write_u32_be (out, (guint32) tag->v_byte_array.len);
      if (tag->v_byte_array.len > 0)
        g_byte_array_append (out, tag->v_byte_array.data, (guint) tag->v_byte_array.len);
      break;
    case NUVASHALAUNCHER_NBT_STRING:
      write_string (out, tag->v_string);
      break;
    case NUVASHALAUNCHER_NBT_LIST:
      {
        guint count = tag->v_list.items != NULL ? tag->v_list.items->len : 0;
        write_u8 (out, (guint8) tag->v_list.elem_type);
        write_u32_be (out, (guint32) count);
        for (guint i = 0; i < count; i++)
          write_payload (out, g_ptr_array_index (tag->v_list.items, i));
        break;
      }
    case NUVASHALAUNCHER_NBT_COMPOUND:
      if (tag->v_compound != NULL)
        {
          for (guint i = 0; i < tag->v_compound->len; i++)
            {
              const NuvashaLauncherNbtTag *child = g_ptr_array_index (tag->v_compound, i);
              write_u8 (out, (guint8) child->type);
              write_string (out, child->name);
              write_payload (out, child);
            }
        }
      write_u8 (out, NUVASHALAUNCHER_NBT_END);
      break;
    case NUVASHALAUNCHER_NBT_INT_ARRAY:
      write_u32_be (out, (guint32) tag->v_int_array.len);
      for (gsize i = 0; i < tag->v_int_array.len; i++)
        write_u32_be (out, (guint32) tag->v_int_array.data[i]);
      break;
    case NUVASHALAUNCHER_NBT_LONG_ARRAY:
      write_u32_be (out, (guint32) tag->v_long_array.len);
      for (gsize i = 0; i < tag->v_long_array.len; i++)
        write_u64_be (out, (guint64) tag->v_long_array.data[i]);
      break;
    default:
      break;
    }
}

GBytes *
nuvashalauncher_nbt_serialize (const NuvashaLauncherNbtTag *root,
                               gboolean gzip,
                               GError **error)
{
  g_autoptr (GByteArray) buf = g_byte_array_new ();

  g_return_val_if_fail (root != NULL && root->type == NUVASHALAUNCHER_NBT_COMPOUND, NULL);

  write_u8 (buf, (guint8) root->type);
  write_string (buf, root->name);
  write_payload (buf, root);

  if (gzip)
    {
      g_autoptr (GZlibCompressor) comp =
        g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
      return convert_bytes (buf->data, buf->len, G_CONVERTER (comp), error);
    }

  return g_byte_array_free_to_bytes (g_steal_pointer (&buf));
}

gboolean
nuvashalauncher_nbt_write_file (const NuvashaLauncherNbtTag *root,
                                const char *path,
                                gboolean gzip,
                                GError **error)
{
  g_autoptr (GBytes) bytes = nuvashalauncher_nbt_serialize (root, gzip, error);
  gsize len = 0;
  const char *data = NULL;

  if (bytes == NULL)
    return FALSE;

  data = g_bytes_get_data (bytes, &len);
  return g_file_set_contents (path, data, (gssize) len, error);
}
