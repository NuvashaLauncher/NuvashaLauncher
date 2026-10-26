/* Java probing and installation discovery. */
#define _GNU_SOURCE

#include "nuvashalauncher-java.h"
#include "nuvashalauncher-java-private.h"
#include "nuvashalauncher-task.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
  /* Borrowed from the operation the caller owns until the answer. */
  nuvashalauncherTaskScope *scope;
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
      nuvashalauncher_task_scope_return_error (probe->scope, g_steal_pointer (&error));
      probe_destroyed (probe);
      return;
    }

  int major = 0;
  char *arch = NULL;
  char *vendor_out = NULL;

  if (!g_subprocess_get_if_exited (G_SUBPROCESS (source)) ||
      g_subprocess_get_exit_status (G_SUBPROCESS (source)) != 0 ||
      !nuvashalauncher_java_parse_version (output, -1, &major, &arch, &vendor_out))
    {
      nuvashalauncher_task_scope_return_error (
        probe->scope,
        g_error_new (nuvashalauncher_JAVA_ERROR, nuvashalauncher_JAVA_ERROR_PARSE,
                     "\"%s\" did not report a version", probe->executable));
      g_free (arch);
      g_free (vendor_out);
      probe_destroyed (probe);
      return;
    }

  nuvashalauncherJava *java = g_new0 (nuvashalauncherJava, 1);

  java->executable = g_steal_pointer (&probe->executable);
  java->major_version = major;
  java->arch = g_steal_pointer (&arch);
  java->vendor = g_steal_pointer (&vendor_out);
  java->headless = !java_has_xawt (java->executable);

  nuvashalauncher_task_scope_return_pointer (probe->scope, java,
                                 (GDestroyNotify) nuvashalauncher_java_free);
  probe_destroyed (probe);
}

nuvashalauncherTaskScope *
nuvashalauncher_java_probe_async (const char         *executable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autoptr (GError) error = NULL;
  const char * const argv[] = { executable, "-XshowSettings:properties", "-version",
                   NULL };
  Probe *probe = NULL;

  nuvashalauncher_task_scope_set_check_cancellable (scope, FALSE);

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
      nuvashalauncher_task_scope_return_error_deferred (probe->scope,
                                            g_steal_pointer (&error));
      probe_destroyed (probe);
      return g_steal_pointer (&scope);
    }

  g_subprocess_communicate_utf8_async (probe->process, NULL,
                                       nuvashalauncher_task_scope_get_cancellable (scope),
                                       probe_communicate, probe);

  return g_steal_pointer (&scope);
}

nuvashalauncherJava *
nuvashalauncher_java_probe_finish (GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Dedup + sequential probing over a list of candidates
 * ---------------------------------------------------------- */

typedef struct
{
  char **executables;
  guint next;
  GPtrArray *javas;   /* nuvashalauncherJava*, element free nuvashalauncher_java_free */
  GHashTable *seen;   /* resolved real paths, for dedup */
  nuvashalauncherTaskSequence *sequence;
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

static nuvashalauncherJava *
nuvashalauncher_java_add_unique (GPtrArray  *javas,
                     GHashTable *seen,
                     nuvashalauncherJava    *java)
{
  g_autofree char *resolved = real_path (java->executable);

  if (g_hash_table_contains (seen, resolved))
    {
      nuvashalauncher_java_free (java);
      return NULL;
    }

  g_hash_table_insert (seen, g_steal_pointer (&resolved), GINT_TO_POINTER (1));
  g_ptr_array_add (javas, java);

  return java;
}

static void probe_list_probed (GObject *source, GAsyncResult *result,
                               gpointer user_data);

static void
probe_list_sequence_step (nuvashalauncherTaskSequence *sequence G_GNUC_UNUSED,
                          gpointer user_data)
{
  ProbeList *list = user_data;

  if (list->executables[list->next] == NULL)
    {
      nuvashalauncher_task_sequence_return_pointer (
        list->sequence, g_steal_pointer (&list->javas),
        (GDestroyNotify) g_ptr_array_unref);
      return;
    }

  {
    nuvashalauncherTaskScope *probe =
      nuvashalauncher_java_probe_async (list->executables[list->next], probe_list_probed,
                            list);

    nuvashalauncher_task_scope_follow_cancellable (
      probe, nuvashalauncher_task_sequence_get_cancellable (list->sequence));
    nuvashalauncher_task_scope_unref_when_done (probe);
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
  nuvashalauncherJava *java = nuvashalauncher_java_probe_finish (result, &error);

  if (java != NULL)
    {
      nuvashalauncher_java_add_unique (list->javas, list->seen, java);

      /* The finished probe stays alive while its result points into it. */
    }

  nuvashalauncher_task_sequence_next (list->sequence);
}

nuvashalauncherTaskScope *
nuvashalauncher_java_probe_list_async (char            **executables,
                           GAsyncReadyCallback callback,
                           gpointer         user_data)
{
  ProbeList *list = NULL;
  nuvashalauncherTaskScope *scope = NULL;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();

  g_return_val_if_fail (executables != NULL, NULL);

  list = g_new0 (ProbeList, 1);
  list->executables = executables; /* the caller gives up the array */
  list->javas = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_java_free);
  list->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  list->sequence = nuvashalauncher_task_sequence_new (
    cancellable, callback, user_data, probe_list_sequence_step, list,
    probe_list_destroyed);

  /* The handle the caller gets back stands for the whole run. The
     sequence keeps the task itself. */
  scope = nuvashalauncher_task_sequence_scope (list->sequence);

  nuvashalauncher_task_sequence_start (list->sequence);
  nuvashalauncher_task_sequence_unref (list->sequence);
  return scope;
}

GPtrArray *
nuvashalauncher_java_probe_list_finish (GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

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

void
nuvashalauncher_java_register_custom_path (const char *executable)
{
  g_autoptr (GSettings) settings = NULL;
  g_auto (GStrv) stored = NULL;
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);

  if (executable == NULL || *executable == '\0')
    return;

  settings = g_settings_new ("com.github.nuvashalauncher.nuvashalauncher");
  stored = g_settings_get_strv (settings, "custom-java-paths");
  for (guint i = 0; stored != NULL && stored[i] != NULL; i++)
    {
      if (g_strcmp0 (stored[i], executable) == 0)
        return;
      g_ptr_array_add (paths, g_strdup (stored[i]));
    }

  g_ptr_array_add (paths, g_strdup (executable));
  g_ptr_array_add (paths, NULL);
  g_settings_set_strv (settings, "custom-java-paths",
                       (const char * const *) paths->pdata);
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

  {
    g_autoptr (GSettings) settings = g_settings_new (
      "com.github.nuvashalauncher.nuvashalauncher");
    g_auto (GStrv) custom = g_settings_get_strv (settings, "custom-java-paths");

    for (guint i = 0; custom != NULL && custom[i] != NULL; i++)
      add_candidate (candidates, custom[i]);
  }

  g_autofree char *on_path = g_find_program_in_path ("java");
  add_candidate (candidates, on_path);

  g_ptr_array_add (candidates, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&candidates), FALSE);
}

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
  nuvashalauncherTaskScope *scope;
} Detect;

static void
detected (GObject      *source G_GNUC_UNUSED,
          GAsyncResult *result,
          gpointer      user_data)
{
  Detect *detect = user_data;
  g_autoptr (GError) error = NULL;
  GPtrArray *javas = nuvashalauncher_java_probe_list_finish (result, &error);

  /* The reference this detection held is handed back to the operation,
     which releases it once the answer is in. The answer is also deferred:
     an empty machine answers before the call that asked is over. */
  nuvashalauncher_task_scope_unref_when_done (detect->scope);

  if (javas == NULL)
    nuvashalauncher_task_scope_return_error_deferred (detect->scope,
                                          g_steal_pointer (&error));
  else
    nuvashalauncher_task_scope_return_pointer_deferred (detect->scope, javas,
                                            (GDestroyNotify) g_ptr_array_unref);

  g_free (detect);
}

nuvashalauncherTaskScope *
nuvashalauncher_java_detect_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_auto (GStrv) collected = collect_candidates ();
  g_auto (GStrv) candidates = dedupe_candidates (collected);
  Detect *detect = NULL;

  nuvashalauncher_task_scope_set_check_cancellable (scope, FALSE);

  detect = g_new0 (Detect, 1);
  detect->scope = nuvashalauncher_task_scope_ref (scope);

  {
    nuvashalauncherTaskScope *listing =
      nuvashalauncher_java_probe_list_async (g_steal_pointer (&candidates), detected,
                                 detect);

    nuvashalauncher_task_scope_follow_cancellable (listing,
                                       nuvashalauncher_task_scope_get_cancellable (scope));
    nuvashalauncher_task_scope_unref_when_done (listing);
  }

  return g_steal_pointer (&scope);
}

nuvashalauncherTaskScope *
nuvashalauncher_java_detect_list_async (char            **roots,
                            GAsyncReadyCallback callback,
                            gpointer         user_data)
{
  g_autoptr (nuvashalauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_auto (GStrv) collected = collect_candidates_list (roots);
  g_auto (GStrv) candidates = dedupe_candidates (collected);
  Detect *detect = NULL;

  nuvashalauncher_task_scope_set_check_cancellable (scope, FALSE);

  detect = g_new0 (Detect, 1);
  detect->scope = nuvashalauncher_task_scope_ref (scope);

  {
    nuvashalauncherTaskScope *listing =
      nuvashalauncher_java_probe_list_async (g_steal_pointer (&candidates), detected,
                                 detect);

    nuvashalauncher_task_scope_follow_cancellable (listing,
                                       nuvashalauncher_task_scope_get_cancellable (scope));
    nuvashalauncher_task_scope_unref_when_done (listing);
  }

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvashalauncher_java_detect_finish (GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
