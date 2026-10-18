/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _XOPEN_SOURCE 700

#include "leppoisa-open-broker.h"
#include "leppoisa-fs.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
  GSocketService *service;
  char *socket_path;
  char *allowed_root;
  GArray *open_fds;
} Broker;

static GPtrArray *brokers;
static guint next_broker_id;

static void
broker_free (Broker *broker)
{
  if (broker == NULL)
    return;

  if (broker->open_fds != NULL)
    for (guint i = 0; i < broker->open_fds->len; i++)
      close (g_array_index (broker->open_fds, int, i));
  if (broker->socket_path != NULL)
    unlink (broker->socket_path);
  g_clear_pointer (&broker->open_fds, g_array_unref);
  g_clear_object (&broker->service);
  g_clear_pointer (&broker->socket_path, g_free);
  g_clear_pointer (&broker->allowed_root, g_free);
  g_free (broker);
}

static void
cleanup_brokers (void)
{
  g_clear_pointer (&brokers, g_ptr_array_unref);
}

static int
open_checked_path (Broker *broker, const char *path)
{
  char root[PATH_MAX], real_path[PATH_MAX];
  g_autofree char *parent = NULL;
  g_autofree char *basename = NULL;
  int parent_fd;
  int fd;

  if (realpath (broker->allowed_root, root) == NULL ||
      realpath (path, real_path) == NULL)
    return -1;

  if (!g_str_has_prefix (real_path, root) ||
      (real_path[strlen (root)] != '\0' && real_path[strlen (root)] != '/'))
    return -1;

  parent = g_path_get_dirname (real_path);
  basename = g_path_get_basename (real_path);
  parent_fd = leppoisa_fs_open_directory_nofollow (parent, NULL);
  if (parent_fd < 0)
    return -1;

  /* Open every parent component without following symlinks, then pin the
   * final object without following a final symlink. This uses the existing
   * POSIX filesystem helpers rather than Linux's openat2 syscall. */
  fd = openat (parent_fd, basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
               (g_file_test (real_path, G_FILE_TEST_IS_DIR) ? O_DIRECTORY : 0));
  close (parent_fd);
  return fd;
}

/* Read one complete request. A stream read is allowed to return only part of
 * a write, so a single read cannot be used as a message boundary. */
static char *
read_request (GInputStream *input, GError **error)
{
  GByteArray *bytes = g_byte_array_new ();
  guint8 chunk[256];

  for (;;)
    {
      gssize count = g_input_stream_read (input, chunk, sizeof chunk, NULL,
                                          error);
      if (count <= 0)
        {
          g_byte_array_unref (bytes);
          return NULL;
        }

      guint8 *newline = memchr (chunk, '\n', (gsize) count);
      gsize amount = newline != NULL ? (gsize) (newline - chunk) : (gsize) count;
      if (bytes->len + amount > PATH_MAX + 32)
        {
          g_byte_array_unref (bytes);
          return NULL;
        }
      g_byte_array_append (bytes, chunk, amount);
      if (newline != NULL)
        {
          g_byte_array_append (bytes, (guint8[]) { 0 }, 1);
          return (char *) g_byte_array_free (bytes, FALSE);
        }
    }
}

/* Keep the descriptor alive after launching: the desktop application may
 * open the URI asynchronously. The small cap prevents a game from turning
 * the broker into an unbounded file-descriptor sink. */
static gboolean
launch_checked_path (Broker *broker, const char *path, GError **error)
{
  int fd;
  g_autofree char *proc_path = NULL;
  g_autofree char *uri = NULL;

  fd = open_checked_path (broker, path);
  if (fd < 0)
    return FALSE;

  /* The descriptor pins the object that was validated, avoiding a
   * symlink/path race.
   * The descriptor is retained because launching the desktop application is
   * not necessarily synchronous. */
  proc_path = g_strdup_printf ("/proc/%ld/fd/%d", (long) getpid (), fd);
  uri = g_filename_to_uri (proc_path, NULL, error);
  if (uri == NULL || !g_app_info_launch_default_for_uri (uri, NULL, error))
    {
      close (fd);
      return FALSE;
    }

  if (broker->open_fds->len >= 256)
    {
      close (g_array_index (broker->open_fds, int, 0));
      g_array_remove_index (broker->open_fds, 0);
    }
  g_array_append_val (broker->open_fds, fd);
  return TRUE;
}

static gboolean
broker_incoming (GSocketService *source G_GNUC_UNUSED,
                 GSocketConnection *connection,
                 GObject *object G_GNUC_UNUSED,
                 gpointer user_data)
{
  Broker *broker = user_data;
  GInputStream *input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  GOutputStream *output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  g_autoptr (GError) error = NULL;
  g_autofree char *request = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;

  request = read_request (input, &error);
  if (request == NULL || error != NULL)
    return TRUE;
  request[strcspn (request, "\r\n")] = '\0';

  if (!g_str_has_prefix (request, "file://"))
    return TRUE;

  file = g_file_new_for_uri (request);
  path = g_file_get_path (file);
  if (path == NULL)
    return TRUE;

  if (!launch_checked_path (broker, path, &error))
    g_debug ("open broker rejected %s: %s", request,
             error != NULL ? error->message : "path is not allowed");
  else
    g_output_stream_write (output, "OK\n", 3, NULL, NULL);

  return TRUE;
}

const char *
leppoisa_open_broker_start (const char *root, GError **error)
{
  Broker *broker;
  g_autoptr (GSocketAddress) address = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *name = NULL;

  g_return_val_if_fail (root != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  if (brokers == NULL)
    {
      brokers = g_ptr_array_new_with_free_func ((GDestroyNotify) broker_free);
      atexit (cleanup_brokers);
    }

  runtime = g_strdup (g_get_user_runtime_dir ());
  name = g_strdup_printf ("leppoisa-open-%u-%u.sock", (guint) getpid (),
                          ++next_broker_id);
  broker = g_new0 (Broker, 1);
  broker->socket_path = g_build_filename (runtime, name, NULL);
  broker->allowed_root = realpath (root, NULL);
  broker->open_fds = g_array_new (FALSE, FALSE, sizeof (int));
  if (broker->allowed_root == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "cannot resolve sandbox instance directory");
      broker_free (broker);
      return NULL;
    }

  broker->service = g_socket_service_new ();
  address = g_unix_socket_address_new (broker->socket_path);
  if (!g_socket_listener_add_address (G_SOCKET_LISTENER (broker->service), address,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
                                      error))
    {
      broker_free (broker);
      return NULL;
    }
  chmod (broker->socket_path, 0600);
  g_signal_connect (broker->service, "incoming", G_CALLBACK (broker_incoming),
                    broker);
  g_socket_service_start (broker->service);
  g_ptr_array_add (brokers, broker);
  return broker->socket_path;
}

int
leppoisa_open_broker_request (const char *path, const char *uri)
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
