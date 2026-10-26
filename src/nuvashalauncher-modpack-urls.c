/* Modpack download URL validation. */

#include "nuvashalauncher-modpack.h"

static const char *allowed_hosts[] = {
  "cdn.modrinth.com",
  "github.com",
  "raw.githubusercontent.com",
  "gitlab.com",
};

gboolean
nuvashalauncher_modpack_download_url_allowed (const char *url)
{
  g_autoptr (GUri) uri = NULL;
  const char *scheme = NULL;
  const char *host = NULL;
  const char *userinfo = NULL;

  if (url == NULL)
    return FALSE;

  uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  if (uri == NULL)
    return FALSE;

  scheme = g_uri_get_scheme (uri);
  host = g_uri_get_host (uri);
  userinfo = g_uri_get_userinfo (uri);

  if (scheme == NULL || g_ascii_strcasecmp (scheme, "https") != 0 ||
      host == NULL || userinfo != NULL || g_uri_get_port (uri) != -1)
    return FALSE;

  for (gsize i = 0; i < G_N_ELEMENTS (allowed_hosts); i++)
    if (g_ascii_strcasecmp (host, allowed_hosts[i]) == 0)
      return TRUE;

  return FALSE;
}

gboolean
nuvashalauncher_modpack_redirect_allowed (const char *original,
                                          const char *redirected)
{
  return !nuvashalauncher_modpack_download_url_allowed (original) ||
         nuvashalauncher_modpack_download_url_allowed (redirected);
}
