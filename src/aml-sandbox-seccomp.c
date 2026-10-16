/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE

#include "aml-sandbox-seccomp.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#define AML_SECCOMP_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define AML_SECCOMP_ARCH AUDIT_ARCH_AARCH64
#elif defined(__i386__)
#define AML_SECCOMP_ARCH AUDIT_ARCH_I386
#elif defined(__arm__)
#define AML_SECCOMP_ARCH AUDIT_ARCH_ARM
#else
#error "AML seccomp needs an architecture audit identifier"
#endif

static gboolean
append_block (struct sock_filter *filter, guint *length, int syscall_number)
{
  if (syscall_number < 0)
    return TRUE;

  filter[(*length)++] = (struct sock_filter) BPF_JUMP (
    BPF_JMP | BPF_JEQ | BPF_K, (guint) syscall_number, 0, 1);
  filter[(*length)++] = (struct sock_filter) BPF_STMT (
    BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
  return TRUE;
}

int
aml_sandbox_seccomp_open (GError **error)
{
  struct sock_filter filter[64];
  guint length = 0;
  int pipe_fds[2] = { -1, -1 };
  const char *bytes;
  gsize remaining;
  const int blocked[] = {
#ifdef __NR_ptrace
    __NR_ptrace,
#endif
#ifdef __NR_mount
    __NR_mount,
#endif
#ifdef __NR_umount2
    __NR_umount2,
#endif
#ifdef __NR_pivot_root
    __NR_pivot_root,
#endif
#ifdef __NR_setns
    __NR_setns,
#endif
#ifdef __NR_unshare
    __NR_unshare,
#endif
#ifdef __NR_bpf
    __NR_bpf,
#endif
#ifdef __NR_perf_event_open
    __NR_perf_event_open,
#endif
#ifdef __NR_open_by_handle_at
    __NR_open_by_handle_at,
#endif
#ifdef __NR_name_to_handle_at
    __NR_name_to_handle_at,
#endif
#ifdef __NR_kexec_load
    __NR_kexec_load,
#endif
#ifdef __NR_kexec_file_load
    __NR_kexec_file_load,
#endif
#ifdef __NR_init_module
    __NR_init_module,
#endif
#ifdef __NR_finit_module
    __NR_finit_module,
#endif
#ifdef __NR_delete_module
    __NR_delete_module,
#endif
#ifdef __NR_reboot
    __NR_reboot,
#endif
#ifdef __NR_swapon
    __NR_swapon,
#endif
#ifdef __NR_swapoff
    __NR_swapoff,
#endif
#ifdef __NR_keyctl
    __NR_keyctl,
#endif
#ifdef __NR_add_key
    __NR_add_key,
#endif
#ifdef __NR_request_key
    __NR_request_key,
#endif
  };

  filter[length++] = (struct sock_filter) BPF_STMT (
    BPF_LD | BPF_W | BPF_ABS, offsetof (struct seccomp_data, arch));
  filter[length++] = (struct sock_filter) BPF_JUMP (
    BPF_JMP | BPF_JEQ | BPF_K, AML_SECCOMP_ARCH, 1, 0);
  filter[length++] = (struct sock_filter) BPF_STMT (
    BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
  filter[length++] = (struct sock_filter) BPF_STMT (
    BPF_LD | BPF_W | BPF_ABS, offsetof (struct seccomp_data, nr));

  for (gsize i = 0; i < G_N_ELEMENTS (blocked); i++)
    append_block (filter, &length, blocked[i]);

  filter[length++] = (struct sock_filter) BPF_STMT (
    BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  if (pipe2 (pipe_fds, O_CLOEXEC) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "cannot create the sandbox seccomp descriptor: %s",
                   g_strerror (errno));
      return -1;
    }

  bytes = (const char *) filter;
  remaining = length * sizeof filter[0];
  while (remaining > 0)
    {
      ssize_t written = write (pipe_fds[1], bytes, remaining);

      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        {
          int saved_errno = errno != 0 ? errno : EIO;
          close (pipe_fds[0]);
          close (pipe_fds[1]);
          g_set_error (error, G_FILE_ERROR,
                       g_file_error_from_errno (saved_errno),
                       "cannot prepare the sandbox seccomp descriptor: %s",
                       g_strerror (saved_errno));
          return -1;
        }
      bytes += written;
      remaining -= (gsize) written;
    }

  /* Bubblewrap consumes the raw classic-BPF instructions from an fd. The
   * pipe contains the program bytes and stays open until bwrap has loaded it. */
  close (pipe_fds[1]);
  return pipe_fds[0];
}
