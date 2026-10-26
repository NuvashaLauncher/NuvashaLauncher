/* Modpack path validation. */

#include "nuvashalauncher-modpack.h"

#include <string.h>

gboolean
nuvashalauncher_modpack_path_is_safe (const char *path)
{
  g_auto (GStrv) parts = NULL;

  if (path == NULL || *path == '\0')
    return FALSE;

  /* Every byte a name has goes through here before it is a path on the
   * disk: a control character (NUL is one of them) or a DEL has no
   * business inside a file name. */
  for (const char *c = path; *c != '\0'; c++)
    if ((guchar) *c < 0x20 || (guchar) *c == 0x7f)
      return FALSE;

  /* A pack written for another system may use the \ as the separator:
   * it must not be a separator on this one, and a traversal written
   * with it must not turn into one. */
  if (strchr (path, '\\') != NULL)
    return FALSE;

  if (g_path_is_absolute (path))
    return FALSE;

  /* A drive name, which a path has no business dragging to a client
   * that has no drives: "C:/mods". */
  if (g_ascii_isalpha (path[0]) && path[1] == ':' &&
      (path[2] == '\0' || path[2] == '/'))
    return FALSE;

  parts = g_strsplit (path, "/", -1);

  for (guint i = 0; parts[i] != NULL; i++)
    {
      /* "" names the point to skip over: a // or a trailing / is not
       * part of a name a pack may use. "." resolves to itself but a ".."
       * is a step out of the instance, and a name may not name an
       * empty part. */
      if (parts[i][0] == '\0')
        return FALSE;

      if (g_strcmp0 (parts[i], ".") == 0 ||
          g_strcmp0 (parts[i], "..") == 0)
        return FALSE;
    }

  return TRUE;
}
