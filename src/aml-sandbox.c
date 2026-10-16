/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "aml-sandbox.h"

#include "aml-fs.h"
#include "aml-sandbox-seccomp.h"

#include <glib/gstdio.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char *source;
  char *target;
} PathMap;

static void
path_map_free (PathMap *map)
{
  if (map == NULL)
    return;
  g_free (map->source);
  g_free (map->target);
  g_free (map);
}

GQuark
aml_sandbox_error_quark (void)
{
  return g_quark_from_static_string ("aml-sandbox-error");
}

static void
add (GPtrArray *arguments, const char *argument)
{
  g_ptr_array_add (arguments, g_strdup (argument));
}

static void
add_pair (GPtrArray *arguments, const char *option,
          const char *source, const char *target)
{
  add (arguments, option);
  add (arguments, source);
  add (arguments, target);
}

static void
add_one (GPtrArray *arguments, const char *option, const char *value)
{
  add (arguments, option);
  add (arguments, value);
}

static gboolean
trusted_bwrap_path (const char *path)
{
  g_autofree char *current = NULL;
  struct stat info;

  if (path == NULL || !g_path_is_absolute (path))
    return FALSE;

  current = g_canonicalize_filename (path, NULL);
  if (g_lstat (current, &info) != 0 || !S_ISREG (info.st_mode) ||
      info.st_uid != 0 || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
    return FALSE;

  while (g_strcmp0 (current, "/") != 0)
    {
      g_autofree char *parent = g_path_get_dirname (current);

      if (g_lstat (parent, &info) != 0 || !S_ISDIR (info.st_mode) ||
          info.st_uid != 0 || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return FALSE;
      g_free (g_steal_pointer (&current));
      current = g_steal_pointer (&parent);
    }

  return TRUE;
}

static char *
find_bwrap (GError **error)
{
  g_autofree char *found = g_find_program_in_path ("bwrap");

  if (!trusted_bwrap_path (found))
    {
      if (found != NULL)
        g_set_error (error, AML_SANDBOX_ERROR,
                     AML_SANDBOX_ERROR_UNTRUSTED,
                     "the Bubblewrap executable is not trusted");
      return NULL;
    }
  return g_steal_pointer (&found);
}

static gboolean
directory_is_usable (const char *path, GError **error)
{
  if (path == NULL || !g_path_is_absolute (path) ||
      !aml_fs_directory_is_real (path))
    {
      g_set_error (error, AML_SANDBOX_ERROR,
                   AML_SANDBOX_ERROR_INVALID_PATH,
                   "the sandbox path is not a real absolute directory: %s",
                   path != NULL ? path : "(null)");
      return FALSE;
    }
  return TRUE;
}

static char *
canonical_directory (const char *path, GError **error)
{
  g_autofree char *canonical = NULL;

  if (!directory_is_usable (path, error))
    return NULL;

  canonical = g_canonicalize_filename (path, NULL);
  if (!directory_is_usable (canonical, error))
    return NULL;
  return g_steal_pointer (&canonical);
}

static void
add_map (GPtrArray *maps, const char *source, const char *target,
         GError **error)
{
  g_autofree char *canonical = canonical_directory (source, error);
  PathMap *map;

  if (canonical == NULL)
    return;

  for (guint i = 0; i < maps->len; i++)
    {
      PathMap *existing = g_ptr_array_index (maps, i);
      if (g_strcmp0 (existing->source, canonical) == 0)
        return;
    }

  map = g_new0 (PathMap, 1);
  map->source = g_steal_pointer (&canonical);
  map->target = g_strdup (target);
  g_ptr_array_add (maps, map);
}

static void
add_virtual_map (GPtrArray *maps, const char *source, const char *target)
{
  PathMap *map = g_new0 (PathMap, 1);

  map->source = g_canonicalize_filename (source, NULL);
  map->target = g_strdup (target);
  g_ptr_array_add (maps, map);
}

static char *
java_home_for (const char *java_executable, GError **error)
{
  g_autofree char *executable = NULL;
  g_autofree char *bin = NULL;
  g_autofree char *home = NULL;

  if (java_executable == NULL || !g_path_is_absolute (java_executable))
    {
      g_set_error (error, AML_SANDBOX_ERROR,
                   AML_SANDBOX_ERROR_INVALID_PATH,
                   "the configured Java executable is not absolute");
      return NULL;
    }

  executable = g_canonicalize_filename (java_executable, NULL);
  bin = g_path_get_dirname (executable);
  home = g_path_get_dirname (bin);

  /* System directories are mounted as part of the runtime below. Mapping a
   * user-owned Java tree separately keeps the host home directory hidden. */
  if (g_str_has_prefix (executable, "/usr/") ||
      g_str_has_prefix (executable, "/bin/") ||
      g_str_has_prefix (executable, "/lib/") ||
      g_str_has_prefix (executable, "/lib64/"))
    return NULL;

  return g_steal_pointer (&home);
}

static char *
replace_paths (const char *argument, GPtrArray *maps,
               const char *natives_source)
{
  GString *result = g_string_new (NULL);
  const char *cursor = argument;
  const char *value_start = strchr (argument, '=');
  gboolean writable_native_property =
    g_str_has_prefix (argument, "-Djna.tmpdir=") ||
    g_str_has_prefix (argument, "-Dorg.lwjgl.system.SharedLibraryExtractPath=") ||
    g_str_has_prefix (argument, "-Dio.netty.native.workdir=");

  while (*cursor != '\0')
    {
      gboolean replaced = FALSE;
      PathMap *best = NULL;
      gsize best_length = 0;

      for (guint i = 0; i < maps->len; i++)
        {
          PathMap *map = g_ptr_array_index (maps, i);
          gsize length = strlen (map->source);
          gboolean boundary = cursor == argument || cursor[-1] == '=' ||
                              cursor[-1] == ':' || cursor[-1] == ',';

          if (boundary && g_str_has_prefix (cursor, map->source) &&
              (cursor[length] == '\0' || cursor[length] == '/' ||
               cursor[length] == ':') && length > best_length)
            {
              best = map;
              best_length = length;
            }
        }

      if (writable_native_property &&
          (best == NULL || g_strcmp0 (best->source, natives_source) == 0) &&
          value_start != NULL && cursor == value_start + 1 &&
          g_str_has_prefix (cursor, natives_source))
        {
          gsize length = strlen (natives_source);

          if (cursor[length] == '\0' || cursor[length] == '/' ||
              cursor[length] == ':')
            {
              g_string_append (result, "/instance/.aml-runtime/natives");
              cursor += length;
              replaced = TRUE;
            }
        }

      if (best != NULL)
        {
          g_string_append (result, best->target);
          cursor += best_length;
          replaced = TRUE;
        }

      if (!replaced)
        g_string_append_c (result, *cursor++);
    }

  return g_string_free (result, FALSE);
}

static void
add_environment (GPtrArray *arguments)
{
  const char *value;
  const char *names[] = { "LANG", "LANGUAGE", "LC_ALL", "LC_COLLATE",
                          "LC_CTYPE", "LC_MESSAGES", "LC_MONETARY",
                          "LC_NUMERIC", "LC_TIME", NULL };

  add (arguments, "--setenv");
  add (arguments, "HOME");
  add (arguments, "/instance");
  add (arguments, "--setenv");
  add (arguments, "PATH");
  add (arguments, "/instance/.aml-runtime:/aml/bin:/usr/bin:/bin");

  for (guint i = 0; names[i] != NULL; i++)
    {
      value = g_getenv (names[i]);
      if (value != NULL)
        {
          add (arguments, "--setenv");
          add (arguments, names[i]);
          add (arguments, value);
        }
    }
}

static gboolean
add_display (GPtrArray    *arguments,
             gboolean      force_x11,
             GError      **error)
{
  const char *runtime = g_get_user_runtime_dir ();
  const char *wayland = g_getenv ("WAYLAND_DISPLAY");
  const char *display = g_getenv ("DISPLAY");

  /* LWJGL 2 (Minecraft versions before 1.13) has no Wayland support.
   * Keep that legacy path on XWayland; modern clients use the native
   * Wayland branch below whenever the compositor provides it. */
  if (force_x11 && wayland != NULL && display != NULL &&
      g_file_test ("/tmp/.X11-unix", G_FILE_TEST_IS_DIR))
    {
      const char *xauthority = g_getenv ("XAUTHORITY");
      g_autofree char *uid = g_strdup_printf ("%u", (guint) getuid ());
      g_autofree char *runtime_target = g_build_filename ("/run/user", uid,
                                                          NULL);

      add_one (arguments, "--dir", "/run");
      add_one (arguments, "--dir", "/run/user");
      add_one (arguments, "--dir", runtime_target);
      add_pair (arguments, "--ro-bind", "/tmp/.X11-unix", "/tmp/.X11-unix");
      add (arguments, "--setenv");
      add (arguments, "DISPLAY");
      add (arguments, display);
      add (arguments, "--setenv");
      add (arguments, "XDG_SESSION_TYPE");
      add (arguments, "x11");
      if (xauthority != NULL &&
          g_file_test (xauthority, G_FILE_TEST_IS_REGULAR))
        {
          add_pair (arguments, "--ro-bind", xauthority, "/xauth");
          add (arguments, "--setenv");
          add (arguments, "XAUTHORITY");
          add (arguments, "/xauth");
        }

      {
        const char *runtime = g_get_user_runtime_dir ();
        g_autofree char *pipewire = NULL;
        g_autofree char *pipewire_target = NULL;
        struct stat socket_info;

        if (runtime != NULL)
          {
            pipewire = g_build_filename (runtime, "pipewire-0", NULL);
            pipewire_target = g_build_filename (runtime_target, "pipewire-0",
                                                NULL);
            if (g_stat (pipewire, &socket_info) == 0 &&
                S_ISSOCK (socket_info.st_mode))
              add_pair (arguments, "--ro-bind", pipewire, pipewire_target);
          }
      }

      {
        const char *runtime = g_get_user_runtime_dir ();
        g_autofree char *pulse_dir = NULL;
        g_autofree char *pulse = NULL;
        g_autofree char *pulse_target_dir = NULL;
        g_autofree char *pulse_target = NULL;
        struct stat socket_info;

        if (runtime != NULL)
          {
            pulse_dir = g_build_filename (runtime, "pulse", NULL);
            pulse = g_build_filename (pulse_dir, "native", NULL);
            pulse_target_dir = g_build_filename (runtime_target, "pulse",
                                                 NULL);
            pulse_target = g_build_filename (pulse_target_dir, "native", NULL);
            if (g_stat (pulse, &socket_info) == 0 &&
                S_ISSOCK (socket_info.st_mode))
              {
                add_one (arguments, "--dir", pulse_target_dir);
                add_pair (arguments, "--ro-bind", pulse, pulse_target);
                add (arguments, "--setenv");
                add (arguments, "PULSE_RUNTIME_PATH");
                add (arguments, pulse_target_dir);
              }
          }
      }

      add (arguments, "--setenv");
      add (arguments, "XDG_RUNTIME_DIR");
      add (arguments, runtime_target);
      return TRUE;
    }

  if (!force_x11 && wayland != NULL && runtime != NULL && *wayland != '\0')
    {
      g_autofree char *socket = g_build_filename (runtime, wayland, NULL);
      g_autofree char *uid = g_strdup_printf ("%u", (guint) getuid ());
      g_autofree char *runtime_target = g_build_filename ("/run/user", uid,
                                                          NULL);
      g_autofree char *target = g_build_filename (runtime_target, wayland,
                                                  NULL);

      if (strchr (wayland, '/') != NULL || g_str_equal (wayland, ".") ||
          g_str_equal (wayland, ".."))
        {
          g_set_error (error, AML_SANDBOX_ERROR,
                       AML_SANDBOX_ERROR_INVALID_DISPLAY,
                       "the Wayland display name is invalid");
          return FALSE;
        }

      {
        struct stat socket_info;

        if (g_stat (socket, &socket_info) != 0 ||
            !S_ISSOCK (socket_info.st_mode))
          {
            g_set_error (error, AML_SANDBOX_ERROR,
                         AML_SANDBOX_ERROR_INVALID_DISPLAY,
                         "the Wayland socket is not available: %s", socket);
            return FALSE;
          }
      }

      add_one (arguments, "--dir", "/run");
      add_one (arguments, "--dir", "/run/user");
      add_one (arguments, "--dir", runtime_target);
      add_pair (arguments, "--ro-bind", socket, target);

      {
        g_autofree char *pipewire = g_build_filename (runtime, "pipewire-0",
                                                       NULL);
        g_autofree char *pipewire_target =
          g_build_filename (runtime_target, "pipewire-0", NULL);
        struct stat socket_info;

        if (g_stat (pipewire, &socket_info) == 0 &&
            S_ISSOCK (socket_info.st_mode))
          add_pair (arguments, "--ro-bind", pipewire, pipewire_target);
      }

      {
        g_autofree char *pulse_dir = g_build_filename (runtime, "pulse", NULL);
        g_autofree char *pulse = g_build_filename (pulse_dir, "native", NULL);
        g_autofree char *pulse_target_dir = g_build_filename (runtime_target,
                                                               "pulse", NULL);
        g_autofree char *pulse_target = g_build_filename (pulse_target_dir,
                                                          "native", NULL);
        struct stat socket_info;

        if (g_stat (pulse, &socket_info) == 0 && S_ISSOCK (socket_info.st_mode))
          {
            add_one (arguments, "--dir", pulse_target_dir);
            add_pair (arguments, "--ro-bind", pulse, pulse_target);
            add (arguments, "--setenv");
            add (arguments, "PULSE_RUNTIME_PATH");
            add (arguments, pulse_target_dir);
          }
      }

      add (arguments, "--setenv");
      add (arguments, "XDG_RUNTIME_DIR");
      add (arguments, runtime_target);
      add (arguments, "--setenv");
      add (arguments, "WAYLAND_DISPLAY");
      add (arguments, wayland);
      add (arguments, "--setenv");
      add (arguments, "XDG_SESSION_TYPE");
      add (arguments, "wayland");
      return TRUE;
    }

  /* X11 is deliberately a compatibility path: unlike Wayland, the X11
   * server permits clients to inspect and interact with other X clients. */
  if (g_getenv ("DISPLAY") != NULL &&
      g_file_test ("/tmp/.X11-unix", G_FILE_TEST_IS_DIR))
    {
      const char *xauthority = g_getenv ("XAUTHORITY");

      add_pair (arguments, "--ro-bind", "/tmp/.X11-unix", "/tmp/.X11-unix");
      add (arguments, "--setenv");
      add (arguments, "DISPLAY");
      add (arguments, g_getenv ("DISPLAY"));
      if (xauthority != NULL &&
          g_file_test (xauthority, G_FILE_TEST_IS_REGULAR))
        {
          add_pair (arguments, "--ro-bind", xauthority, "/xauth");
          add (arguments, "--setenv");
          add (arguments, "XAUTHORITY");
          add (arguments, "/xauth");
        }
      return TRUE;
    }

  return TRUE;
}

GPtrArray *
aml_sandbox_build_arguments (GPtrArray              *java_argv,
                             const char              *game_directory,
                             const AmlResolvedVersion *resolved,
                             const char              *java_executable,
                             gboolean                 allow_network,
                             int                     *seccomp_fd,
                             int                      open_fd,
                             int                     *broker_fd,
                             int                     *broker_gio_fd,
                             GError                 **error)
{
  g_autoptr (GPtrArray) maps = g_ptr_array_new_with_free_func (
    (GDestroyNotify) path_map_free);
  g_autoptr (GPtrArray) arguments = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *java_home = NULL;
  g_autofree char *data_dir = NULL;
  g_autofree char *logging = NULL;
  g_autofree char *client_directory = NULL;
  g_autofree char *game_source = NULL;
  g_autofree char *library_source = NULL;
  g_autofree char *assets_source = NULL;
  g_autofree char *natives_source = NULL;
  g_autofree char *client_parent = NULL;
  g_autofree char *runtime_skins = NULL;
  g_autofree char *xrandr_link = NULL;
  g_autofree char *assets_objects = NULL;
  g_autofree char *assets_indexes = NULL;
  g_autofree char *assets_virtual = NULL;
  g_autofree char *rewritten = NULL;
  g_autofree char *bwrap = NULL;
  g_autofree char *open_broker = NULL;
  g_autofree char *xrandr_helper = NULL;
  g_autofree char *self_exe = NULL;
  gboolean native_tree = FALSE;
  gboolean legacy_x11 = FALSE;
  gboolean system_java;

  g_return_val_if_fail (java_argv != NULL && java_argv->len > 0, NULL);
  g_return_val_if_fail (resolved != NULL, NULL);
  g_return_val_if_fail (seccomp_fd != NULL, NULL);
  g_return_val_if_fail (broker_fd != NULL, NULL);
  g_return_val_if_fail (broker_gio_fd != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  *seccomp_fd = -1;
  *broker_fd = -1;
  *broker_gio_fd = -1;

  bwrap = find_bwrap (error);
  if (bwrap == NULL)
    {
      if (error != NULL && *error != NULL)
        return NULL;
      g_set_error (error, AML_SANDBOX_ERROR,
                   AML_SANDBOX_ERROR_UNAVAILABLE,
                   "Bubblewrap (bwrap) is not installed");
      return NULL;
    }

  add (arguments, bwrap);
  add (arguments, "--die-with-parent");
  add (arguments, "--new-session");
  add (arguments, "--unshare-user");
  add (arguments, "--unshare-pid");
  add (arguments, "--unshare-ipc");
  add (arguments, "--unshare-uts");
  add (arguments, "--unshare-cgroup");
  add (arguments, "--cap-drop");
  add (arguments, "ALL");
  add (arguments, "--seccomp");
  add (arguments, "3");
  add (arguments, "--clearenv");
  if (!allow_network)
    add (arguments, "--unshare-net");

  add (arguments, "--tmpfs");
  add (arguments, "/");
  add (arguments, "--proc");
  add (arguments, "/proc");
  add (arguments, "--dev");
  add (arguments, "/dev");
  add (arguments, "--tmpfs");
  add (arguments, "/tmp");

  add_one (arguments, "--dir", "/aml");
  add_one (arguments, "--dir", "/aml/bin");
  add_one (arguments, "--dir", "/aml/lib");

  self_exe = g_file_read_link ("/proc/self/exe", error);
  if (self_exe == NULL)
    return NULL;
  xrandr_helper = g_build_filename (g_path_get_dirname (self_exe),
                                    "aml-xrandr", NULL);
  if (g_file_test (xrandr_helper, G_FILE_TEST_IS_REGULAR))
    add_pair (arguments, "--ro-bind", xrandr_helper, "/aml/bin/xrandr");

  add_pair (arguments, "--ro-bind", "/usr", "/usr");
  if (g_file_test ("/bin", G_FILE_TEST_IS_DIR))
    add_pair (arguments, "--ro-bind", "/bin", "/bin");
  add_pair (arguments, "--ro-bind", "/lib", "/lib");
  if (g_file_test ("/lib64", G_FILE_TEST_IS_DIR))
    add_pair (arguments, "--ro-bind", "/lib64", "/lib64");
  if (g_file_test ("/sbin", G_FILE_TEST_IS_DIR))
    add_pair (arguments, "--ro-bind", "/sbin", "/sbin");
  add_pair (arguments, "--ro-bind", "/etc", "/etc");
  if (open_fd >= 0)
    {
      open_broker = g_build_filename (g_path_get_dirname (self_exe),
                                      "aml-open-broker.so", NULL);
      if (!g_file_test (open_broker, G_FILE_TEST_IS_REGULAR))
        {
          g_set_error (error, AML_SANDBOX_ERROR,
                       AML_SANDBOX_ERROR_INVALID_PATH,
                       "cannot locate the sandbox open helper");
          return NULL;
        }
      add_pair (arguments, "--ro-bind", open_broker,
                "/aml/lib/aml-open-broker.so");
      *broker_fd = open ("/proc/self/exe", O_RDONLY | O_CLOEXEC);
      if (*broker_fd < 0)
        {
          g_set_error (error, AML_SANDBOX_ERROR,
                       AML_SANDBOX_ERROR_INVALID_PATH,
                       "cannot locate AML for the sandbox open broker");
          return NULL;
        }
      *broker_gio_fd = open ("/proc/self/exe", O_RDONLY | O_CLOEXEC);
      if (*broker_gio_fd < 0)
        {
          close (*broker_fd);
          *broker_fd = -1;
          g_set_error (error, AML_SANDBOX_ERROR,
                       AML_SANDBOX_ERROR_INVALID_PATH,
                       "cannot locate AML for the gio open broker");
          return NULL;
        }
      add (arguments, "--ro-bind-fd");
      add (arguments, "5");
      add (arguments, "/aml/bin/xdg-open");
      add (arguments, "--ro-bind-fd");
      add (arguments, "6");
      add (arguments, "/aml/bin/gio");
    }
  if (g_file_test ("/dev/dri", G_FILE_TEST_IS_DIR))
    add_pair (arguments, "--dev-bind", "/dev/dri", "/dev/dri");
  if (g_file_test ("/sys", G_FILE_TEST_IS_DIR))
    add_pair (arguments, "--ro-bind", "/sys", "/sys");

  game_source = canonical_directory (game_directory, error);
  library_source = canonical_directory (resolved->library_directory, error);
  assets_source = canonical_directory (resolved->assets_directory, error);
  natives_source = g_canonicalize_filename (resolved->natives_directory, NULL);
  native_tree = aml_fs_directory_is_real (natives_source);
  client_parent = g_path_get_dirname (resolved->client_jar);
  client_directory = canonical_directory (client_parent, error);
  if (game_source == NULL || library_source == NULL ||
      assets_source == NULL || natives_source == NULL ||
      client_directory == NULL)
    return NULL;

  if (!aml_fs_make_directory_tree_nofollow (game_source,
                                            ".aml-runtime/assets/skins",
                                            0700, error))
    return NULL;
  runtime_skins = g_build_filename (game_source, ".aml-runtime", "assets",
                                    "skins", NULL);
  xrandr_link = g_build_filename (game_source, ".aml-runtime", "xrandr",
                                  NULL);
  {
    g_autofree char *runtime = g_path_get_dirname (xrandr_link);

    if (g_mkdir_with_parents (runtime, 0700) != 0)
      {
        g_set_error (error, AML_SANDBOX_ERROR,
                     AML_SANDBOX_ERROR_INVALID_PATH,
                     "cannot create the sandbox runtime directory");
        return NULL;
      }
  }
  if (!g_file_test (xrandr_link, G_FILE_TEST_EXISTS))
    {
      if (symlink ("/aml/bin/xrandr", xrandr_link) != 0)
        {
          g_set_error (error, AML_SANDBOX_ERROR,
                       AML_SANDBOX_ERROR_INVALID_PATH,
                       "cannot create the sandbox xrandr link");
          return NULL;
        }
    }
  assets_objects = canonical_directory (
    g_build_filename (assets_source, "objects", NULL), error);
  assets_indexes = canonical_directory (
    g_build_filename (assets_source, "indexes", NULL), error);
  if (assets_objects == NULL || assets_indexes == NULL)
    return NULL;
  {
    g_autofree char *virtual = g_build_filename (assets_source, "virtual",
                                                 NULL);
    if (g_file_test (virtual, G_FILE_TEST_IS_DIR))
      assets_virtual = canonical_directory (virtual, error);
    if (error != NULL && *error != NULL)
      return NULL;
  }

  add_map (maps, game_source, "/instance", error);
  add_map (maps, library_source, "/aml/libraries", error);
  add_map (maps, assets_source, "/instance/.aml-runtime/assets", error);
  if (native_tree)
    add_map (maps, natives_source, "/aml/natives", error);
  {
    g_autofree char *skins = g_build_filename (assets_source, "skins", NULL);
    g_autofree char *jna = g_build_filename (natives_source, "jna", NULL);
    g_autofree char *lwjgl = g_build_filename (natives_source, "lwjgl", NULL);
    g_autofree char *netty = g_build_filename (natives_source, "netty", NULL);

    /* These are extraction/work directories, not the native libraries
     * themselves. Keep the shared native tree read-only and give each
     * instance its own writable cache below the instance mount. */
    add_virtual_map (maps, skins, "/instance/.aml-runtime/assets/skins");
    g_autofree char *java = g_build_filename (natives_source, "java", NULL);

    add_virtual_map (maps, java, "/instance/.aml-runtime/natives/java");
    add_virtual_map (maps, jna, "/instance/.aml-runtime/jna");
    add_virtual_map (maps, lwjgl, "/instance/.aml-runtime/lwjgl");
    add_virtual_map (maps, netty, "/instance/.aml-runtime/netty");
  }
  add_map (maps, client_directory, "/aml/version", error);

  data_dir = g_path_get_dirname (library_source);
  if (resolved->logging_argument != NULL)
    {
      logging = g_build_filename (data_dir, "logging", NULL);
      add_map (maps, logging, "/aml/logging", error);
    }

  java_home = java_home_for (java_executable, error);
  system_java = java_home == NULL && error != NULL && *error == NULL;
  if (error != NULL && *error != NULL)
    return NULL;
  if (!system_java)
    add_map (maps, java_home, "/java", error);

  if (error != NULL && *error != NULL)
    return NULL;

  add_pair (arguments, "--bind", game_source, "/instance");
  add_pair (arguments, "--ro-bind", library_source, "/aml/libraries");
  add_one (arguments, "--dir", "/instance/.aml-runtime/assets");
  add_pair (arguments, "--bind", runtime_skins,
            "/instance/.aml-runtime/assets/skins");
  add_pair (arguments, "--ro-bind", assets_objects,
            "/instance/.aml-runtime/assets/objects");
  add_pair (arguments, "--ro-bind", assets_indexes,
            "/instance/.aml-runtime/assets/indexes");
  if (assets_virtual != NULL)
    add_pair (arguments, "--ro-bind", assets_virtual,
              "/instance/.aml-runtime/assets/virtual");
  if (native_tree)
    add_pair (arguments, "--ro-bind-try", natives_source, "/aml/natives");
  add_pair (arguments, "--ro-bind", client_directory, "/aml/version");
  if (logging != NULL)
    add_pair (arguments, "--ro-bind", logging, "/aml/logging");
  if (!system_java)
    add_pair (arguments, "--ro-bind", java_home, "/java");

  {
    int minor = -1;

    if (resolved->version_id != NULL &&
        sscanf (resolved->version_id, "1.%d", &minor) == 1 && minor < 13)
      legacy_x11 = TRUE;
    if (!add_display (arguments, legacy_x11, error))
      return NULL;
  }
  if (error != NULL && *error != NULL)
    return NULL;
  add_environment (arguments);
  if (!legacy_x11 && g_getenv ("WAYLAND_DISPLAY") != NULL)
    {
      add (arguments, "--setenv");
      add (arguments, "XDG_SESSION_TYPE");
      add (arguments, "wayland");
    }
  if (open_fd >= 0)
    {
      add (arguments, "--setenv");
      add (arguments, "LD_PRELOAD");
      add (arguments, "/aml/lib/aml-open-broker.so");
    }
  add (arguments, "--chdir");
  add (arguments, "/instance");
  add (arguments, "--");

  *seccomp_fd = aml_sandbox_seccomp_open (error);
  if (*seccomp_fd < 0)
    return NULL;

  for (guint i = 0; i < java_argv->len; i++)
    {
      rewritten = replace_paths (g_ptr_array_index (java_argv, i), maps,
                                 natives_source);
      g_ptr_array_add (arguments, g_steal_pointer (&rewritten));
    }

  return g_steal_pointer (&arguments);
}
