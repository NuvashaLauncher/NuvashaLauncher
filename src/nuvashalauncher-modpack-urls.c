/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Modpack download URL validation. */

#include "nuvashalauncher-modpack.h"

static const char *allowed_hosts[] = {
  "cdn.modrinth.com",
  "github.com",
  "raw.githubusercontent.com",
  "gitlab.com",
};

static const char *github_redirect_hosts[] = {
  "release-assets.githubusercontent.com",
  "objects.githubusercontent.com",
};

static gboolean
url_matches_hosts (const char *url, const char *const *hosts, gsize n_hosts)
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

  for (gsize i = 0; i < n_hosts; i++)
    if (g_ascii_strcasecmp (host, hosts[i]) == 0)
      return TRUE;

  return FALSE;
}

gboolean
nuvashalauncher_modpack_download_url_allowed (const char *url)
{
  return url_matches_hosts (url, allowed_hosts, G_N_ELEMENTS (allowed_hosts));
}

gboolean
nuvashalauncher_modpack_redirect_allowed (const char *original,
                                          const char *redirected)
{
  static const char *const github_only[] = { "github.com" };

  if (!nuvashalauncher_modpack_download_url_allowed (original))
    return TRUE;

  if (nuvashalauncher_modpack_download_url_allowed (redirected))
    return TRUE;

  if (url_matches_hosts (original, github_only, G_N_ELEMENTS (github_only)) &&
      url_matches_hosts (redirected, github_redirect_hosts,
                         G_N_ELEMENTS (github_redirect_hosts)))
    return TRUE;

  return FALSE;
}
