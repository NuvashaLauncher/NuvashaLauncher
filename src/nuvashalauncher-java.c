/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Detect and inspect Java runtimes without blocking the main loop. */

/* realpath() is provided by glibc; nuvashalauncher targets Linux. */
#define _GNU_SOURCE

#include "nuvashalauncher-java.h"
#include "nuvashalauncher-java-private.h"
#include "nuvashalauncher-task.h"

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* TRUE when <base>/lib holds AWT's X11 bridge, either straight in lib/
 * (OpenJDK 9 and newer) or under an architecture directory such as
 * lib/amd64/ (OpenJDK 8). A headless package carries
 * libawt_headless.so instead, so neither is there. */
static gboolean
home_has_xawt (const char *base)
{
  g_autofree char *lib = g_build_filename (base, "lib", NULL);
  g_autofree char *direct = g_build_filename (lib, "libawt_xawt.so", NULL);
  g_autoptr (GDir) dir = NULL;
  const char *name = NULL;

  if (g_file_test (direct, G_FILE_TEST_EXISTS))
    return TRUE;

  dir = g_dir_open (lib, 0, NULL);

  if (dir == NULL)
    return FALSE;

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *sub =
        g_build_filename (lib, name, "libawt_xawt.so", NULL);

      if (g_file_test (sub, G_FILE_TEST_EXISTS))
        return TRUE;
    }

  return FALSE;
}

/* True when this JVM can open a window: its home, or the JRE a JDK keeps
 * under jre/, holds AWT's X11 bridge. The executable is resolved first,
 * because a /usr/bin/java or a jvm/default link points somewhere else. */
gboolean
java_has_xawt (const char *executable)
{
  g_autofree char *real = realpath (executable, NULL);
  g_autofree char *dir = NULL;
  g_autofree char *home = NULL;
  g_autofree char *jre = NULL;

  if (real == NULL)
    return FALSE;

  /* <home>/bin/java -> <home>; a JDK's own JRE is <home>/jre. */
  dir = g_path_get_dirname (real);
  home = g_path_get_dirname (dir);
  jre = g_build_filename (home, "jre", NULL);

  return home_has_xawt (jre) || home_has_xawt (home);
}

GQuark
nuvashalauncher_java_error_quark (void)
{
  return g_quark_from_static_string ("nuvashalauncher-java-error");
}

void
nuvashalauncher_java_free (NuvashaLauncherJava *java)
{
  if (java == NULL)
    return;

  g_free (java->executable);
  g_free (java->arch);
  g_free (java->vendor);
  g_free (java);
}

gboolean
nuvashalauncher_java_is_compatible (const NuvashaLauncherJava *java, int required_major)
{
  g_return_val_if_fail (java != NULL, FALSE);

  /* The instance asked for the javaVersion.majorVersion in its version
   * document; a newer JVM is fine, an older one is not. */
  return java->major_version >= required_major;
}

/* The real file a path names, resolving symlinks. realpath(3) is used
 * so that the same JVM reached through two spellings counts once. */
char *
real_path (const char *path)
{
  char *resolved = realpath (path, NULL);

  if (resolved == NULL)
    return g_strdup (path);

  return resolved;
}

NuvashaLauncherJava *
nuvashalauncher_java_best_compatible (GPtrArray *javas, int required_major)
{
  NuvashaLauncherJava *best = NULL;          /* the closest one that can open a window */
  NuvashaLauncherJava *best_headless = NULL; /* fallback when no windowed runtime fits */

  g_return_val_if_fail (javas != NULL, NULL);

  for (guint i = 0; i < javas->len; i++)
    {
      NuvashaLauncherJava *java = g_ptr_array_index (javas, i);

      if (!nuvashalauncher_java_is_compatible (java, required_major))
        continue;

      /* A headless runtime cannot show the game, so one that can is
         worth a larger major version: the game wants Java N or newer,
         and a newer windowed Java runs it. */
      if (java->headless)
        {
          if (best_headless == NULL ||
              java->major_version < best_headless->major_version)
            best_headless = java;
        }
      else if (best == NULL || java->major_version < best->major_version)
        best = java;
    }

  return best != NULL ? best : best_headless;
}

/* True when @path_real resolves to the same file as the NuvashaLauncherJava this
 * entry matches. */
static gboolean
java_matches_real (NuvashaLauncherJava *java,
                   const char          *raw_path,
                   const char          *path_real)
{
  if (g_strcmp0 (java->executable, raw_path) == 0 ||
      g_strcmp0 (java->executable, path_real) == 0)
    return TRUE;

  {
    g_autofree char *java_real = real_path (java->executable);

    return g_strcmp0 (java_real, path_real) == 0;
  }
}

const NuvashaLauncherJava *
nuvashalauncher_java_select (GPtrArray  *javas,
                 const char *instance_java_path,
                 int         required_major)
{
  g_return_val_if_fail (javas != NULL, NULL);

  if (instance_java_path != NULL)
    {
      g_autofree char *path_real = real_path (instance_java_path);

      for (guint i = 0; i < javas->len; i++)
        {
          NuvashaLauncherJava *java = g_ptr_array_index (javas, i);

          if (java_matches_real (java, instance_java_path, path_real))
            {
               /* An explicit path is preferred, but it must still satisfy
                * the Java version named by Minecraft. A zero requirement
                * means the version document did not declare one. */
                return required_major == 0 ||
                       java->major_version == required_major ? java : NULL;
            }
        }

      /* The instance points at a Java detection cannot see; it is the
       * instance's decision either way. */
      return NULL;
    }

  /* Older version documents do not declare a Java version. Pick the
   * closest usable runtime. */
  if (required_major == 0)
    return nuvashalauncher_java_best_compatible (javas, 0);

  /* Automatic selection must not choose a different major. A
   * newer JVM may be technically compatible, but it is not the runtime the
   * required major version and can behave differently with loaders and launchers.
   * Leave the automatic choice unset until the required major is present. */
  for (guint i = 0; i < javas->len; i++)
    {
      NuvashaLauncherJava *java = g_ptr_array_index (javas, i);

      if (java->major_version == required_major && !java->headless)
        return java;
    }

  return NULL;
}

char *
nuvashalauncher_java_get_dir (void)
{
  char *dir = g_build_filename (g_get_user_data_dir (), "nuvashalauncher", "java", NULL);
  g_mkdir_with_parents (dir, 0755);
  return dir;
}

gboolean
nuvashalauncher_java_should_copy (const char  *executable,
                                  char       **out_source_home,
                                  char       **out_rel_exec)
{
  g_autofree char *real_exec = NULL;
  const char *target = NULL;
  g_autofree char *launcher_dir = NULL;
  g_autofree char *real_launcher_dir = NULL;
  const char *launcher_target = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *dir_base = NULL;

  if (out_source_home != NULL)
    *out_source_home = NULL;
  if (out_rel_exec != NULL)
    *out_rel_exec = NULL;

  if (executable == NULL || *executable == '\0')
    return FALSE;

  real_exec = realpath (executable, NULL);
  target = real_exec != NULL ? real_exec : executable;

  launcher_dir = nuvashalauncher_java_get_dir ();
  real_launcher_dir = realpath (launcher_dir, NULL);
  launcher_target = real_launcher_dir != NULL ? real_launcher_dir : launcher_dir;

  /* If already inside the launcher Java directory, do not copy. */
  if (launcher_target != NULL && g_str_has_prefix (target, launcher_target))
    return FALSE;

  /* System directories must not be copied. */
  if (g_str_has_prefix (target, "/usr/") ||
      g_str_has_prefix (target, "/nix/store") ||
      g_str_has_prefix (target, "/app/") ||
      g_str_has_prefix (target, "/bin/") ||
      g_str_has_prefix (target, "/lib/") ||
      g_str_has_prefix (target, "/lib64/"))
    return FALSE;

  dir = g_path_get_dirname (target);
  dir_base = g_path_get_basename (dir);

  if (g_strcmp0 (dir_base, "bin") == 0)
    {
      g_autofree char *parent = g_path_get_dirname (dir);
      g_autofree char *parent_base = g_path_get_basename (parent);

      if (g_strcmp0 (parent_base, "jre") == 0)
        {
          if (out_source_home != NULL)
            *out_source_home = g_path_get_dirname (parent);
          if (out_rel_exec != NULL)
            *out_rel_exec = g_build_filename ("jre", "bin", g_path_get_basename (target), NULL);
        }
      else
        {
          if (out_source_home != NULL)
            *out_source_home = g_steal_pointer (&parent);
          if (out_rel_exec != NULL)
            *out_rel_exec = g_build_filename ("bin", g_path_get_basename (target), NULL);
        }
    }
  else
    {
      if (out_source_home != NULL)
        *out_source_home = g_steal_pointer (&dir);
      if (out_rel_exec != NULL)
        *out_rel_exec = g_path_get_basename (target);
    }

  return TRUE;
}
