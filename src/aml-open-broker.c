/* The SDL entry point used by Minecraft's "Open Folder" action.
 * This library is loaded only inside the game sandbox. */
#define _GNU_SOURCE

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static bool
broker_request (const char *url)
{
  const char *path = url;
  size_t length;

  if (path == NULL)
    return false;

  if (strncmp (path, "file:", 5) == 0)
    {
      path += 5;
      while (strncmp (path, "//", 2) == 0)
        path++;
    }

  if (strncmp (path, "/instance/", 10) != 0 || strstr (path, "..") != NULL)
    return false;

  length = strlen (path);
  if (write (4, path, length) == (ssize_t) length && write (4, "\n", 1) == 1)
    return true;

  {
    int fd = open ("/instance/.aml-runtime/open-request",
                   O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    bool ok = false;

    if (fd >= 0)
      {
        ok = write (fd, path, length) == (ssize_t) length &&
             write (fd, "\n", 1) == 1;
        close (fd);
      }
    return ok;
  }
}

static bool
is_xdg_open (const char *file)
{
  const char *base = strrchr (file != NULL ? file : "", '/');
  const char *name = file != NULL ? (base != NULL ? base + 1 : file) : "";

  return strcmp (name, "xdg-open") == 0;
}

static const char *
open_argument (char *const argv[])
{
  if (argv == NULL)
    return NULL;

  /* xdg-open accepts one URI. SDL puts it in argv[1]. */
  return argv[1];
}

static void
resolve_symbol (void *address, void *target, size_t target_size)
{
  /* POSIX specifies dlsym() as returning an address that may designate a
   * function. memcpy avoids the ISO C diagnostic for converting that address
   * to a function pointer while preserving the platform representation. */
  memcpy (target, &address, target_size);
}

bool
SDL_OpenURL (const char *url)
{
  return broker_request (url);
}

typedef int (*PosixSpawnFunction) (pid_t *, const char *, const posix_spawn_file_actions_t *,
                                   const posix_spawnattr_t *, char *const[], char *const[]);
typedef int (*ExecFunction) (const char *, char *const[], char *const[]);

static int
spawn_broker (pid_t *pid, char *const argv[])
{
  pid_t child = fork ();

  if (child < 0)
    return errno;
  if (child == 0)
    {
      broker_request (open_argument (argv));
      _exit (0);
    }

  if (pid != NULL)
    *pid = child;
  return 0;
}

int
posix_spawnp (pid_t *pid, const char *file,
              const posix_spawn_file_actions_t *actions,
              const posix_spawnattr_t *attributes, char *const argv[],
              char *const envp[])
{
  static PosixSpawnFunction real_spawnp;

  (void) actions;
  (void) attributes;
  (void) envp;
  if (is_xdg_open (file))
    return spawn_broker (pid, argv);

  if (real_spawnp == NULL)
    resolve_symbol (dlsym (RTLD_NEXT, "posix_spawnp"), &real_spawnp,
                    sizeof real_spawnp);
  return real_spawnp (pid, file, actions, attributes, argv, envp);
}

int
posix_spawn (pid_t *pid, const char *file,
             const posix_spawn_file_actions_t *actions,
             const posix_spawnattr_t *attributes, char *const argv[],
             char *const envp[])
{
  static PosixSpawnFunction real_spawn;

  (void) envp;
  if (is_xdg_open (file))
    return spawn_broker (pid, argv);

  if (real_spawn == NULL)
    resolve_symbol (dlsym (RTLD_NEXT, "posix_spawn"), &real_spawn,
                    sizeof real_spawn);
  return real_spawn (pid, file, actions, attributes, argv, envp);
}

int
execve (const char *file, char *const argv[], char *const envp[])
{
  static ExecFunction real_execve;

  if (is_xdg_open (file))
    {
      broker_request (open_argument (argv));
      errno = 0;
      _exit (0);
    }

  if (real_execve == NULL)
    resolve_symbol (dlsym (RTLD_NEXT, "execve"), &real_execve,
                    sizeof real_execve);
  return real_execve (file, argv, envp);
}

int
execvp (const char *file, char *const argv[])
{
  static int (*real_execvp) (const char *, char *const[]);

  if (is_xdg_open (file))
    {
      broker_request (open_argument (argv));
      errno = 0;
      _exit (0);
    }

  if (real_execvp == NULL)
    resolve_symbol (dlsym (RTLD_NEXT, "execvp"), &real_execvp,
                    sizeof real_execvp);
  return real_execvp (file, argv);
}
