/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "nuvashalauncher-errors.h"
#include "nuvashalauncher-i18n.h"
#include "nuvashalauncher-auth-microsoft.h"
#include "nuvashalauncher-download.h"
#include "nuvashalauncher-modrinth.h"
#include "nuvashalauncher-modpack.h"
#include "nuvashalauncher-minecraft-services.h"
#include "nuvashalauncher-minecraft-versions.h"
#include "nuvashalauncher-minecraft-resolve.h"
#include "nuvashalauncher-instance-manager.h"
#include "nuvashalauncher-loaders.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-launcher.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-instance.h"
#include <json-glib/json-glib.h>

gboolean
nuvashalauncher_error_is_cancelled (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
    g_error_matches (error, nuvashalauncher_DOWNLOAD_ERROR, nuvashalauncher_DOWNLOAD_ERROR_CANCELLED) ||
    g_error_matches (error, nuvashalauncher_MODPACK_ERROR, nuvashalauncher_MODPACK_ERROR_CANCELLED);
}

const char *
nuvashalauncher_error_message (const GError *error)
{
  const char *network = _("The connection failed. Check your internet connection and try again.");
  const char *response = _("The service returned an invalid response. Try again later.");
  const char *http = _("The service rejected the request. Try again later.");
  const char *rate = _("Too many requests. Wait a moment and try again.");
  const char *io = _("Could not read or write files. Check the location, permissions, and free disk space.");
  const char *hash = _("The downloaded file failed verification. It was not installed. Try downloading it again.");
  const char *archive = _("The archive is invalid or unsafe. Download it again from a trusted source.");

  if (error == NULL)
    return _("The operation failed. See Technical Details for more information.");
  if (error->domain == G_RESOLVER_ERROR || error->domain == G_TLS_ERROR)
    return network;
  if (error->domain == JSON_PARSER_ERROR)
    return response;
  if (error->domain == G_SPAWN_ERROR)
    return _("The Java executable could not be used. Select a working Java runtime.");
  if (error->domain == G_IO_ERROR)
    {
      switch (error->code)
        {
        case G_IO_ERROR_HOST_NOT_FOUND:
        case G_IO_ERROR_HOST_UNREACHABLE:
        case G_IO_ERROR_NETWORK_UNREACHABLE:
        case G_IO_ERROR_CONNECTION_REFUSED:
        case G_IO_ERROR_CONNECTION_CLOSED:
        case G_IO_ERROR_TIMED_OUT:
        case G_IO_ERROR_NOT_CONNECTED:
          return network;
        case G_IO_ERROR_PERMISSION_DENIED:
          return _("Access was denied. Check the folder permissions.");
        case G_IO_ERROR_NO_SPACE:
          return _("There is not enough free disk space.");
        case G_IO_ERROR_NOT_FOUND:
          return _("The file or folder was not found. Check its location.");
        case G_IO_ERROR_EXISTS:
          return _("The destination already exists. Choose another location.");
        case G_IO_ERROR_FAILED:
        case G_IO_ERROR_INVALID_DATA:
          return _("The operation failed. See Technical Details for more information.");
        default: return io;
        }
    }
  if (error->domain == G_FILE_ERROR)
    {
      if (error->code == G_FILE_ERROR_ACCES || error->code == G_FILE_ERROR_PERM)
        return _("Access was denied. Check the folder permissions.");
      if (error->code == G_FILE_ERROR_NOSPC)
        return _("There is not enough free disk space.");
      return io;
    }
  if (error->domain == nuvashalauncher_AUTH_ERROR)
    {
      switch (error->code)
        {
        case nuvashalauncher_AUTH_ERROR_NETWORK: return network;
        case nuvashalauncher_AUTH_ERROR_PARSE: return response;
        case nuvashalauncher_AUTH_ERROR_EXPIRED:
          return _("The sign-in request expired. Start again to get a new code.");
        case nuvashalauncher_AUTH_ERROR_REAUTH:
          return _("Your saved sign-in is no longer valid. Sign in again.");
        case nuvashalauncher_AUTH_ERROR_DECLINED:
          return _("Sign-in was declined. Start again if you want to sign in.");
        case nuvashalauncher_AUTH_ERROR_NO_GAME:
          return _("This account does not own Minecraft: Java Edition.");
        case nuvashalauncher_AUTH_ERROR_NO_PROFILE:
          return _("Set up a Minecraft: Java Edition profile on minecraft.net, then try again.");
        case nuvashalauncher_AUTH_ERROR_NO_CLIENT_ID:
          return _("Microsoft sign-in is not configured in this build. Update the launcher.");
        case nuvashalauncher_AUTH_ERROR_XBOX_PROFILE:
          return _("this Microsoft account has no Xbox profile behind it yet: sign in to xbox.com once with it, then sign in here again");
        case nuvashalauncher_AUTH_ERROR_XBOX_FAMILY:
          return _("this is a child account: it has to be added to a Microsoft family by an adult before it can play");
        case nuvashalauncher_AUTH_ERROR_XBOX_COUNTRY:
          return _("Xbox Live is not available for the country this account is registered in");
        case nuvashalauncher_AUTH_ERROR_XBOX_AGE:
          return _("this account needs an adult to verify it before it can play, which is what South Korean accounts have to do");
        default: return _("Microsoft sign-in failed. Try again.");
        }
    }
  if (error->domain == nuvashalauncher_DOWNLOAD_ERROR)
    {
      switch (error->code)
        {
        case nuvashalauncher_DOWNLOAD_ERROR_NETWORK: return network;
        case nuvashalauncher_DOWNLOAD_ERROR_HTTP: return http;
        case nuvashalauncher_DOWNLOAD_ERROR_RATE_LIMITED: return rate;
        case nuvashalauncher_DOWNLOAD_ERROR_CHECKSUM: return hash;
        case nuvashalauncher_DOWNLOAD_ERROR_INVALID: return response;
        default: return io;
        }
    }
  if (error->domain == nuvashalauncher_MODRINTH_ERROR)
    {
      switch (error->code)
        {
        case nuvashalauncher_MODRINTH_ERROR_NETWORK: return network;
        case nuvashalauncher_MODRINTH_ERROR_HTTP: return http;
        case nuvashalauncher_MODRINTH_ERROR_RATE_LIMIT: return rate;
        case nuvashalauncher_MODRINTH_ERROR_NOT_FOUND:
          return _("The requested file, project, or compatible version was not found.");
        default: return response;
        }
    }
  if (error->domain == nuvashalauncher_MINECRAFT_SERVICES_ERROR)
    {
      switch (error->code)
        {
        case nuvashalauncher_MINECRAFT_SERVICES_ERROR_NETWORK: return network;
        case nuvashalauncher_MINECRAFT_SERVICES_ERROR_RATE_LIMITED: return rate;
        case nuvashalauncher_MINECRAFT_SERVICES_ERROR_HTTP: return http;
        default: return response;
        }
    }
  if (error->domain == nuvashalauncher_MODPACK_ERROR)
    {
      switch (error->code)
        {
        case nuvashalauncher_MODPACK_ERROR_HASH: return hash;
        case nuvashalauncher_MODPACK_ERROR_IO: return io;
        case nuvashalauncher_MODPACK_ERROR_DOWNLOAD: return _("Pack could not be fetched");
        case nuvashalauncher_MODPACK_ERROR_LOADER:
          return _("Could not install this mod loader. Choose another version or inspect Technical Details.");
        default: return archive;
        }
    }
  if (error->domain == nuvashalauncher_LOADERS_ERROR)
    return _("Could not install this mod loader. Choose another version or inspect Technical Details.");
  if (error->domain == nuvashalauncher_JAVA_ERROR)
    return _("The Java executable could not be used. Select a working Java runtime.");
  if (error->domain == nuvashalauncher_MC_ERROR)
    return _("The game metadata is invalid or unavailable. Refresh the version list and try again.");
  if (error->domain == nuvashalauncher_RESOLVE_ERROR)
    {
      if (error->code == nuvashalauncher_RESOLVE_ERROR_IO)
        return io;
      if (error->code == nuvashalauncher_RESOLVE_ERROR_UNSAFE_PATH)
        return archive;
      return _("The game metadata is invalid or unavailable. Refresh the version list and try again.");
    }
  if (error->domain == nuvashalauncher_INSTANCE_MANAGER_ERROR)
    {
      if (g_str_has_prefix (error->message, "the instance directory must be an absolute path"))
        return _("Choose another instance name or directory.");
      if (g_str_has_prefix (error->message, "the instance directory must not contain"))
        return _("Choose another instance name or directory.");
      if (g_str_has_prefix (error->message, "the parent of the instance directory"))
        return _("Choose another instance name or directory.");
      if (g_str_has_prefix (error->message, "the filesystem root"))
        return _("Choose another instance name or directory.");
      if (g_str_has_prefix (error->message, "the nuvashalauncher data directory"))
        return _("Choose another instance name or directory.");
      if (g_str_has_prefix (error->message, "the directory is already used"))
        return _("The destination already exists. Choose another location.");
      if (g_str_has_prefix (error->message, "the directory "))
        return _("Choose another instance name or directory.");
      if (error->code == nuvashalauncher_INSTANCE_MANAGER_ERROR_IO)
        return io;
      if (error->code == nuvashalauncher_INSTANCE_MANAGER_ERROR_NOT_FOUND)
        return _("This instance is no longer available. Refresh the instance list.");
      return _("Choose another instance name or directory.");
    }
  if (error->domain == nuvashalauncher_LAUNCH_ERROR)
    return _("Your instance settings are invalid. Check the selected version and JVM arguments.");
  if (error->domain == nuvashalauncher_INSTANCE_ERROR)
    return _("Choose another instance name or directory.");
  if (error->domain == nuvashalauncher_SETTINGS_ERROR)
    return io;
  if (g_error_matches (error, nuvashalauncher_ACCOUNTS_ERROR, nuvashalauncher_ACCOUNTS_ERROR_IO))
    return _("Could not save the account. Check your system keyring and file permissions.");
  if (g_error_matches (error, nuvashalauncher_ACCOUNTS_ERROR,
                       nuvashalauncher_ACCOUNTS_ERROR_INVALID))
    return error->message;
  return _("The operation failed. See Technical Details for more information.");
}
