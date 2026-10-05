/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlJava: finding and understanding the Java runtimes on this
 * machine. Candidates are asked, one after the other and never on the
 * main loop thread, what they are; the results are what the UI lists. */

/* realpath() is glibc-only, and AML only ever runs on Linux. */
#define _GNU_SOURCE

#include "aml-java.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Where Linux keeps JDK bundles. */
static const char *jvm_roots[] = {
  "/usr/lib/jvm",
  "/usr/lib64/jvm",
  NULL
};

GQuark
aml_java_error_quark (void)
{
  return g_quark_from_static_string ("aml-java-error");
}

void
aml_java_free (AmlJava *java)
{
  if (java == NULL)
    return;

  g_free (java->executable);
  g_free (java->arch);
  g_free (java->vendor);
  g_free (java);
}

gboolean
aml_java_is_compatible (const AmlJava *java, int required_major)
{
  g_return_val_if_fail (java != NULL, FALSE);

  /* The instance asked for the javaVersion.majorVersion in its version
   * document; a newer JVM is fine, an older one is not. */
  return java->major_version >= required_major;
}

/* The real file a path names, resolving symlinks. realpath(3) is used
 * so that the same JVM reached through two spellings counts once. */
static char *
real_path (const char *path)
{
  char *resolved = realpath (path, NULL);

  if (resolved == NULL)
    return g_strdup (path);

  return resolved;
}

AmlJava *
aml_java_best_compatible (GPtrArray *javas, int required_major)
{
  AmlJava *best = NULL;

  g_return_val_if_fail (javas != NULL, NULL);

  for (guint i = 0; i < javas->len; i++)
    {
      AmlJava *java = g_ptr_array_index (javas, i);

      if (!aml_java_is_compatible (java, required_major))
        continue;

      if (best == NULL || java->major_version < best->major_version)
        best = java;
    }

  return best;
}

/* True when @path resolves to the same file as the AmlJava this
 * entry matches, comparing resolved real paths. */
static gboolean
java_matches (AmlJava   *java,
              const char *path)
{
  g_autofree char *java_real = real_path (java->executable);
  g_autofree char *path_real = real_path (path);

  return g_strcmp0 (java_real, path_real) == 0;
}

const AmlJava *
aml_java_select (GPtrArray  *javas,
                 const char *instance_java_path,
                 const char *global_java_path,
                 int         required_major)
{
  g_return_val_if_fail (javas != NULL, NULL);

  if (instance_java_path != NULL)
    {
      for (guint i = 0; i < javas->len; i++)
        {
          AmlJava *java = g_ptr_array_index (javas, i);

          if (java_matches (java, instance_java_path))
            return java;
        }

      /* The instance points at a Java detection cannot see; it is the
       * instance's decision either way. */
      return NULL;
    }

  if (global_java_path != NULL)
    {
      for (guint i = 0; i < javas->len; i++)
        {
          AmlJava *java = g_ptr_array_index (javas, i);

          if (java_matches (java, global_java_path))
            return java;
        }

      return NULL;
    }

  return aml_java_best_compatible (javas, required_major);
}

/* ---------------------------------------------------------- *
 * Parsing `java -version`
 * ---------------------------------------------------------- */

/* The major number out of the banner's version string: "1.8.0_x" is 8,
 * everything else starts with the major itself. Returns FALSE when the
 * string has no usable number. */
static gboolean
parse_major (const char *version, int *major)
{
  g_auto (GStrv) parts = g_strsplit (version, ".", 0);

  /* An empty or all-dots version string names no Java version. */
  if (parts == NULL || parts[0] == NULL || *parts[0] == '\0')
    return FALSE;

  if (g_strcmp0 (parts[0], "1") == 0 && parts[1] != NULL)
    return sscanf (parts[1], "%d", major) == 1;

  return sscanf (parts[0], "%d", major) == 1;
}

gboolean
aml_java_parse_version (const char  *output,
                        gssize       length,
                        int         *major_version,
                        char       **architecture,
                        char       **vendor)
{
  g_auto (GStrv) lines =
    g_strsplit (output != NULL ? output : "", "\n", 0);
  int major = 0;
  char *arch = NULL;
  char *arch_fallback = NULL;
  char *vendor_out = NULL;

  g_return_val_if_fail (output != NULL, FALSE);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *banner;
      char *start;
      char *end;

      /* The banner around `version "x.y.z"`. */
      if (major == 0 && (banner = g_strstr_len (lines[i], length, "version \"")) != NULL)
        {
          start = banner + strlen ("version \"");
          end = strchr (start, '"');

          if (end != NULL)
            {
              g_autofree char *version = g_strndup (start, end - start);

              parse_major (version, &major);
            }
        }

      if (arch == NULL && g_strstr_len (lines[i], length, "os.arch =") != NULL)
        {
          char *equals = strchr (lines[i], '=');

          arch = g_strdup (g_strstrip (equals + 1));
        }
      else if (arch_fallback == NULL &&
               g_strstr_len (lines[i], length, "64-Bit") != NULL)
        arch_fallback = g_strdup ("64-bit");
      else if (arch_fallback == NULL &&
               g_strstr_len (lines[i], length, "32-Bit") != NULL)
        arch_fallback = g_strdup ("32-bit");
    }

  /* The explicit os.arch wins; the 64-Bit/32-Bit banner is the
   * fallback for reports where the JVM did not print its properties. */
  if (arch == NULL)
    arch = arch_fallback;
  else
    g_free (arch_fallback);

  /* The vendor sits in front of the runtime name on the
   * "Runtime Environment" line: "OpenJDK Runtime Environment (build
   * ...)". The property dump from -XshowSettings shows
   * java.runtime.name = OpenJDK Runtime Environment  too, but that
   * line does not belong to the exit report, so skip it. */
  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *cut = g_strstr_len (lines[i], length, "Runtime Environment");

      if (cut != NULL && strchr (lines[i], '=') == NULL)
        {
          g_autofree char *prefix = g_strndup (lines[i], cut - lines[i]);

          vendor_out = g_strdup (g_strstrip (prefix));

          if (vendor_out[0] == '\0')
            g_clear_pointer (&vendor_out, g_free);

          break;
        }
    }

  if (major == 0)
    {
      g_free (arch);
      g_free (vendor_out);
      return FALSE;
    }

  if (major_version != NULL)
    *major_version = major;

  if (architecture != NULL)
    *architecture = g_steal_pointer (&arch);
  else
    g_free (arch);

  if (vendor != NULL)
    *vendor = g_steal_pointer (&vendor_out);
  else
    g_free (vendor_out);

  return TRUE;
}

/* ---------------------------------------------------------- *
 * Probing one executable
 * ---------------------------------------------------------- */

typedef struct
{
  GTask *task;
  char *executable;
  GSubprocess *process;
} Probe;

static void
probe_destroyed (gpointer data)
{
  Probe *probe = data;

  g_clear_object (&probe->task);
  g_clear_pointer (&probe->executable, g_free);
  g_clear_object (&probe->process);
  g_free (probe);
}

static void
probe_communicate (GObject      *source G_GNUC_UNUSED,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  Probe *probe = user_data;
  g_autofree char *output = NULL;
  g_autoptr (GError) error = NULL;

  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), result,
                                             &output, NULL, &error))
    {
      g_task_return_error (probe->task, g_steal_pointer (&error));
      probe_destroyed (probe);
      return;
    }

  int major = 0;
  char *arch = NULL;
  char *vendor_out = NULL;

  if (!g_subprocess_get_if_exited (G_SUBPROCESS (source)) ||
      g_subprocess_get_exit_status (G_SUBPROCESS (source)) != 0 ||
      !aml_java_parse_version (output, -1, &major, &arch, &vendor_out))
    {
      g_task_return_new_error (probe->task, AML_JAVA_ERROR,
                               AML_JAVA_ERROR_PARSE,
                               "\"%s\" did not report a version",
                               probe->executable);
      g_free (arch);
      g_free (vendor_out);
      probe_destroyed (probe);
      return;
    }

  AmlJava *java = g_new0 (AmlJava, 1);

  java->executable = g_steal_pointer (&probe->executable);
  java->major_version = major;
  java->arch = g_steal_pointer (&arch);
  java->vendor = g_steal_pointer (&vendor_out);

  g_task_return_pointer (probe->task, java, (GDestroyNotify) aml_java_free);
  probe_destroyed (probe);
}

void
aml_java_probe_async (const char         *executable,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_autoptr (GError) error = NULL;
  const char * const argv[] = { executable, "-XshowSettings:properties", "-version",
                   NULL };
  Probe *probe = NULL;

  g_task_set_check_cancellable (task, FALSE);

  probe = g_new0 (Probe, 1);
  probe->task = g_steal_pointer (&task);
  probe->executable = g_strdup (executable);

  probe->process = g_subprocess_newv (argv,
                                      G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                      &error);

  if (probe->process == NULL)
    {
      g_task_return_error (probe->task, g_steal_pointer (&error));
      probe_destroyed (probe);
      return;
    }

  g_subprocess_communicate_utf8_async (probe->process, NULL,
                                      cancellable, probe_communicate, probe);
}

AmlJava *
aml_java_probe_finish (GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Dedup + sequential probing over a list of candidates
 * ---------------------------------------------------------- */

typedef struct
{
  GTask *task;
  char **executables;
  guint next;
  GPtrArray *javas;   /* AmlJava*, element free aml_java_free */
  GHashTable *seen;   /* resolved real paths, for dedup */
} ProbeList;

static void
probe_list_destroyed (gpointer data)
{
  ProbeList *list = data;

  g_clear_object (&list->task);
  g_strfreev (list->executables);
  g_clear_pointer (&list->javas, g_ptr_array_unref);
  g_clear_pointer (&list->seen, g_hash_table_unref);
  g_free (list);
}

static AmlJava *
aml_java_add_unique (GPtrArray  *javas,
                     GHashTable *seen,
                     AmlJava    *java)
{
  g_autofree char *resolved = real_path (java->executable);

  if (g_hash_table_contains (seen, resolved))
    {
      aml_java_free (java);
      return NULL;
    }

  g_hash_table_insert (seen, g_steal_pointer (&resolved), GINT_TO_POINTER (1));
  g_ptr_array_add (javas, java);

  return java;
}

static void
probe_list_step (ProbeList *list);

static void
probe_list_probed (GObject      *source G_GNUC_UNUSED,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  ProbeList *list = user_data;
  g_autoptr (GError) error = NULL;
  AmlJava *java = aml_java_probe_finish (result, &error);

  if (java != NULL)
    {
      aml_java_add_unique (list->javas, list->seen, java);

      /* The finished probe stays alive while its result points into it. */
    }

  GCancellable *cancellable = g_task_get_cancellable (list->task);

  if (cancellable != NULL && g_cancellable_is_cancelled (cancellable))
    {
      /* A cancelled probe list says nothing: there is no partial list. */
      g_task_return_new_error (list->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "the scan was cancelled");
      probe_list_destroyed (list);
      return;
    }

  probe_list_step (list);
}

static void
probe_list_step (ProbeList *list)
{
  /* The executables array is taken over as-is; a NULL entry ends the
   * walk. */
  if (list->executables[list->next] == NULL)
    {
      g_task_return_pointer (list->task, g_steal_pointer (&list->javas),
                             (GDestroyNotify) g_ptr_array_unref);
      probe_list_destroyed (list);
      return;
    }

  aml_java_probe_async (list->executables[list->next],
                        g_task_get_cancellable (list->task),
                        probe_list_probed, list);
  list->next++;
}

void
aml_java_probe_list_async (char            **executables,
                           GCancellable    *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer         user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  ProbeList *list = NULL;

  g_task_set_check_cancellable (task, FALSE);

  list = g_new0 (ProbeList, 1);
  list->task = g_steal_pointer (&task);
  list->executables = executables; /* the caller gives up the array */
  list->javas = g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_java_free);
  list->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  probe_list_step (list);
}

GPtrArray *
aml_java_probe_list_finish (GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Detecting the installations on this machine
 * ---------------------------------------------------------- */

static void
add_candidate (GPtrArray  *candidates,
               const char *path)
{
  if (path == NULL || *path == '\0')
    return;

  if (g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
    g_ptr_array_add (candidates, g_strdup (path));
}

static char **
collect_candidates (void)
{
  g_autoptr (GPtrArray) candidates =
    g_ptr_array_new_with_free_func (g_free);
  const char *java_home = g_getenv ("JAVA_HOME");

  for (guint i = 0; jvm_roots[i] != NULL; i++)
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (GDir) dir = g_dir_open (jvm_roots[i], 0, &error);

      if (dir == NULL)
        continue;

      for (const char *name = g_dir_read_name (dir); name != NULL;
           name = g_dir_read_name (dir))
        {
          g_autofree char *candidate =
            g_build_filename (jvm_roots[i], name, "bin", "java", NULL);

          add_candidate (candidates, candidate);
        }
    }

  if (java_home != NULL && *java_home != '\0')
    {
      g_autofree char *candidate =
        g_build_filename (java_home, "bin", "java", NULL);

      add_candidate (candidates, candidate);
    }

  g_autofree char *on_path = g_find_program_in_path ("java");
  add_candidate (candidates, on_path);

  g_ptr_array_add (candidates, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&candidates), FALSE);
}

typedef struct
{
  GTask *detect_task;
} Detect;

static void
detected (GObject      *source G_GNUC_UNUSED,
          GAsyncResult *result,
          gpointer      user_data)
{
  Detect *detect = user_data;
  g_autoptr (GError) error = NULL;
  GPtrArray *javas = aml_java_probe_list_finish (result, &error);

  if (javas == NULL)
    g_task_return_error (detect->detect_task, g_steal_pointer (&error));
  else
    g_task_return_pointer (detect->detect_task, javas,
                           (GDestroyNotify) g_ptr_array_unref);

  g_clear_object (&detect->detect_task);
  g_free (detect);
}

void
aml_java_detect_async (GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_autofree char **candidates = NULL;
  Detect *detect = NULL;

  g_task_set_check_cancellable (task, FALSE);

  candidates = collect_candidates ();
  detect = g_new0 (Detect, 1);
  detect->detect_task = g_steal_pointer (&task);

  aml_java_probe_list_async (g_steal_pointer (&candidates), cancellable,
                             detected, detect);
}

GPtrArray *
aml_java_detect_finish (GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
