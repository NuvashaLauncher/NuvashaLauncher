/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Asynchronous Minecraft Server List Ping (SLP) implementation. */

#include "nuvashalauncher-server-ping.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#define SLP_DEFAULT_PORT 25565
#define SLP_TIMEOUT_SECONDS 5
#define SLP_MAX_PACKET_LEN (2 * 1024 * 1024)
#define SLP_PROTOCOL_VERSION 767

void
nuvashalauncher_server_status_free (NuvashaLauncherServerStatus *status)
{
  if (status == NULL)
    return;
  g_free (status->icon_b64);
  g_free (status);
}

static gboolean
parse_server_address (const char *address,
                      char **out_host,
                      guint16 *out_port,
                      gboolean *out_explicit_port)
{
  g_autofree char *trimmed = NULL;
  const char *colon;

  *out_host = NULL;
  *out_port = SLP_DEFAULT_PORT;
  *out_explicit_port = FALSE;

  if (address == NULL)
    return FALSE;

  trimmed = g_strstrip (g_strdup (address));
  if (*trimmed == '\0')
    return FALSE;

  if (trimmed[0] == '[')
    {
      const char *close = strchr (trimmed, ']');
      if (close == NULL || close == trimmed + 1)
        return FALSE;
      *out_host = g_strndup (trimmed + 1, (gsize) (close - (trimmed + 1)));
      if (close[1] == ':' && close[2] != '\0')
        {
          char *end = NULL;
          unsigned long p = strtoul (close + 2, &end, 10);
          if (end == close + 2 || *end != '\0' || p == 0 || p > 65535)
            return FALSE;
          *out_port = (guint16) p;
          *out_explicit_port = TRUE;
        }
      return TRUE;
    }

  colon = strrchr (trimmed, ':');
  if (colon != NULL && strchr (trimmed, ':') == colon)
    {
      char *end = NULL;
      unsigned long p = strtoul (colon + 1, &end, 10);
      if (colon == trimmed || end == colon + 1 || *end != '\0' || p == 0 || p > 65535)
        return FALSE;
      *out_host = g_strndup (trimmed, (gsize) (colon - trimmed));
      *out_port = (guint16) p;
      *out_explicit_port = TRUE;
      return TRUE;
    }

  *out_host = g_strdup (trimmed);
  return TRUE;
}

static void
append_varint (GByteArray *buf, guint32 value)
{
  do
    {
      guint8 byte = (guint8) (value & 0x7FU);
      value >>= 7;
      if (value != 0)
        byte |= 0x80U;
      g_byte_array_append (buf, &byte, 1);
    }
  while (value != 0);
}

static void
append_string (GByteArray *buf, const char *str)
{
  gsize len = strlen (str);
  append_varint (buf, (guint32) len);
  g_byte_array_append (buf, (const guint8 *) str, (guint) len);
}

static gboolean
read_varint_stream (GInputStream *in,
                    guint32 *out_value,
                    gsize *out_bytes_read,
                    GCancellable *cancellable)
{
  guint32 result = 0;
  guint shift = 0;
  gsize count = 0;

  for (guint i = 0; i < 5; i++)
    {
      guint8 byte = 0;
      gsize n_read = 0;

      if (!g_input_stream_read_all (in, &byte, 1, &n_read, cancellable, NULL) ||
          n_read != 1)
        return FALSE;

      count++;
      result |= (guint32) (byte & 0x7FU) << shift;
      if ((byte & 0x80U) == 0)
        {
          *out_value = result;
          if (out_bytes_read != NULL)
            *out_bytes_read = count;
          return TRUE;
        }
      shift += 7;
    }

  return FALSE;
}

static gboolean
read_varint_buf (const guint8 *data, gsize len, gsize *pos, guint32 *out_value)
{
  guint32 result = 0;
  guint shift = 0;

  for (guint i = 0; i < 5; i++)
    {
      if (*pos >= len)
        return FALSE;
      guint8 byte = data[(*pos)++];
      result |= (guint32) (byte & 0x7FU) << shift;
      if ((byte & 0x80U) == 0)
        {
          *out_value = result;
          return TRUE;
        }
      shift += 7;
    }

  return FALSE;
}

static gboolean
send_packet (GOutputStream *out,
             const GByteArray *payload,
             GCancellable *cancellable)
{
  g_autoptr (GByteArray) framed = g_byte_array_new ();

  append_varint (framed, payload->len);
  g_byte_array_append (framed, payload->data, payload->len);
  return g_output_stream_write_all (out, framed->data, framed->len,
                                    NULL, cancellable, NULL);
}

static char *
extract_favicon_b64 (const char *favicon)
{
  static const char prefix[] = "data:image/png;base64,";
  GString *clean;

  if (favicon == NULL || !g_str_has_prefix (favicon, prefix))
    return NULL;

  clean = g_string_new (NULL);
  for (const char *p = favicon + sizeof (prefix) - 1; *p != '\0'; p++)
    {
      if (*p != '\r' && *p != '\n' && *p != ' ' && *p != '\t')
        g_string_append_c (clean, *p);
    }

  if (clean->len == 0)
    {
      g_string_free (clean, TRUE);
      return NULL;
    }

  return g_string_free (clean, FALSE);
}

static void
parse_status_json (const char *json,
                   gsize json_len,
                   NuvashaLauncherServerStatus *status)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonObject *obj;

  if (!json_parser_load_from_data (parser, json, (gssize) json_len, NULL))
    return;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return;

  obj = json_node_get_object (root);

  if (json_object_has_member (obj, "players"))
    {
      JsonNode *players_node = json_object_get_member (obj, "players");
      if (players_node != NULL && JSON_NODE_HOLDS_OBJECT (players_node))
        {
          JsonObject *players = json_node_get_object (players_node);
          if (json_object_has_member (players, "online"))
            {
              JsonNode *n = json_object_get_member (players, "online");
              if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
                status->players_online = json_node_get_int (n);
            }
          if (json_object_has_member (players, "max"))
            {
              JsonNode *n = json_object_get_member (players, "max");
              if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
                status->players_max = json_node_get_int (n);
            }
        }
    }

  if (json_object_has_member (obj, "favicon"))
    {
      JsonNode *fav_node = json_object_get_member (obj, "favicon");
      if (fav_node != NULL && JSON_NODE_HOLDS_VALUE (fav_node) &&
          json_node_get_value_type (fav_node) == G_TYPE_STRING)
        {
          status->icon_b64 = extract_favicon_b64 (json_node_get_string (fav_node));
        }
    }
}

static void
ping_worker (GTask *task,
             gpointer source_object G_GNUC_UNUSED,
             gpointer task_data,
             GCancellable *cancellable)
{
  const char *address = task_data;
  g_autofree char *host = NULL;
  g_autofree char *connect_host = NULL;
  guint16 port = SLP_DEFAULT_PORT;
  guint16 connect_port = SLP_DEFAULT_PORT;
  gboolean explicit_port = FALSE;
  g_autoptr (NuvashaLauncherServerStatus) status = g_new0 (NuvashaLauncherServerStatus, 1);
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketConnection) conn = NULL;
  GInputStream *in;
  GOutputStream *out;
  g_autoptr (GByteArray) handshake = NULL;
  g_autoptr (GByteArray) status_req = NULL;
  guint32 packet_len = 0;
  g_autofree guint8 *packet_buf = NULL;
  gsize n_read = 0;
  gsize pos = 0;
  guint32 packet_id = 0;
  guint32 json_len = 0;
  gint64 t_status_start;
  gint64 t_status_end;

  status->online = FALSE;
  status->players_online = -1;
  status->players_max = -1;
  status->ping_ms = -1;

  if (!parse_server_address (address, &host, &port, &explicit_port))
    {
      g_task_return_pointer (task, g_steal_pointer (&status),
                             (GDestroyNotify) nuvashalauncher_server_status_free);
      return;
    }

  connect_host = g_strdup (host);
  connect_port = port;

  /* Resolve Minecraft SRV record when no explicit port was supplied. */
  if (!explicit_port)
    {
      g_autoptr (GResolver) resolver = g_resolver_get_default ();
      GList *targets = g_resolver_lookup_service (resolver, "minecraft", "tcp",
                                                  host, cancellable, NULL);
      if (targets != NULL)
        {
          GSrvTarget *srv = targets->data;
          const char *srv_host = g_srv_target_get_hostname (srv);
          guint16 srv_port = g_srv_target_get_port (srv);
          if (srv_host != NULL && *srv_host != '\0' && srv_port > 0)
            {
              g_free (connect_host);
              connect_host = g_strdup (srv_host);
              connect_port = srv_port;
            }
          g_resolver_free_targets (targets);
        }
    }

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
      return;
    }

  client = g_socket_client_new ();
  g_socket_client_set_timeout (client, SLP_TIMEOUT_SECONDS);
  conn = g_socket_client_connect_to_host (client, connect_host, connect_port,
                                          cancellable, NULL);
  if (conn == NULL)
    {
      if (g_cancellable_is_cancelled (cancellable))
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
      else
        g_task_return_pointer (task, g_steal_pointer (&status),
                               (GDestroyNotify) nuvashalauncher_server_status_free);
      return;
    }

  in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  out = g_io_stream_get_output_stream (G_IO_STREAM (conn));

  /* Handshake (0x00) */
  handshake = g_byte_array_new ();
  append_varint (handshake, 0x00);
  append_varint (handshake, SLP_PROTOCOL_VERSION);
  append_string (handshake, host);
  {
    guint8 port_be[2] = { (guint8) (port >> 8), (guint8) (port & 0xFFU) };
    g_byte_array_append (handshake, port_be, 2);
  }
  append_varint (handshake, 1); /* next state: status */

  /* Status Request (0x00) */
  status_req = g_byte_array_new ();
  append_varint (status_req, 0x00);

  t_status_start = g_get_monotonic_time ();
  if (!send_packet (out, handshake, cancellable) ||
      !send_packet (out, status_req, cancellable))
    {
      g_task_return_pointer (task, g_steal_pointer (&status),
                             (GDestroyNotify) nuvashalauncher_server_status_free);
      return;
    }

  if (!read_varint_stream (in, &packet_len, NULL, cancellable) ||
      packet_len == 0 || packet_len > SLP_MAX_PACKET_LEN)
    {
      g_task_return_pointer (task, g_steal_pointer (&status),
                             (GDestroyNotify) nuvashalauncher_server_status_free);
      return;
    }

  packet_buf = g_malloc (packet_len);
  if (!g_input_stream_read_all (in, packet_buf, packet_len, &n_read, cancellable, NULL) ||
      n_read != packet_len)
    {
      g_task_return_pointer (task, g_steal_pointer (&status),
                             (GDestroyNotify) nuvashalauncher_server_status_free);
      return;
    }
  t_status_end = g_get_monotonic_time ();

  if (!read_varint_buf (packet_buf, packet_len, &pos, &packet_id) ||
      packet_id != 0x00 ||
      !read_varint_buf (packet_buf, packet_len, &pos, &json_len) ||
      json_len > packet_len - pos)
    {
      g_task_return_pointer (task, g_steal_pointer (&status),
                             (GDestroyNotify) nuvashalauncher_server_status_free);
      return;
    }

  status->online = TRUE;
  status->ping_ms = MAX (0, (t_status_end - t_status_start) / 1000);
  parse_status_json ((const char *) (packet_buf + pos), json_len, status);

  /* Ping Request (0x01) + 8-byte payload for accurate RTT measurement. */
  {
    g_autoptr (GByteArray) ping_pkt = g_byte_array_new ();
    guint64 token = (guint64) g_get_monotonic_time ();
    guint8 token_be[8];
    gint64 t_ping_start;
    guint32 pong_len = 0;

    for (int i = 7; i >= 0; i--)
      {
        token_be[i] = (guint8) (token & 0xFFU);
        token >>= 8;
      }
    append_varint (ping_pkt, 0x01);
    g_byte_array_append (ping_pkt, token_be, 8);

    t_ping_start = g_get_monotonic_time ();
    if (send_packet (out, ping_pkt, cancellable) &&
        read_varint_stream (in, &pong_len, NULL, cancellable) &&
        pong_len > 0 && pong_len <= 64)
      {
        guint8 pong_buf[64];
        gsize pong_read = 0;
        if (g_input_stream_read_all (in, pong_buf, pong_len, &pong_read, cancellable, NULL) &&
            pong_read == pong_len)
          {
            gint64 rtt = (g_get_monotonic_time () - t_ping_start) / 1000;
            status->ping_ms = MAX (0, rtt);
          }
      }
  }

  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_task_return_pointer (task, g_steal_pointer (&status),
                         (GDestroyNotify) nuvashalauncher_server_status_free);
}

typedef struct {
  NuvashaLauncherTaskScope *scope;
} PingWrap;

static void
on_ping_worker_done (GObject *source G_GNUC_UNUSED,
                     GAsyncResult *result,
                     gpointer user_data)
{
  PingWrap *wrap = user_data;
  g_autoptr (GError) error = NULL;
  NuvashaLauncherServerStatus *status = g_task_propagate_pointer (G_TASK (result), &error);

  if (status != NULL)
    nuvashalauncher_task_scope_return_pointer (
      wrap->scope, status, (GDestroyNotify) nuvashalauncher_server_status_free);
  else
    nuvashalauncher_task_scope_return_error (wrap->scope, g_steal_pointer (&error));

  nuvashalauncher_task_scope_unref (wrap->scope);
  g_free (wrap);
}

NuvashaLauncherTaskScope *
nuvashalauncher_server_ping_async (const char *address,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  NuvashaLauncherTaskScope *scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  PingWrap *wrap = g_new0 (PingWrap, 1);
  g_autoptr (GTask) worker = NULL;

  wrap->scope = nuvashalauncher_task_scope_ref (scope);
  worker = g_task_new (NULL, nuvashalauncher_task_scope_get_cancellable (scope),
                       on_ping_worker_done, wrap);
  g_task_set_task_data (worker, g_strdup (address != NULL ? address : ""), g_free);
  g_task_run_in_thread (worker, ping_worker);

  return scope;
}

NuvashaLauncherServerStatus *
nuvashalauncher_server_ping_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
