/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* ComfyJava: a usable Java runtime on this machine, and how to find it.
 *
 * Detection runs the well-known locations of a JDK on Linux and asks
 * each candidate executable what it is, so nothing here hard-codes a
 * version or a path. Parsing the output of `java -version` is plain
 * synchronous work; probing a candidate is a GSubprocess on the main
 * context. */

#ifndef comfy_JAVA_H
#define comfy_JAVA_H

#include <gio/gio.h>

#include "comfy-task.h"

G_BEGIN_DECLS

#define comfy_JAVA_ERROR (comfy_java_error_quark ())

typedef enum {
  comfy_JAVA_ERROR_IO,   /* the subprocess could not be run or read */
  comfy_JAVA_ERROR_PARSE /* the output was not a version report */
} ComfyJavaError;

GQuark comfy_java_error_quark (void);

/* One Java runtime. @executable is the path that was probed, @arch the
 * os.arch of that JVM ("64-bit"/"32-bit" when only the -version
 * banner could be read), @vendor what `java -version` reports, e.g.
 * "OpenJDK". @headless is TRUE when the JVM is missing AWT's X11
 * bridge and cannot run a game. */
typedef struct {
  char *executable;
  char *arch;
  char *vendor;
  int major_version; /* 8, 17, 21, 25, ... */
  gboolean headless;
} ComfyJava;

void comfy_java_free (ComfyJava *java);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ComfyJava, comfy_java_free)

/* FALSE when this Java is older than the one the version document
 * wants (its javaVersion.majorVersion). */
gboolean comfy_java_is_compatible (const ComfyJava *java, int required_major);

/* The argument of `java -version`, straight from the executable:
 * "1.8.0_x" is 8, "17.0.x" is 17, "21" is 21. Works on the banner
 * alone and on the full -XshowSettings:properties output when that is
 * what is given. Returns FALSE when there is nothing to parse. */
gboolean comfy_java_parse_version (const char  *output,
                                 gssize       length,
                                 int         *major_version,
                                 char       **architecture,
                                 char       **vendor);

/* The closest Java that still satisfies the instance: the compatible
 * one with the smallest major version, NULL when none fits. A runtime
 * that can open a window beats a headless one, even a newer major,
 * because a headless runtime cannot show the game at all. */
ComfyJava *comfy_java_best_compatible (GPtrArray *javas, int required_major);

/* Picks the Java an instance will run with: the instance setting wins,
 * and when it names none the exact required major is selected when it is
 * detected. There is no automatic fallback to another major, and no global
 * default: a runtime is the instance's own choice.
 * @javas holds what detection found; nothing here requires every
 * candidate to be in it. */
const ComfyJava *comfy_java_select (GPtrArray *javas,
                                const char *instance_java_path,
                                int         required_major);

/* Runs "<executable> -version" (stderr is merged into stdout, the
 * flags Go answers) and parses what comes back. */
ComfyTaskScope *comfy_java_probe_async (const char *executable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

/* (transfer full). Errors from the spawn, from comfy_JAVA_ERROR and
 * G_IO_ERROR_CANCELLED for a cancelled caller. */
ComfyJava *comfy_java_probe_finish (GAsyncResult  *result,
                                GError       **error);

/* One async probe per candidate, skipping the ones that do not report a
 * version, deduplicated by the resolved real path of @executables
 * (elements are stolen into the result in the order they were probed).
 * @executables is NULL-terminated. */
ComfyTaskScope *comfy_java_probe_list_async (char **executables,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/* (transfer full). The array holds ComfyJava* with comfy_java_free() as
 * the element and the array free function, so
 * g_ptr_array_unref() drops everything. */
GPtrArray *comfy_java_probe_list_finish (GAsyncResult  *result,
                                       GError       **error);

/* Probes every candidate Linux has at hand: bin/java and
 * jre/bin/java inside each entry of the default search roots
 * (/usr/lib/jvm, /usr/lib64/jvm, /usr/java, /usr/local/java, /opt,
 * ~/.jdks, ~/.sdkman/candidates/java, ~/.asdf/installs/java,
 * ~/.local/share/mise/installs/java), $JAVA_HOME/bin/java and the
 * "java" on $PATH. Same result shape as comfy_java_probe_list_finish(). */
ComfyTaskScope *comfy_java_detect_async (GAsyncReadyCallback callback,
                                     gpointer user_data);

GPtrArray *comfy_java_detect_finish (GAsyncResult  *result,
                                   GError       **error);

/* Same as comfy_java_detect_async(), but over caller-given roots only:
 * for each root every subdirectory is treated as a possible JVM home
 * with both <home>/bin/java and <home>/jre/bin/java checked. This is
 * what a headless test uses to scan fake VMs in a temp directory.
 * Unlike comfy_java_detect_async() it never reaches $JAVA_HOME, $PATH
 * or anything outside the given roots. */
ComfyTaskScope *comfy_java_detect_list_async (char **roots,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

G_END_DECLS

#endif /* comfy_JAVA_H */
