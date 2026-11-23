/* Parallel isolated test runner in C17 + POSIX. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 500

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct
{
  const char *binary;
  int iter;
  char *output;
  size_t output_len;
  char *work_dir;
  int status;
  bool timed_out;
  bool ran;
  double elapsed_sec;
} TestResult;

typedef struct
{
  const char *root_tmp;
  const char *schema_dir;
  const char *case_pattern;
  int count;
  int repeat;
  int timeout_sec;
  int num_width;
  bool verbose;
  bool fail_fast;
  bool keep_tmp;
  bool is_debug;
  bool color;
  const char *c_green;
  const char *c_yellow;
  const char *c_red;
  const char *c_dim;
  const char *c_reset;
  atomic_int next_idx;
  atomic_bool stop_requested;
  pthread_mutex_t print_lock;
  int done_count;
  int *order;
  TestResult *results;
} RunnerCtx;

static double
monotonic_sec (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static const char *
base_name (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash != NULL ? slash + 1 : path;
}

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

static bool
env_truthy (const char *name)
{
  const char *v = getenv (name);
  return v != NULL && strcmp (v, "") != 0 && strcmp (v, "0") != 0;
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
  double t0 = monotonic_sec ();
  char work[2048];
  char sub[2176];
  const char *subs[] = { "config", "data", "cache", "state", "runtime" };

  snprintf (work, sizeof (work), "%s/t-%d", ctx->root_tmp, idx);
  res->work_dir = strdup (work);
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
      setpgid (0, 0);
      dup2 (pipefd[1], STDOUT_FILENO);
      dup2 (pipefd[1], STDERR_FILENO);
      close (pipefd[0]);
      close (pipefd[1]);
      if (ctx->case_pattern != NULL)
        {
          char *argv[] = {
            (char *) res->binary,
            "-p",
            (char *) ctx->case_pattern,
            NULL
          };
          execve (res->binary, argv, envp);
        }
      else
        {
          char *argv[] = { (char *) res->binary, NULL };
          execve (res->binary, argv, envp);
        }
      _exit (127);
    }

  close (pipefd[1]);
  free_env (envp);

  if (ctx->timeout_sec > 0)
    {
      int flags = fcntl (pipefd[0], F_GETFL, 0);
      if (flags >= 0)
        fcntl (pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc (cap);
  if (buf == NULL)
    abort ();

  double deadline = ctx->timeout_sec > 0 ? t0 + (double) ctx->timeout_sec : 0.0;
  bool timed_out = false;

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

      if (ctx->timeout_sec > 0)
        {
          double rem = deadline - monotonic_sec ();
          if (rem <= 0.0)
            {
              timed_out = true;
              kill (-pid, SIGKILL);
              break;
            }
          int wait_ms = (int) (rem * 1000.0) + 1;
          if (wait_ms > 250)
            wait_ms = 250;
          struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
          int pr = poll (&pfd, 1, wait_ms);
          if (pr < 0)
            {
              if (errno == EINTR)
                continue;
              break;
            }
          if (pr == 0)
            continue;
        }

      ssize_t n = read (pipefd[0], buf + len, cap - len - 1);
      if (n > 0)
        len += (size_t) n;
      else if (n == 0)
        break;
      else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
        break;
    }
  buf[len] = '\0';
  close (pipefd[0]);

  int wstatus = 0;
  while (waitpid (pid, &wstatus, 0) < 0 && errno == EINTR)
    ;

  res->ran = true;
  res->timed_out = timed_out;
  if (timed_out)
    res->status = 124;
  else if (WIFEXITED (wstatus))
    res->status = WEXITSTATUS (wstatus);
  else if (WIFSIGNALED (wstatus))
    res->status = 128 + WTERMSIG (wstatus);
  else
    res->status = 1;

  res->output = buf;
  res->output_len = len;
  res->elapsed_sec = monotonic_sec () - t0;
}

static int
priority_rank (const char *binary)
{
  const char *name = base_name (binary);
  static const char *const heavy[] = {
    "test-window",
    "test-skin-dialog",
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

static void
count_tap (const char *output, bool *is_tap, int *tap_pass, int *tap_skip)
{
  *is_tap = false;
  *tap_pass = 0;
  *tap_skip = 0;
  if (output == NULL)
    return;
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

static void
extract_skip_reason (const char *output, char *out, size_t out_cap)
{
  out[0] = '\0';
  if (output == NULL)
    return;
  const char *p = output;
  while (*p != '\0')
    {
      const char *eol = strchr (p, '\n');
      size_t linelen = eol != NULL ? (size_t) (eol - p) : strlen (p);
      if (linelen >= 10 && strncmp (p, "skipped - ", 10) == 0)
        {
          size_t n = linelen - 10;
          if (n >= out_cap)
            n = out_cap - 1;
          memcpy (out, p + 10, n);
          out[n] = '\0';
          return;
        }
      p = eol != NULL ? eol + 1 : p + linelen;
    }
}

static void
extract_tap_seed (const char *output, char *out, size_t out_cap)
{
  out[0] = '\0';
  if (output == NULL)
    return;
  const char *needle = "# random seed: ";
  const char *hit = strstr (output, needle);
  if (hit == NULL)
    return;
  hit += strlen (needle);
  const char *eol = strchr (hit, '\n');
  size_t n = eol != NULL ? (size_t) (eol - hit) : strlen (hit);
  if (n >= out_cap)
    n = out_cap - 1;
  memcpy (out, hit, n);
  out[n] = '\0';
}

static bool
line_is_failure_highlight (const char *line, size_t len)
{
  if ((len >= 7 && strncmp (line, "not ok ", 7) == 0) ||
      (len >= 9 && strncmp (line, "Bail out!", 9) == 0) ||
      (len >= 6 && strncmp (line, "ERROR:", 6) == 0) ||
      (len >= 9 && strncmp (line, "** ERROR:", 9) == 0))
    return true;

  char tmp[512];
  size_t n = len < sizeof (tmp) - 1 ? len : sizeof (tmp) - 1;
  memcpy (tmp, line, n);
  tmp[n] = '\0';
  return strstr (tmp, "SUMMARY: AddressSanitizer") != NULL ||
         strstr (tmp, "SUMMARY: UndefinedBehaviorSanitizer") != NULL ||
         strstr (tmp, "SUMMARY: LeakSanitizer") != NULL ||
         strstr (tmp, "runtime error:") != NULL ||
         strstr (tmp, "assertion failed") != NULL ||
         strstr (tmp, "Assertion ") != NULL;
}

static void
print_failure_output (const RunnerCtx *ctx, const char *output, bool is_tap)
{
  if (output == NULL || output[0] == '\0')
    return;

  int hidden_ok = 0;
  const char *p = output;
  while (*p != '\0')
    {
      const char *eol = strchr (p, '\n');
      size_t linelen = eol != NULL ? (size_t) (eol - p) : strlen (p);

      if (is_tap && linelen >= 3 && strncmp (p, "ok ", 3) == 0)
        {
          hidden_ok++;
          p = eol != NULL ? eol + 1 : p + linelen;
          continue;
        }

      bool hl = line_is_failure_highlight (p, linelen);
      if (hl)
        fwrite (ctx->c_red, 1, strlen (ctx->c_red), stdout);
      fwrite (p, 1, linelen, stdout);
      if (hl)
        fwrite (ctx->c_reset, 1, strlen (ctx->c_reset), stdout);
      putchar ('\n');

      p = eol != NULL ? eol + 1 : p + linelen;
    }

  if (hidden_ok > 0)
    printf ("%s# (%d passing TAP subtest(s) hidden; rerun with V=1 to show)%s\n",
            ctx->c_dim, hidden_ok, ctx->c_reset);
}

static void
format_label (const TestResult *res, int repeat, char *buf, size_t cap)
{
  const char *name = base_name (res->binary);
  if (repeat > 1)
    snprintf (buf, cap, "%s [%d/%d]", name, res->iter, repeat);
  else
    snprintf (buf, cap, "%s", name);
}

static void
report_result (RunnerCtx *ctx, const TestResult *res)
{
  char label[160];
  format_label (res, ctx->repeat, label, sizeof (label));

  pthread_mutex_lock (&ctx->print_lock);
  int done = ++ctx->done_count;

  if (ctx->verbose)
    {
      printf ("%s[%*d/%d]%s RUN  %s\n",
              ctx->c_dim, ctx->num_width, done, ctx->count, ctx->c_reset,
              res->binary);
      if (res->output_len > 0)
        fwrite (res->output, 1, res->output_len, stdout);
    }

  if (res->status == 77)
    {
      if (!ctx->verbose)
        {
          char reason[128];
          extract_skip_reason (res->output, reason, sizeof (reason));
          if (reason[0] != '\0')
            printf ("%s[%*d/%d]%s %sSKIP%s  %-34s %s(%.2fs, %s)%s\n",
                    ctx->c_dim, ctx->num_width, done, ctx->count, ctx->c_reset,
                    ctx->c_yellow, ctx->c_reset, label, ctx->c_dim,
                    res->elapsed_sec, reason, ctx->c_reset);
          else
            printf ("%s[%*d/%d]%s %sSKIP%s  %-34s %s(%.2fs)%s\n",
                    ctx->c_dim, ctx->num_width, done, ctx->count, ctx->c_reset,
                    ctx->c_yellow, ctx->c_reset, label, ctx->c_dim,
                    res->elapsed_sec, ctx->c_reset);
        }
    }
  else if (res->status != 0)
    {
      if (!ctx->verbose)
        {
          if (res->timed_out)
            printf ("%s[%*d/%d]%s %sFAIL%s  %-34s %s(TIMEOUT after %ds)%s\n",
                    ctx->c_dim, ctx->num_width, done, ctx->count, ctx->c_reset,
                    ctx->c_red, ctx->c_reset, label, ctx->c_dim,
                    ctx->timeout_sec, ctx->c_reset);
          else
            printf ("%s[%*d/%d]%s %sFAIL%s  %-34s %s(exit %d, %.2fs)%s\n",
                    ctx->c_dim, ctx->num_width, done, ctx->count, ctx->c_reset,
                    ctx->c_red, ctx->c_reset, label, ctx->c_dim,
                    res->status, res->elapsed_sec, ctx->c_reset);
        }
    }
  else
    {
      bool is_tap = false;
      int tap_pass = 0;
      int tap_skip = 0;
      count_tap (res->output, &is_tap, &tap_pass, &tap_skip);
      int bin_pass = is_tap ? (tap_pass - tap_skip) : 1;
      int bin_skip = is_tap ? tap_skip : 0;

      if (!ctx->verbose)
        {
          if (bin_skip > 0)
            printf ("%s[%*d/%d]%s %sPASS%s  %-34s %s(%.2fs, %d passed, %d skipped)%s\n",
                    ctx->c_dim, ctx->num_width, done, ctx->count, ctx->c_reset,
                    ctx->c_green, ctx->c_reset, label, ctx->c_dim,
                    res->elapsed_sec, bin_pass, bin_skip, ctx->c_reset);
          else
            printf ("%s[%*d/%d]%s %sPASS%s  %-34s %s(%.2fs)%s\n",
                    ctx->c_dim, ctx->num_width, done, ctx->count, ctx->c_reset,
                    ctx->c_green, ctx->c_reset, label, ctx->c_dim,
                    res->elapsed_sec, ctx->c_reset);
        }
    }

  fflush (stdout);
  pthread_mutex_unlock (&ctx->print_lock);
}

static void *
worker_main (void *arg)
{
  RunnerCtx *ctx = arg;
  for (;;)
    {
      if (ctx->fail_fast && atomic_load (&ctx->stop_requested))
        break;
      int pos = atomic_fetch_add (&ctx->next_idx, 1);
      if (pos >= ctx->count)
        break;
      int idx = ctx->order[pos];
      run_one (ctx, idx);
      TestResult *res = &ctx->results[idx];
      if (res->status != 0 && res->status != 77 && ctx->fail_fast)
        atomic_store (&ctx->stop_requested, true);
      report_result (ctx, res);
    }
  return NULL;
}

static int
count_digits (int n)
{
  int d = 1;
  while (n >= 10)
    {
      n /= 10;
      d++;
    }
  return d;
}

int
main (int argc, char **argv)
{
  const char *schema_dir = NULL;
  const char *case_pattern = getenv ("NUVASHALAUNCHER_TEST_CASE");
  if (case_pattern != NULL && case_pattern[0] == '\0')
    case_pattern = NULL;

  bool verbose = env_truthy ("NUVASHALAUNCHER_TEST_VERBOSE");
  bool fail_fast = env_truthy ("NUVASHALAUNCHER_TEST_FAIL_FAST");
  bool keep_tmp = env_truthy ("NUVASHALAUNCHER_TEST_KEEP_TMP");
  bool is_debug = false;
  bool list_only = false;

  int timeout_sec = 0;
  const char *timeout_env = getenv ("NUVASHALAUNCHER_TEST_TIMEOUT");
  if (timeout_env != NULL && timeout_env[0] != '\0')
    timeout_sec = atoi (timeout_env);

  int repeat = 1;
  const char *repeat_env = getenv ("NUVASHALAUNCHER_TEST_REPEAT");
  if (repeat_env != NULL && repeat_env[0] != '\0')
    repeat = atoi (repeat_env);

  int i = 1;
  while (i < argc)
    {
      if (strcmp (argv[i], "--schema-dir") == 0 && i + 1 < argc)
        {
          schema_dir = argv[i + 1];
          i += 2;
        }
      else if (strcmp (argv[i], "--verbose") == 0)
        {
          verbose = true;
          i++;
        }
      else if (strcmp (argv[i], "--fail-fast") == 0)
        {
          fail_fast = true;
          i++;
        }
      else if (strcmp (argv[i], "--keep-tmp") == 0)
        {
          keep_tmp = true;
          i++;
        }
      else if (strcmp (argv[i], "--debug") == 0)
        {
          is_debug = true;
          i++;
        }
      else if (strcmp (argv[i], "--list") == 0)
        {
          list_only = true;
          i++;
        }
      else if (strcmp (argv[i], "--timeout") == 0 && i + 1 < argc)
        {
          timeout_sec = atoi (argv[i + 1]);
          i += 2;
        }
      else if (strcmp (argv[i], "--repeat") == 0 && i + 1 < argc)
        {
          repeat = atoi (argv[i + 1]);
          i += 2;
        }
      else if (strcmp (argv[i], "--case") == 0 && i + 1 < argc)
        {
          case_pattern = argv[i + 1];
          if (case_pattern[0] == '\0')
            case_pattern = NULL;
          i += 2;
        }
      else if (strcmp (argv[i], "--") == 0)
        {
          i++;
          break;
        }
      else
        break;
    }

  if (repeat < 1)
    repeat = 1;
  if (timeout_sec < 0)
    timeout_sec = 0;

  if (list_only)
    {
      for (int j = i; j < argc; j++)
        printf ("%s\n", base_name (argv[j]));
      return 0;
    }

  if (schema_dir == NULL || i >= argc)
    {
      fprintf (stderr,
               "usage: %s --schema-dir DIR [--verbose] [--fail-fast] "
               "[--keep-tmp] [--timeout SEC] [--repeat N] [--case PAT] "
               "[--debug] [--list] TEST...\n",
               argv[0]);
      return 2;
    }

  int first_bin = i;
  int num_bins = argc - first_bin;
  int count = num_bins * repeat;
  double suite_t0 = monotonic_sec ();

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

  bool color = isatty (STDOUT_FILENO) && getenv ("NO_COLOR") == NULL;
  RunnerCtx ctx = {
    .root_tmp = root_tmp,
    .schema_dir = schema_dir,
    .case_pattern = case_pattern,
    .count = count,
    .repeat = repeat,
    .timeout_sec = timeout_sec,
    .num_width = count_digits (count),
    .verbose = verbose,
    .fail_fast = fail_fast,
    .keep_tmp = keep_tmp,
    .is_debug = is_debug,
    .color = color,
    .c_green = color ? "\033[32m" : "",
    .c_yellow = color ? "\033[33m" : "",
    .c_red = color ? "\033[31m" : "",
    .c_dim = color ? "\033[2m" : "",
    .c_reset = color ? "\033[0m" : "",
    .next_idx = 0,
    .stop_requested = false,
    .done_count = 0,
    .order = calloc ((size_t) count, sizeof (int)),
    .results = calloc ((size_t) count, sizeof (TestResult)),
  };
  if (ctx.order == NULL || ctx.results == NULL)
    abort ();
  pthread_mutex_init (&ctx.print_lock, NULL);

  for (int r = 0; r < repeat; r++)
    {
      for (int b = 0; b < num_bins; b++)
        {
          int idx = r * num_bins + b;
          ctx.results[idx].binary = argv[first_bin + b];
          ctx.results[idx].iter = r + 1;
        }
    }

  int pos = 0;
  for (int pass = 0; pass <= 8; pass++)
    {
      for (int j = 0; j < count; j++)
        {
          int r = priority_rank (ctx.results[j].binary);
          if ((pass < 8 && r == pass) || (pass == 8 && r >= 8))
            ctx.order[pos++] = j;
        }
    }

  long nproc = sysconf (_SC_NPROCESSORS_ONLN);
  int jobs = (nproc > 0 && nproc <= 64) ? (int) nproc : 4;
  if (jobs > count)
    jobs = count;

  pthread_t *threads = calloc ((size_t) jobs, sizeof (pthread_t));
  if (threads == NULL)
    abort ();
  for (int j = 0; j < jobs; j++)
    pthread_create (&threads[j], NULL, worker_main, &ctx);
  for (int j = 0; j < jobs; j++)
    pthread_join (threads[j], NULL);
  free (threads);
  pthread_mutex_destroy (&ctx.print_lock);

  int passed = 0;
  int skipped = 0;
  int failed = 0;
  int not_run = 0;
  int exit_code = 0;

  for (int j = 0; j < count; j++)
    {
      TestResult *res = &ctx.results[j];
      if (!res->ran)
        {
          not_run++;
          continue;
        }
      if (res->status == 77)
        skipped++;
      else if (res->status != 0)
        {
          failed++;
          if (exit_code == 0)
            exit_code = res->status;
        }
      else
        {
          bool is_tap = false;
          int tap_pass = 0;
          int tap_skip = 0;
          count_tap (res->output, &is_tap, &tap_pass, &tap_skip);
          passed += is_tap ? (tap_pass - tap_skip) : 1;
          skipped += is_tap ? tap_skip : 0;
        }
    }

  if (failed > 0)
    {
      printf ("\n%s--- Failure details ---%s\n", ctx.c_red, ctx.c_reset);
      for (int j = 0; j < count; j++)
        {
          TestResult *res = &ctx.results[j];
          if (!res->ran || res->status == 0 || res->status == 77)
            continue;

          char label[160];
          format_label (res, repeat, label, sizeof (label));
          if (res->timed_out)
            printf ("\n%sFAIL%s  %s %s(timed out after %ds)%s\n",
                    ctx.c_red, ctx.c_reset, label, ctx.c_dim,
                    timeout_sec, ctx.c_reset);
          else
            printf ("\n%sFAIL%s  %s %s(exit %d, %.2fs)%s\n",
                    ctx.c_red, ctx.c_reset, label, ctx.c_dim,
                    res->status, res->elapsed_sec, ctx.c_reset);

          if (!verbose && res->output_len > 0)
            {
              bool is_tap = false;
              int tap_pass = 0;
              int tap_skip = 0;
              count_tap (res->output, &is_tap, &tap_pass, &tap_skip);
              print_failure_output (&ctx, res->output, is_tap);
            }

          if (res->work_dir != NULL)
            printf ("%sWorkdir:%s  %s\n", ctx.c_dim, ctx.c_reset, res->work_dir);

          char seed[64];
          extract_tap_seed (res->output, seed, sizeof (seed));
          const char *name = base_name (res->binary);
          printf ("%sReproduce:%s\n", ctx.c_dim, ctx.c_reset);
          printf ("  make %s TESTS=%s%s%s\n",
                  is_debug ? "test-debug" : "test",
                  name,
                  case_pattern != NULL ? " CASE=" : "",
                  case_pattern != NULL ? case_pattern : "");
          printf ("  %sGSETTINGS_SCHEMA_DIR=%s GSETTINGS_BACKEND=memory "
                  "NUVASHALAUNCHER_TEST_SECRET_STORE=1 NUVASHALAUNCHER_TEST_FAST_TIMERS=1 "
                  "%s%s%s%s%s%s\n",
                  is_debug
                    ? "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 "
                      "LSAN_OPTIONS=detect_leaks=1:exitcode=1:suppressions=\"tests/gtk-lsan.supp\" "
                    : "",
                  schema_dir,
                  res->binary[0] == '/' ? "" : "./",
                  res->binary,
                  case_pattern != NULL ? " -p " : "",
                  case_pattern != NULL ? case_pattern : "",
                  seed[0] != '\0' ? " --seed=" : "",
                  seed);
        }
    }

  if (keep_tmp)
    printf ("%sTemp dir:%s %s\n", ctx.c_dim, ctx.c_reset, root_tmp);
  else if (failed > 0)
    {
      for (int j = 0; j < count; j++)
        {
          TestResult *res = &ctx.results[j];
          if (res->work_dir != NULL && (res->status == 0 || res->status == 77))
            rm_rf (res->work_dir);
        }
    }
  else
    rm_rf (root_tmp);

  double total_sec = monotonic_sec () - suite_t0;
  if (exit_code == 0)
    printf ("%sPASS %d test(s), SKIP %d test(s)%s %s(%.2fs)%s\n",
            ctx.c_green, passed, skipped, ctx.c_reset,
            ctx.c_dim, total_sec, ctx.c_reset);
  else if (not_run > 0)
    printf ("%sFAIL %d binary(ies)%s, %d passed, %d skipped, %d not run (--fail-fast) %s(%.2fs)%s\n",
            ctx.c_red, failed, ctx.c_reset, passed, skipped, not_run,
            ctx.c_dim, total_sec, ctx.c_reset);
  else
    printf ("%sFAIL %d binary(ies)%s, %d passed, %d skipped %s(%.2fs)%s\n",
            ctx.c_red, failed, ctx.c_reset, passed, skipped,
            ctx.c_dim, total_sec, ctx.c_reset);

  for (int j = 0; j < count; j++)
    {
      free (ctx.results[j].output);
      free (ctx.results[j].work_dir);
    }
  free (ctx.order);
  free (ctx.results);
  return exit_code;
}
