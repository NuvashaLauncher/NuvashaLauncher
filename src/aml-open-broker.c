/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _XOPEN_SOURCE 700

#include "aml-open-broker.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static GSocketService *service;
static char *socket_path;
static char *allowed_root;

static gboolean
path_allowed (const char *path)
{
  char root[PATH_MAX], real_path[PATH_MAX];
  size_t length;

  if (realpath (allowed_root, root) == NULL ||
      realpath (path, real_path) == NULL)
    return FALSE;

  length = strlen (root);
  return g_str_has_prefix (real_path, root) &&
         (real_path[length] == '\0' || real_path[length] == '/');
}

static gboolean
broker_incoming (GSocketService *source G_GNUC_UNUSED,
                 GSocketConnection *connection,
                 GObject *object G_GNUC_UNUSED,
                 gpointer user_data G_GNUC_UNUSED)
{
  GInputStream *input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  GOutputStream *output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  char request[PATH_MAX + 32] = { 0 };
  gssize count;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;

  count = g_input_stream_read (input, request, sizeof request - 1, NULL, &error);
  if (count <= 0 || error != NULL || memchr (request, '\n', count) == NULL)
    return TRUE;
  request[strcspn (request, "\r\n")] = '\0';

  if (!g_str_has_prefix (request, "file://"))
    return TRUE;

  file = g_file_new_for_uri (request);
  path = g_file_get_path (file);
  if (path == NULL || !path_allowed (path))
    return TRUE;

  if (!g_app_info_launch_default_for_uri (request, NULL, &error))
    g_debug ("open broker rejected %s: %s", request, error->message);
  else
    g_output_stream_write (output, "OK\n", 3, NULL, NULL);

  return TRUE;
}

const char *
aml_open_broker_start (const char *root, GError **error)
{
  g_autoptr (GSocketAddress) address = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *name = NULL;

  g_return_val_if_fail (root != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  if (service != NULL)
    return socket_path;

  runtime = g_strdup (g_get_user_runtime_dir ());
  name = g_strdup_printf ("aml-open-%u.sock", (guint) getpid ());
  socket_path = g_build_filename (runtime, name, NULL);
  allowed_root = realpath (root, NULL);
  if (allowed_root == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "cannot resolve sandbox instance directory");
      g_clear_pointer (&socket_path, g_free);
      return NULL;
    }

  service = g_socket_service_new ();
  address = g_unix_socket_address_new (socket_path);
  if (!g_socket_listener_add_address (G_SOCKET_LISTENER (service), address,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
                                      error))
    {
      g_clear_object (&service);
      g_clear_pointer (&socket_path, g_free);
      g_clear_pointer (&allowed_root, g_free);
      return NULL;
    }
  chmod (socket_path, 0600);
  g_signal_connect (service, "incoming", G_CALLBACK (broker_incoming), NULL);
  g_socket_service_start (service);
  return socket_path;
}

int
aml_open_broker_request (const char *path, const char *uri)
{
  g_autoptr (GSocket) socket = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *file_uri = NULL;
  char response[4] = { 0 };

  if (path == NULL || uri == NULL)
    return 2;
  if (g_str_has_prefix (uri, "file://"))
    file_uri = g_strdup (uri);
  else if (uri[0] == '/')
    file_uri = g_filename_to_uri (uri, NULL, NULL);
  else
    return 2;
  if (file_uri == NULL)
    return 2;
  socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                         G_SOCKET_PROTOCOL_DEFAULT, &error);
  address = g_unix_socket_address_new (path);
  if (socket == NULL || !g_socket_connect (socket, address, NULL, &error) ||
      g_socket_send (socket, file_uri, strlen (file_uri), NULL, &error) < 0 ||
      g_socket_send (socket, "\n", 1, NULL, &error) < 0 ||
      g_socket_receive (socket, response, 3, NULL, &error) < 0)
    return 1;
  return g_strcmp0 (response, "OK\n") == 0 ? 0 : 1;
}
