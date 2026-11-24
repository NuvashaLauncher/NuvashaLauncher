/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Java probing and installation discovery. */
#define _GNU_SOURCE

#include "nuvashalauncher-java.h"
#include "nuvashalauncher-java-private.h"
#include "nuvashalauncher-settings.h"
#include "nuvashalauncher-task.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
  NuvashaLauncherJava *java;
  dev_t dev;
  ino_t ino;
  off_t size;
  time_t mtime_sec;
  long mtime_nsec;
} ProbeCacheEntry;

G_LOCK_DEFINE_STATIC (probe_cache);
static GHashTable *probe_cache;

static NuvashaLauncherJava *
java_copy (const NuvashaLauncherJava *src)
{
  NuvashaLauncherJava *copy = g_new0 (NuvashaLauncherJava, 1);

  copy->executable = g_strdup (src->executable);
  copy->arch = g_strdup (src->arch);
  copy->vendor = g_strdup (src->vendor);
  copy->major_version = src->major_version;
  copy->headless = src->headless;
  return copy;
}

static void
probe_cache_entry_free (gpointer data)
{
  ProbeCacheEntry *entry = data;

  if (entry == NULL)
    return;
  nuvashalauncher_java_free (entry->java);
  g_free (entry);
}

static gboolean
probe_cache_stat (const char *executable, struct stat *st)
{
  g_autofree char *resolved = NULL;
  const char *target = executable;

  if (executable == NULL || *executable == '\0')
    return FALSE;

  if (strchr (executable, G_DIR_SEPARATOR) == NULL)
    {
      resolved = g_find_program_in_path (executable);
      if (resolved == NULL)
        return FALSE;
      target = resolved;
    }

  return stat (target, st) == 0 && S_ISREG (st->st_mode);
}

static NuvashaLauncherJava *
probe_cache_lookup (const char *executable)
{
  struct stat st;
  ProbeCacheEntry *entry = NULL;

  if (!probe_cache_stat (executable, &st))
    return NULL;

  G_LOCK (probe_cache);
  if (probe_cache != NULL)
    entry = g_hash_table_lookup (probe_cache, executable);
  if (entry != NULL &&
      entry->dev == st.st_dev &&
      entry->ino == st.st_ino &&
      entry->size == st.st_size &&
      entry->mtime_sec == st.st_mtim.tv_sec &&
      entry->mtime_nsec == st.st_mtim.tv_nsec)
    {
      NuvashaLauncherJava *copy = java_copy (entry->java);
      G_UNLOCK (probe_cache);
      return copy;
    }
  G_UNLOCK (probe_cache);
  return NULL;
}

static void
probe_cache_store (const char *executable, const NuvashaLauncherJava *java)
{
  struct stat st;
  ProbeCacheEntry *entry = NULL;

  if (!probe_cache_stat (executable, &st))
    return;

  entry = g_new0 (ProbeCacheEntry, 1);
  entry->java = java_copy (java);
  entry->dev = st.st_dev;
  entry->ino = st.st_ino;
  entry->size = st.st_size;
  entry->mtime_sec = st.st_mtim.tv_sec;
  entry->mtime_nsec = st.st_mtim.tv_nsec;

  G_LOCK (probe_cache);
  if (probe_cache == NULL)
    probe_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, probe_cache_entry_free);
  g_hash_table_replace (probe_cache, g_strdup (executable), entry);
  G_UNLOCK (probe_cache);
}

typedef struct
{
  /* Borrowed from the operation the caller owns until the answer. */
  NuvashaLauncherTaskScope *scope;
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
      g_subprocess_force_exit (G_SUBPROCESS (source));
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
        g_error_new (NUVASHALAUNCHER_JAVA_ERROR, NUVASHALAUNCHER_JAVA_ERROR_PARSE,
                     "\"%s\" did not report a version", probe->executable));
      g_free (arch);
      g_free (vendor_out);
      probe_destroyed (probe);
      return;
    }

  NuvashaLauncherJava *java = g_new0 (NuvashaLauncherJava, 1);

  java->executable = g_steal_pointer (&probe->executable);
  java->major_version = major;
  java->arch = g_steal_pointer (&arch);
  java->vendor = g_steal_pointer (&vendor_out);
  java->headless = !java_has_xawt (java->executable);

  probe_cache_store (java->executable, java);

  nuvashalauncher_task_scope_return_pointer (probe->scope, java,
                                 (GDestroyNotify) nuvashalauncher_java_free);
  probe_destroyed (probe);
}

NuvashaLauncherTaskScope *
nuvashalauncher_java_probe_async (const char         *executable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autoptr (GError) error = NULL;
  const char * const argv[] = { executable, "-XshowSettings:properties", "-version",
                   NULL };
  NuvashaLauncherJava *cached = NULL;
  Probe *probe = NULL;

  nuvashalauncher_task_scope_set_check_cancellable (scope, FALSE);

  cached = probe_cache_lookup (executable);
  if (cached != NULL)
    {
      nuvashalauncher_task_scope_return_pointer_deferred (
        scope, cached, (GDestroyNotify) nuvashalauncher_java_free);
      return g_steal_pointer (&scope);
    }

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

NuvashaLauncherJava *
nuvashalauncher_java_probe_finish (GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Dedup + bounded-parallel probing over a list of candidates
 * ---------------------------------------------------------- */

#define JAVA_PROBE_CONCURRENCY 4

typedef struct
{
  char **executables;
  guint total;
  guint next;
  guint in_flight;
  NuvashaLauncherJava **results;
  GPtrArray *javas;   /* NuvashaLauncherJava*, element free nuvashalauncher_java_free */
  GHashTable *seen;   /* resolved real paths, for dedup */
  NuvashaLauncherTaskScope *scope;
  gboolean finished;
} ProbeList;

typedef struct
{
  ProbeList *list;
  guint index;
} ProbeSlot;

static void
probe_list_destroyed (gpointer data)
{
  ProbeList *list = data;

  if (list->results != NULL)
    {
      for (guint i = 0; i < list->total; i++)
        nuvashalauncher_java_free (list->results[i]);
      g_free (list->results);
    }
  g_strfreev (list->executables);
  g_clear_pointer (&list->javas, g_ptr_array_unref);
  g_clear_pointer (&list->seen, g_hash_table_unref);
  g_clear_pointer (&list->scope, nuvashalauncher_task_scope_unref);
  g_free (list);
}

static NuvashaLauncherJava *
nuvashalauncher_java_add_unique (GPtrArray  *javas,
                     GHashTable *seen,
                     NuvashaLauncherJava    *java)
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

static void probe_list_slot_probed (GObject *source, GAsyncResult *result,
                                    gpointer user_data);

static void
probe_list_pump (ProbeList *list)
{
  GCancellable *cancellable = nuvashalauncher_task_scope_get_cancellable (list->scope);

  if (list->finished)
    return;

  while (!g_cancellable_is_cancelled (cancellable) &&
         list->in_flight < JAVA_PROBE_CONCURRENCY &&
         list->next < list->total)
    {
      ProbeSlot *slot = g_new0 (ProbeSlot, 1);
      NuvashaLauncherTaskScope *probe = NULL;

      slot->list = list;
      slot->index = list->next++;
      list->in_flight++;

      probe = nuvashalauncher_java_probe_async (list->executables[slot->index],
                                                probe_list_slot_probed, slot);
      nuvashalauncher_task_scope_follow_cancellable (probe, cancellable);
      nuvashalauncher_task_scope_unref_when_done (probe);
    }

  if (list->in_flight == 0 &&
      (g_cancellable_is_cancelled (cancellable) || list->next >= list->total))
    {
      list->finished = TRUE;
      if (g_cancellable_is_cancelled (cancellable))
        {
          nuvashalauncher_task_scope_return_error_deferred (
            list->scope,
            g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled"));
        }
      else
        {
          for (guint i = 0; i < list->total; i++)
            {
              if (list->results[i] != NULL)
                {
                  nuvashalauncher_java_add_unique (list->javas, list->seen,
                                                   list->results[i]);
                  list->results[i] = NULL;
                }
            }
          nuvashalauncher_task_scope_return_pointer_deferred (
            list->scope, g_steal_pointer (&list->javas),
            (GDestroyNotify) g_ptr_array_unref);
        }
      probe_list_destroyed (list);
    }
}

static void
probe_list_slot_probed (GObject      *source G_GNUC_UNUSED,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  ProbeSlot *slot = user_data;
  ProbeList *list = slot->list;
  guint index = slot->index;
  g_autoptr (GError) error = NULL;
  NuvashaLauncherJava *java = nuvashalauncher_java_probe_finish (result, &error);

  g_free (slot);
  list->in_flight--;

  if (java != NULL)
    list->results[index] = java;

  probe_list_pump (list);
}

NuvashaLauncherTaskScope *
nuvashalauncher_java_probe_list_async (char            **executables,
                           GAsyncReadyCallback callback,
                           gpointer         user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  ProbeList *list = NULL;

  g_return_val_if_fail (executables != NULL, NULL);

  nuvashalauncher_task_scope_set_check_cancellable (scope, FALSE);

  list = g_new0 (ProbeList, 1);
  list->executables = executables; /* the caller gives up the array */
  list->total = g_strv_length (executables);
  list->results = list->total > 0 ? g_new0 (NuvashaLauncherJava *, list->total) : NULL;
  list->javas = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_java_free);
  list->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  list->scope = nuvashalauncher_task_scope_ref (scope);

  probe_list_pump (list);
  return g_steal_pointer (&scope);
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
  g_autofree char *launcher_java = nuvashalauncher_java_get_dir ();
  char **out = g_new0 (char *, G_N_ELEMENTS (system) + 6);
  guint n = 0;

  for (guint i = 0; system[i] != NULL; i++)
    out[n++] = g_strdup (system[i]);

  out[n++] = g_steal_pointer (&jdks);
  out[n++] = g_steal_pointer (&sdkman);
  out[n++] = g_steal_pointer (&asdf);
  out[n++] = g_steal_pointer (&mise);
  out[n++] = g_steal_pointer (&launcher_java);
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

  settings = nuvashalauncher_settings_open_gsettings ();
  if (settings == NULL)
    return;
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

/* The system the Fedora, Arch etc. layout plus $JAVA_HOME and PATH.
 * Inputs from GSettings/env are captured on the main thread so the filesystem
 * scan and realpath() deduplication can run safely on a worker thread. */
static char **
collect_candidates_from_inputs (char * const *roots,
                                const char   *java_home,
                                const char   *on_path,
                                char * const *custom,
                                char       ***out_valid_custom,
                                gboolean     *out_pruned)
{
  g_autoptr (GPtrArray) candidates =
    g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) valid_custom = g_ptr_array_new_with_free_func (g_free);
  gboolean pruned = FALSE;

  for (guint i = 0; roots != NULL && roots[i] != NULL; i++)
    add_root_scandir (candidates, roots[i]);

  if (java_home != NULL && *java_home != '\0')
    {
      g_autofree char *candidate =
        g_build_filename (java_home, "bin", "java", NULL);

      add_candidate (candidates, candidate);
    }

  for (guint i = 0; custom != NULL && custom[i] != NULL; i++)
    {
      if (g_file_test (custom[i], G_FILE_TEST_IS_REGULAR) &&
          g_file_test (custom[i], G_FILE_TEST_IS_EXECUTABLE))
        {
          g_ptr_array_add (candidates, g_strdup (custom[i]));
          g_ptr_array_add (valid_custom, g_strdup (custom[i]));
        }
      else
        pruned = TRUE;
    }

  add_candidate (candidates, on_path);

  if (out_pruned != NULL)
    *out_pruned = pruned;
  if (out_valid_custom != NULL && pruned)
    {
      g_ptr_array_add (valid_custom, NULL);
      *out_valid_custom = (char **) g_ptr_array_free (g_steal_pointer (&valid_custom), FALSE);
    }

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
  NuvashaLauncherTaskScope *scope;
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

typedef struct
{
  NuvashaLauncherTaskScope *scope;
  gboolean use_custom_roots;
  GStrv roots;
  char *java_home;
  char *on_path;
  GStrv custom;
  GStrv valid_custom;
  gboolean pruned;
  GStrv candidates;
} CandidateScan;

static void
candidate_scan_free (CandidateScan *scan)
{
  if (scan->scope != NULL)
    nuvashalauncher_task_scope_unref (scan->scope);
  g_strfreev (scan->roots);
  g_free (scan->java_home);
  g_free (scan->on_path);
  g_strfreev (scan->custom);
  g_strfreev (scan->valid_custom);
  g_strfreev (scan->candidates);
  g_free (scan);
}

static void
candidate_scan_thread (GTask *task,
                       gpointer source_object G_GNUC_UNUSED,
                       gpointer task_data,
                       GCancellable *cancellable G_GNUC_UNUSED)
{
  CandidateScan *scan = task_data;
  g_auto (GStrv) collected = NULL;

  if (scan->use_custom_roots)
    collected = collect_candidates_list (scan->roots);
  else
    collected = collect_candidates_from_inputs (
      scan->roots, scan->java_home, scan->on_path, scan->custom,
      &scan->valid_custom, &scan->pruned);

  scan->candidates = dedupe_candidates (collected);
  g_task_return_boolean (task, TRUE);
}

static void
candidate_scan_ready (GObject *source G_GNUC_UNUSED,
                      GAsyncResult *result,
                      gpointer user_data)
{
  CandidateScan *scan = user_data;
  g_autoptr (GError) error = NULL;
  NuvashaLauncherTaskScope *scope = scan->scope;
  Detect *detect = NULL;
  NuvashaLauncherTaskScope *listing = NULL;

  g_task_propagate_boolean (G_TASK (result), &error);

  if (g_cancellable_is_cancelled (nuvashalauncher_task_scope_get_cancellable (scope)))
    {
      nuvashalauncher_task_scope_return_error (
        scope, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
      candidate_scan_free (scan);
      return;
    }

  if (!scan->use_custom_roots && scan->pruned && scan->valid_custom != NULL)
    {
      g_autoptr (GSettings) settings = nuvashalauncher_settings_open_gsettings ();
      if (settings != NULL)
        g_settings_set_strv (settings, "custom-java-paths",
                             (const char * const *) scan->valid_custom);
    }

  detect = g_new0 (Detect, 1);
  detect->scope = nuvashalauncher_task_scope_ref (scope);

  listing = nuvashalauncher_java_probe_list_async (
    g_steal_pointer (&scan->candidates), detected, detect);
  nuvashalauncher_task_scope_follow_cancellable (
    listing, nuvashalauncher_task_scope_get_cancellable (scope));
  nuvashalauncher_task_scope_unref_when_done (listing);

  candidate_scan_free (scan);
}

NuvashaLauncherTaskScope *
nuvashalauncher_java_detect_async (GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autoptr (GSettings) settings = nuvashalauncher_settings_open_gsettings ();
  CandidateScan *scan = g_new0 (CandidateScan, 1);
  g_autoptr (GTask) task = NULL;

  nuvashalauncher_task_scope_set_check_cancellable (scope, FALSE);

  scan->scope = nuvashalauncher_task_scope_ref (scope);
  scan->use_custom_roots = FALSE;
  scan->roots = default_roots ();
  scan->java_home = g_strdup (g_getenv ("JAVA_HOME"));
  scan->on_path = g_find_program_in_path ("java");
  scan->custom = settings != NULL
                   ? g_settings_get_strv (settings, "custom-java-paths")
                   : NULL;

  task = g_task_new (NULL, nuvashalauncher_task_scope_get_cancellable (scope),
                     candidate_scan_ready, scan);
  g_task_set_task_data (task, scan, NULL);
  g_task_run_in_thread (task, candidate_scan_thread);

  return g_steal_pointer (&scope);
}

NuvashaLauncherTaskScope *
nuvashalauncher_java_detect_list_async (char            **roots,
                            GAsyncReadyCallback callback,
                            gpointer         user_data)
{
  g_autoptr (NuvashaLauncherTaskScope) scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  CandidateScan *scan = g_new0 (CandidateScan, 1);
  g_autoptr (GTask) task = NULL;

  nuvashalauncher_task_scope_set_check_cancellable (scope, FALSE);

  scan->scope = nuvashalauncher_task_scope_ref (scope);
  scan->use_custom_roots = TRUE;
  scan->roots = g_strdupv (roots);

  task = g_task_new (NULL, nuvashalauncher_task_scope_get_cancellable (scope),
                     candidate_scan_ready, scan);
  g_task_set_task_data (task, scan, NULL);
  g_task_run_in_thread (task, candidate_scan_thread);

  return g_steal_pointer (&scope);
}

GPtrArray *
nuvashalauncher_java_detect_finish (GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (nuvashalauncher_task_result_is_valid (result, "nuvashalauncher-task-scope"), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
