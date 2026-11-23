/* Parallel isolated test runner in C17 + POSIX. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 500

#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct
{
  const char *binary;
  char *output;
  size_t output_len;
  int status;
} TestResult;

typedef struct
{
  const char *root_tmp;
  const char *schema_dir;
  int count;
  atomic_int next_idx;
  int *order;
  TestResult *results;
} RunnerCtx;

static int
unlink_cb (const char *fpath, const struct stat *sb,
           int typeflag, struct FTW *ftwbuf)
{
  (void) sb;
  (void) typeflag;
  (void) ftwbuf;
  return remove (fpath);
}

static void
rm_rf (const char *path)
{
  nftw (path, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
}

static void
mkdir_p (const char *path, mode_t mode)
{
  if (mkdir (path, mode) != 0 && errno != EEXIST)
    perror (path);
}

static char *
xasprintf (const char *fmt, const char *a, const char *b)
{
  int needed = snprintf (NULL, 0, fmt, a, b);
  char *buf = malloc ((size_t) needed + 1);
  if (buf == NULL)
    abort ();
  snprintf (buf, (size_t) needed + 1, fmt, a, b);
  return buf;
}

static char **
build_env (const char *work, const char *schema_dir)
{
  const char *overrides[] = {
    "NUVASHALAUNCHER_TEST_SECRET_STORE",
    "NUVASHALAUNCHER_TEST_SECRET_STORE_FILE",
    "NUVASHALAUNCHER_TEST_FAST_TIMERS",
    "TMPDIR",
    "XDG_CONFIG_HOME",
    "XDG_DATA_HOME",
    "XDG_CACHE_HOME",
    "XDG_STATE_HOME",
    "XDG_RUNTIME_DIR",
    "GSETTINGS_SCHEMA_DIR",
    "GSETTINGS_BACKEND",
  };
  size_t n_over = sizeof (overrides) / sizeof (overrides[0]);
  size_t env_count = 0;

  while (environ[env_count] != NULL)
    env_count++;

  char **envp = calloc (env_count + n_over + 1, sizeof (char *));
  if (envp == NULL)
    abort ();

  size_t out = 0;
  for (size_t i = 0; i < env_count; i++)
    {
      bool skip = false;
      for (size_t j = 0; j < n_over; j++)
        {
          size_t klen = strlen (overrides[j]);
          if (strncmp (environ[i], overrides[j], klen) == 0 &&
              environ[i][klen] == '=')
            {
              skip = true;
              break;
            }
        }
      if (!skip)
        envp[out++] = strdup (environ[i]);
    }

  envp[out++] = strdup ("NUVASHALAUNCHER_TEST_SECRET_STORE=1");
  envp[out++] = xasprintf ("NUVASHALAUNCHER_TEST_SECRET_STORE_FILE=%s/secrets.json", work, "");
  envp[out++] = strdup ("NUVASHALAUNCHER_TEST_FAST_TIMERS=1");
  envp[out++] = xasprintf ("TMPDIR=%s", work, "");
  envp[out++] = xasprintf ("XDG_CONFIG_HOME=%s/config", work, "");
  envp[out++] = xasprintf ("XDG_DATA_HOME=%s/data", work, "");
  envp[out++] = xasprintf ("XDG_CACHE_HOME=%s/cache", work, "");
  envp[out++] = xasprintf ("XDG_STATE_HOME=%s/state", work, "");
  envp[out++] = xasprintf ("XDG_RUNTIME_DIR=%s/runtime", work, "");
  envp[out++] = xasprintf ("GSETTINGS_SCHEMA_DIR=%s", schema_dir, "");
  envp[out++] = strdup ("GSETTINGS_BACKEND=memory");
  envp[out] = NULL;
  return envp;
}

static void
free_env (char **envp)
{
  for (size_t i = 0; envp[i] != NULL; i++)
    free (envp[i]);
  free (envp);
}

static void
run_one (RunnerCtx *ctx, int idx)
{
  TestResult *res = &ctx->results[idx];
  char work[2048];
  char sub[2176];
  const char *subs[] = { "config", "data", "cache", "state", "runtime" };

  snprintf (work, sizeof (work), "%s/t-%d", ctx->root_tmp, idx);
  mkdir_p (work, 0755);
  for (size_t i = 0; i < 5; i++)
    {
      snprintf (sub, sizeof (sub), "%s/%s", work, subs[i]);
      mkdir_p (sub, i == 4 ? 0700 : 0755);
    }

  char **envp = build_env (work, ctx->schema_dir);
  int pipefd[2];
  if (pipe (pipefd) != 0)
    abort ();

  pid_t pid = fork ();
  if (pid < 0)
    abort ();

  if (pid == 0)
    {
      dup2 (pipefd[1], STDOUT_FILENO);
      dup2 (pipefd[1], STDERR_FILENO);
      close (pipefd[0]);
      close (pipefd[1]);
      char *argv[] = { (char *) res->binary, NULL };
      execve (res->binary, argv, envp);
      _exit (127);
    }

  close (pipefd[1]);
  free_env (envp);

  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc (cap);
  if (buf == NULL)
    abort ();

  for (;;)
    {
      if (len + 2048 > cap)
        {
          cap *= 2;
          char *nbuf = realloc (buf, cap);
          if (nbuf == NULL)
            abort ();
          buf = nbuf;
        }
      ssize_t n = read (pipefd[0], buf + len, cap - len - 1);
      if (n > 0)
        len += (size_t) n;
      else if (n == 0)
        break;
      else if (errno != EINTR)
        break;
    }
  buf[len] = '\0';
  close (pipefd[0]);

  int wstatus = 0;
  while (waitpid (pid, &wstatus, 0) < 0 && errno == EINTR)
    ;

  if (WIFEXITED (wstatus))
    res->status = WEXITSTATUS (wstatus);
  else if (WIFSIGNALED (wstatus))
    res->status = 128 + WTERMSIG (wstatus);
  else
    res->status = 1;

  res->output = buf;
  res->output_len = len;
}

static int
priority_rank (const char *binary)
{
  const char *base = strrchr (binary, '/');
  const char *name = base != NULL ? base + 1 : binary;
  static const char *const heavy[] = {
    "test-mrpack-security",
    "test-launcher-process",
    "test-cli",
    "test-download-queue",
    "test-modpack-install",
    "test-versions-network",
  };
  for (size_t i = 0; i < sizeof (heavy) / sizeof (heavy[0]); i++)
    {
      if (strcmp (name, heavy[i]) == 0)
        return (int) i;
    }
  return 100;
}

static void *
worker_main (void *arg)
{
  RunnerCtx *ctx = arg;
  for (;;)
    {
      int pos = atomic_fetch_add (&ctx->next_idx, 1);
      if (pos >= ctx->count)
        break;
      run_one (ctx, ctx->order[pos]);
    }
  return NULL;
}

static void
count_tap (const char *output, bool *is_tap, int *tap_pass, int *tap_skip)
{
  *is_tap = false;
  *tap_pass = 0;
  *tap_skip = 0;
  const char *p = output;
  while (*p != '\0')
    {
      const char *eol = strchr (p, '\n');
      size_t linelen = eol != NULL ? (size_t) (eol - p) : strlen (p);

      if (linelen >= 11 && strncmp (p, "TAP version", 11) == 0)
        *is_tap = true;
      else if (linelen >= 3 && strncmp (p, "ok ", 3) == 0)
        {
          (*tap_pass)++;
          for (size_t i = 3; i + 6 <= linelen; i++)
            {
              if (strncmp (p + i, "# SKIP", 6) == 0)
                {
                  (*tap_skip)++;
                  break;
                }
            }
        }
      p = eol != NULL ? eol + 1 : p + linelen;
    }
}

int
main (int argc, char **argv)
{
  const char *schema_dir = NULL;
  int first_bin = 1;

  if (argc >= 3 && strcmp (argv[1], "--schema-dir") == 0)
    {
      schema_dir = argv[2];
      first_bin = 3;
    }
  if (schema_dir == NULL || first_bin >= argc)
    {
      fprintf (stderr, "usage: %s --schema-dir DIR TEST...\n", argv[0]);
      return 2;
    }

  mkdir_p ("build", 0755);
  char cwd[1024];
  if (getcwd (cwd, sizeof (cwd)) == NULL)
    {
      perror ("getcwd");
      return 1;
    }
  char root_tmpl[1100];
  snprintf (root_tmpl, sizeof (root_tmpl), "%s/build/test-tmp-XXXXXX", cwd);
  char *root_tmp = mkdtemp (root_tmpl);
  if (root_tmp == NULL)
    {
      perror ("mkdtemp");
      return 1;
    }

  int count = argc - first_bin;
  RunnerCtx ctx = {
    .root_tmp = root_tmp,
    .schema_dir = schema_dir,
    .count = count,
    .next_idx = 0,
    .order = calloc ((size_t) count, sizeof (int)),
    .results = calloc ((size_t) count, sizeof (TestResult)),
  };
  if (ctx.order == NULL || ctx.results == NULL)
    abort ();
  for (int i = 0; i < count; i++)
    ctx.results[i].binary = argv[first_bin + i];

  int pos = 0;
  for (int pass = 0; pass <= 7; pass++)
    {
      for (int i = 0; i < count; i++)
        {
          int r = priority_rank (ctx.results[i].binary);
          if ((pass < 7 && r == pass) || (pass == 7 && r >= 7))
            ctx.order[pos++] = i;
        }
    }

  long nproc = sysconf (_SC_NPROCESSORS_ONLN);
  int jobs = (nproc > 0 && nproc <= 64) ? (int) nproc : 4;
  if (jobs > count)
    jobs = count;

  pthread_t *threads = calloc ((size_t) jobs, sizeof (pthread_t));
  if (threads == NULL)
    abort ();
  for (int i = 0; i < jobs; i++)
    pthread_create (&threads[i], NULL, worker_main, &ctx);
  for (int i = 0; i < jobs; i++)
    pthread_join (threads[i], NULL);
  free (threads);

  int passed = 0;
  int skipped = 0;
  int exit_code = 0;

  for (int i = 0; i < count; i++)
    {
      TestResult *res = &ctx.results[i];
      printf ("RUN  %s\n", res->binary);
      if (res->output_len > 0)
        fwrite (res->output, 1, res->output_len, stdout);

      if (res->status == 77)
        skipped++;
      else if (res->status != 0)
        {
          exit_code = res->status;
          break;
        }
      else
        {
          bool is_tap = false;
          int tap_pass = 0;
          int tap_skip = 0;
          count_tap (res->output, &is_tap, &tap_pass, &tap_skip);
          if (is_tap)
            {
              passed += tap_pass - tap_skip;
              skipped += tap_skip;
            }
          else
            passed++;
        }
    }

  if (exit_code == 0)
    printf ("PASS %d test(s), SKIP %d test(s)\n", passed, skipped);

  for (int i = 0; i < count; i++)
    free (ctx.results[i].output);
  free (ctx.order);
  free (ctx.results);
  rm_rf (root_tmp);
  return exit_code;
}
