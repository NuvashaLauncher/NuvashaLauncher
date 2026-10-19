/* Modpack download URL validation. */

#include "nuvasha-modpack.h"

#include <string.h>

static const char *allowed_hosts[] = {
  "cdn.modrinth.com",
  "github.com",
  "raw.githubusercontent.com",
  "gitlab.com",
};

gboolean
nuvasha_modpack_download_url_allowed (const char *url)
{
  const char *authority = NULL;
  const char *rest = NULL;
  gsize length = 0;
  gsize i = 0;

  if (url == NULL || !g_str_has_prefix (url, "https://"))
    return FALSE;

  authority = url + strlen ("https://");
  rest = strchr (authority, '/');

  length = rest != NULL ? (gsize) (rest - authority) : strlen (authority);

  if (length == 0)
    return FALSE;

  for (i = 0; i < G_N_ELEMENTS (allowed_hosts); i++)
    if (length == strlen (allowed_hosts[i]) &&
        g_str_has_prefix (authority, allowed_hosts[i]))
      return TRUE;

  return FALSE;
}
