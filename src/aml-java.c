/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* AmlJava: finding and understanding the Java runtimes on this
 * machine. Candidates are asked, one after the other and never on the
 * main loop thread, what they are; the results are what the UI lists. */

/* realpath() is glibc-only, and AML only ever runs on Linux. */
#define _GNU_SOURCE

#include "aml-java.h"
#include "aml-task.h"
#include "aml-task.h"

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
static gboolean
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
  AmlJava *best = NULL;          /* the closest one that can open a window */
  AmlJava *best_headless = NULL; /* only used when nothing else fits */

  g_return_val_if_fail (javas != NULL, NULL);

  for (guint i = 0; i < javas->len; i++)
    {
      AmlJava *java = g_ptr_array_index (javas, i);

      if (!aml_java_is_compatible (java, required_major))
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
                 int         required_major)
{
  g_return_val_if_fail (javas != NULL, NULL);

  if (instance_java_path != NULL)
    {
      for (guint i = 0; i < javas->len; i++)
        {
          AmlJava *java = g_ptr_array_index (javas, i);

          if (java_matches (java, instance_java_path))
            {
              /* An explicit path is preferred, but it must still satisfy
               * the Java version named by Minecraft.  Otherwise an old
               * per-instance setting could make a newer game fail after
               * launch with an opaque JVM error. */
               return java->major_version == required_major ? java : NULL;
            }
        }

      /* The instance points at a Java detection cannot see; it is the
       * instance's decision either way. */
      return NULL;
    }

  /* Automatic selection must not silently choose a different major. A
   * newer JVM may be technically compatible, but it is not the runtime the
   * user asked for and can behave differently with loaders and launchers.
   * Leave the automatic choice unset until the required major is present. */
  for (guint i = 0; i < javas->len; i++)
    {
      AmlJava *java = g_ptr_array_index (javas, i);

      if (java->major_version == required_major && !java->headless)
        return java;
    }

  return NULL;
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
  /* Borrowed from the operation the caller owns until the answer. */
  AmlTaskScope *scope;
  char *executable;
  GSubprocess *process;
} Probe;

static void
probe_destroyed (gpointer data)
{
  Probe *probe = data;

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
      aml_task_scope_return_error (probe->scope, g_steal_pointer (&error));
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
      aml_task_scope_return_error (
        probe->scope,
        g_error_new (AML_JAVA_ERROR, AML_JAVA_ERROR_PARSE,
                     "\"%s\" did not report a version", probe->executable));
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
  java->headless = !java_has_xawt (java->executable);

  aml_task_scope_return_pointer (probe->scope, java,
                                 (GDestroyNotify) aml_java_free);
  probe_destroyed (probe);
}

AmlTaskScope *
aml_java_probe_async (const char         *executable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  g_autoptr (GError) error = NULL;
  const char * const argv[] = { executable, "-XshowSettings:properties", "-version",
                   NULL };
  Probe *probe = NULL;

  aml_task_scope_set_check_cancellable (scope, FALSE);

  probe = g_new0 (Probe, 1);

  /* Borrowed: the caller's handle keeps the operation alive, and the
     probe is answered before it goes away. */
  probe->scope = scope;
  probe->executable = g_strdup (executable);

  probe->process = g_subprocess_newv (argv,
                                      G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                      &error);

  if (probe->process == NULL)
    {
      aml_task_scope_return_error_deferred (probe->scope,
                                            g_steal_pointer (&error));
      probe_destroyed (probe);
      return g_steal_pointer (&scope);
    }

  g_subprocess_communicate_utf8_async (probe->process, NULL,
                                       aml_task_scope_get_cancellable (scope),
                                       probe_communicate, probe);

  return g_steal_pointer (&scope);
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
  char **executables;
  guint next;
  GPtrArray *javas;   /* AmlJava*, element free aml_java_free */
  GHashTable *seen;   /* resolved real paths, for dedup */
  AmlTaskSequence *sequence;
} ProbeList;

static void
probe_list_destroyed (gpointer data)
{
  ProbeList *list = data;

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

static void probe_list_probed (GObject *source, GAsyncResult *result,
                               gpointer user_data);

static void
probe_list_sequence_step (AmlTaskSequence *sequence G_GNUC_UNUSED,
                          gpointer user_data)
{
  ProbeList *list = user_data;

  if (list->executables[list->next] == NULL)
    {
      aml_task_sequence_return_pointer (
        list->sequence, g_steal_pointer (&list->javas),
        (GDestroyNotify) g_ptr_array_unref);
      return;
    }

  {
    AmlTaskScope *probe =
      aml_java_probe_async (list->executables[list->next], probe_list_probed,
                            list);

    /* One probe of many: it follows the sequence's cancellation and
       releases itself once it has answered. */
    aml_task_scope_follow_cancellable (
      probe, aml_task_sequence_get_cancellable (list->sequence));
    aml_task_scope_unref_when_done (probe);
  }

  list->next++;
}

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

  aml_task_sequence_next (list->sequence);
}

AmlTaskScope *
aml_java_probe_list_async (char            **executables,
                           GAsyncReadyCallback callback,
                           gpointer         user_data)
{
  ProbeList *list = NULL;
  AmlTaskScope *scope = NULL;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();

  g_return_val_if_fail (executables != NULL, NULL);

  list = g_new0 (ProbeList, 1);
  list->executables = executables; /* the caller gives up the array */
  list->javas = g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_java_free);
  list->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  list->sequence = aml_task_sequence_new (
    cancellable, callback, user_data, probe_list_sequence_step, list,
    probe_list_destroyed);

  /* The handle the caller gets back stands for the whole run. The
     sequence keeps the task itself. */
  scope = aml_task_sequence_scope (list->sequence);

  aml_task_sequence_start (list->sequence);
  aml_task_sequence_unref (list->sequence);
  return scope;
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
               const char *path);

/* Search-gate: @home/bin/java and @home/jre/bin/java. */
static void
add_jvm_home (GPtrArray  *candidates,
              const char *home)
{
  g_autofree char *bin_java = g_build_filename (home, "bin", "java", NULL);
  g_autofree char *jre_java =
    g_build_filename (home, "jre", "bin", "java", NULL);

  add_candidate (candidates, bin_java);
  add_candidate (candidates, jre_java);
}

static void
add_root_scandir (GPtrArray  *candidates,
                  const char *root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDir) dir = g_dir_open (root, 0, &error);

  if (dir == NULL)
    return;

  for (const char *name = g_dir_read_name (dir); name != NULL;
       name = g_dir_read_name (dir))
    {
      g_autofree char *home = g_build_filename (root, name, NULL);

      add_jvm_home (candidates, home);
    }
}

/* Built-in subtrees whose subdirectories are treated as JVM homes. */
static char **
default_roots (void)
{
  const char *home = g_get_home_dir ();
  static const char *system[] = {
    "/usr/lib/jvm",
    "/usr/lib64/jvm",
    "/usr/java",
    "/usr/local/java",
    "/opt",
    NULL
  };
  g_autofree char *jdks = g_build_filename (home, ".jdks", NULL);
  g_autofree char *sdkman = g_build_filename (home, ".sdkman",
                                              "candidates", "java", NULL);
  g_autofree char *asdf = g_build_filename (home, ".asdf", "installs",
                                            "java", NULL);
  g_autofree char *mise = g_build_filename (home, ".local", "share", "mise",
                                            "installs", "java", NULL);
  char **out = g_new0 (char *, G_N_ELEMENTS (system) + 5);
  guint n = 0;

  for (guint i = 0; system[i] != NULL; i++)
    out[n++] = g_strdup (system[i]);

  out[n++] = g_steal_pointer (&jdks);
  out[n++] = g_steal_pointer (&sdkman);
  out[n++] = g_steal_pointer (&asdf);
  out[n++] = g_steal_pointer (&mise);
  out[n++] = NULL;

  return out;
}

/* Only executable regular files are candidates: directories, devices
 * and scripts that are not executable cannot be probed. */
static void
add_candidate (GPtrArray  *candidates,
               const char *path)
{
  if (path == NULL || *path == '\0')
    return;

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR) &&
      g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
    g_ptr_array_add (candidates, g_strdup (path));
}

/* The system the Fedora, Arch etc. layout plus $JAVA_HOME and PATH. */
static char **
collect_candidates (void)
{
  g_autoptr (GPtrArray) candidates =
    g_ptr_array_new_with_free_func (g_free);
  const char *java_home = g_getenv ("JAVA_HOME");
  char **roots = default_roots ();

  for (guint i = 0; roots[i] != NULL; i++)
    add_root_scandir (candidates, roots[i]);

  g_strfreev (roots);

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

/* For aml_java_detect_list: only the caller's roots are scanned. */
static char **
collect_candidates_list (char **roots)
{
  g_autoptr (GPtrArray) candidates =
    g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; roots != NULL && roots[i] != NULL; i++)
    add_root_scandir (candidates, roots[i]);

  g_ptr_array_add (candidates, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&candidates), FALSE);
}

/* The same real installation can be reached through several spellings
 * (/usr/lib/jvm/default vs /usr/lib/jvm/java-25-openjdk): probe the
 * first path for each, no second one. */
static char **
dedupe_candidates (char **candidates)
{
  g_autoptr (GHashTable) seen =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GPtrArray) out =
    g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; candidates != NULL && candidates[i] != NULL; i++)
    {
      g_autofree char *real = realpath (candidates[i], NULL);
      const char *key = real != NULL ? real : candidates[i];

      if (!g_hash_table_contains (seen, key))
        {
          g_hash_table_insert (seen, g_strdup (key), GINT_TO_POINTER (1));
          /* Probe the canonical path when there is one: the result
           * always points at the real install, not a symlink. */
          g_ptr_array_add (out, g_strdup (key));
        }
    }

  g_ptr_array_add (out, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&out), FALSE);
}

typedef struct
{
  AmlTaskScope *scope;
} Detect;

static void
detected (GObject      *source G_GNUC_UNUSED,
          GAsyncResult *result,
          gpointer      user_data)
{
  Detect *detect = user_data;
  g_autoptr (GError) error = NULL;
  GPtrArray *javas = aml_java_probe_list_finish (result, &error);

  /* The reference this detection held is handed back to the operation,
     which releases it once the answer is in. The answer is also deferred:
     an empty machine answers before the call that asked is over. */
  aml_task_scope_unref_when_done (detect->scope);

  if (javas == NULL)
    aml_task_scope_return_error_deferred (detect->scope,
                                          g_steal_pointer (&error));
  else
    aml_task_scope_return_pointer_deferred (detect->scope, javas,
                                            (GDestroyNotify) g_ptr_array_unref);

  g_free (detect);
}

AmlTaskScope *
aml_java_detect_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  g_auto (GStrv) collected = collect_candidates ();
  g_auto (GStrv) candidates = dedupe_candidates (collected);
  Detect *detect = NULL;

  aml_task_scope_set_check_cancellable (scope, FALSE);

  detect = g_new0 (Detect, 1);
  detect->scope = aml_task_scope_ref (scope);

  {
    AmlTaskScope *listing =
      aml_java_probe_list_async (g_steal_pointer (&candidates), detected,
                                 detect);

    aml_task_scope_follow_cancellable (listing,
                                       aml_task_scope_get_cancellable (scope));
    aml_task_scope_unref_when_done (listing);
  }

  return g_steal_pointer (&scope);
}

AmlTaskScope *
aml_java_detect_list_async (char            **roots,
                            GAsyncReadyCallback callback,
                            gpointer         user_data)
{
  g_autoptr (AmlTaskScope) scope =
    aml_task_scope_new (NULL, NULL, callback, user_data);
  g_auto (GStrv) collected = collect_candidates_list (roots);
  g_auto (GStrv) candidates = dedupe_candidates (collected);
  Detect *detect = NULL;

  aml_task_scope_set_check_cancellable (scope, FALSE);

  detect = g_new0 (Detect, 1);
  detect->scope = aml_task_scope_ref (scope);

  {
    AmlTaskScope *listing =
      aml_java_probe_list_async (g_steal_pointer (&candidates), detected,
                                 detect);

    aml_task_scope_follow_cancellable (listing,
                                       aml_task_scope_get_cancellable (scope));
    aml_task_scope_unref_when_done (listing);
  }

  return g_steal_pointer (&scope);
}

GPtrArray *
aml_java_detect_finish (GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
