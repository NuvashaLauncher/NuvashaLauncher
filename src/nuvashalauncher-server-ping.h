/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* NuvashaLauncherServerPing: asynchronous Minecraft Server List Ping (SLP)
 * client for querying online status, player counts, latency, and favicon. */

#ifndef NUVASHALAUNCHER_SERVER_PING_H
#define NUVASHALAUNCHER_SERVER_PING_H

#include <gio/gio.h>

#include "nuvashalauncher-task.h"

G_BEGIN_DECLS

typedef struct {
  gboolean online;
  gint64 players_online; /* -1 if unavailable */
  gint64 players_max;    /* -1 if unavailable */
  gint64 ping_ms;        /* round-trip latency in ms, or -1 */
  char *icon_b64;        /* raw base64 PNG (without data URI prefix), or NULL */
} NuvashaLauncherServerStatus;

void nuvashalauncher_server_status_free (NuvashaLauncherServerStatus *status);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NuvashaLauncherServerStatus,
                               nuvashalauncher_server_status_free)

/* Pings a Minecraft server address (e.g. "hypixel.net" or "127.0.0.1:25565")
 * asynchronously off the main loop. Always returns a NuvashaLauncherServerStatus
 * (with online == FALSE on timeout/connection failure) unless cancelled. */
NuvashaLauncherTaskScope *nuvashalauncher_server_ping_async (
  const char *address,
  GAsyncReadyCallback callback,
  gpointer user_data);

NuvashaLauncherServerStatus *nuvashalauncher_server_ping_finish (
  GAsyncResult *result,
  GError **error);

G_END_DECLS

#endif /* NUVASHALAUNCHER_SERVER_PING_H */
