/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Tests for aml-java.
 *
 * String parsing is pure; the GSubprocess machinery is covered with
 * fake "java" shell scripts in a temporary directory that print one of
 * the known version report shapes. Everything runs headless. */

/* symlink(), like realpath(), only exists under glibc's defaults. */
#define _GNU_SOURCE

#include "aml-java.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------- *
 * The same async wait the download tests use
 * ---------------------------------------------------------- */

typedef struct
{
  GMainLoop *loop;
  guint watchdog;
  gboolean timed_out;
} AsyncRun;

static gboolean
async_timeout (gpointer user_data)
{
  AsyncRun *run = user_data;

  run->timed_out = TRUE;
  g_main_loop_quit (run->loop);

  return G_SOURCE_REMOVE;
}

static void
run_one (AsyncRun *run)
{
  run->loop = g_main_loop_new (NULL, FALSE);
  run->watchdog = g_timeout_add_seconds (30, async_timeout, run);
  g_main_loop_run (run->loop);
  if (!run->timed_out)
    g_source_remove (run->watchdog);
  g_assert_false (run->timed_out);
  g_main_loop_unref (run->loop);
  run->loop = NULL;
}

/* ---------------------------------------------------------- *
 * Known version reports, the shapes OpenJDK, Temurin and the
 * support-era VMs publish
 * ---------------------------------------------------------- */

static const char *report_openjdk_25 =
  "openjdk version \"25.0.4.1\" 2026-08-18\n"
  "OpenJDK Runtime Environment (build 25.0.4.1)\n"
  "OpenJDK 64-Bit Server VM (build 25.0.4.1, mixed mode, sharing)\n";

static const char *report_openjdk_21 =
  "openjdk version \"21.0.3\" 2024-04-16 LTS\n"
  "OpenJDK Runtime Environment (build 21.0.3+9-LTS)\n"
  "OpenJDK 64-Bit Server VM (build 21.0.3+9-LTS, mixed mode, sharing)\n";

static const char *report_openjdk_17 =
  "openjdk version \"17.0.9\" 2023-10-17 LTS\n"
  "OpenJDK Runtime Environment (build 17.0.9+9-LTS)\n"
  "OpenJDK 64-Bit Server VM (build 17.0.9+9-LTS, mixed mode, sharing)\n";

static const char *report_oracle_18 =
  "java version \"1.8.0_382\"\n"
  "Java(TM) SE Runtime Environment (build 1.8.0_382-b05)\n"
  "Java HotSpot(TM) 64-Bit Server VM (build 25.382-b05, mixed mode)\n";

static const char *report_with_settings =
  "Property settings:\n"
  "    file.encoding = UTF-8\n"
  "    java.home = /usr/lib/jvm/java-25-openjdk\n"
  "    os.arch = amd64\n"
  "    os.version = 7.2.7-arch1-1\n"
  "\n"
  "openjdk version \"21.0.3\" 2024-04-16\n"
  "OpenJDK Runtime Environment (build 21.0.3+9)\n"
  "OpenJDK 64-Bit Server VM (build 21.0.3+9, mixed mode, sharing)\n";

static const char *report_adoptium =
  "openjdk version \"17.0.11\" 2024-04-16 LTS\n"
  "Eclipse Temurin 17 (build 17.0.11+9-LTS)\n"
  "Eclipse OpenJ9 64-Bit Server VM (build 17.0.11+9-LTS, mixed, sharing)\n";

static void
test_parse_banner_modern (void)
{
  g_autofree char *arch = NULL;
  g_autofree char *vendor = NULL;
  int major = 0;

  g_assert_true (aml_java_parse_version (report_openjdk_25, -1, &major,
                                         &arch, &vendor));
  g_assert_cmpint (major, ==, 25);
  g_assert_cmpstr (arch, ==, "64-bit");
  g_assert_cmpstr (vendor, ==, "OpenJDK");

  g_clear_pointer (&arch, g_free);
  g_clear_pointer (&vendor, g_free);
  g_assert_true (aml_java_parse_version (report_openjdk_21, -1, &major,
                                         &arch, &vendor));
  g_assert_cmpint (major, ==, 21);

  g_clear_pointer (&arch, g_free);
  g_clear_pointer (&vendor, g_free);
  g_assert_true (aml_java_parse_version (report_openjdk_17, -1, &major,
                                         &arch, &vendor));
  g_assert_cmpint (major, ==, 17);
}

static void
test_parse_banner_legacy_1_8 (void)
{
  g_autofree char *arch = NULL;
  g_autofree char *vendor = NULL;
  int major = 0;

  g_assert_true (aml_java_parse_version (report_oracle_18, -1, &major,
                                         &arch, &vendor));
  g_assert_cmpint (major, ==, 8);
  g_assert_cmpstr (arch, ==, "64-bit");
  g_assert_cmpstr (vendor, ==, "Java(TM) SE");
}

static void
test_parse_show_settings_picks_os_arch (void)
{
  g_autofree char *arch = NULL;
  g_autofree char *vendor = NULL;
  int major = 0;

  g_assert_true (aml_java_parse_version (report_with_settings, -1, &major,
                                         &arch, &vendor));
  g_assert_cmpint (major, ==, 21);
  g_assert_cmpstr (arch, ==, "amd64");
  g_assert_cmpstr (vendor, ==, "OpenJDK");
}

static void
test_parse_odd_reports_still_parse (void)
{
  g_autofree char *arch = NULL;
  g_autofree char *vendor = NULL;
  int major = 0;

  /* No "Runtime Environment" phrasing at all: the parser keeps what
   * it has instead of failing. */
  g_assert_true (aml_java_parse_version (report_adoptium, -1, &major,
                                         &arch, &vendor));
  g_assert_cmpint (major, ==, 17);
  g_assert_null (vendor);

  g_assert_false (aml_java_parse_version ("hello\nworld\n", -1, &major,
                                          &arch, &vendor));
  g_assert_false (aml_java_parse_version ("", -1, &major, &arch, &vendor));
  g_assert_false (aml_java_parse_version ("java version \"\"\n", -1, &major,
                                          &arch, &vendor));
}

/* ---------------------------------------------------------- *
 * Compatibility and selection
 * ---------------------------------------------------------- */

static GPtrArray *
fake_java_list (void)
{
  GPtrArray *list = g_ptr_array_new_with_free_func (
    (GDestroyNotify) aml_java_free);
  int majors[] = { 8, 17, 21, 25 };

  for (guint i = 0; i < G_N_ELEMENTS (majors); i++)
    {
      AmlJava *java = g_new0 (AmlJava, 1);

      java->executable =
        g_strdup_printf ("/fake/jdk/%d/bin/java", majors[i]);
      java->vendor = g_strdup ("OpenJDK");
      java->arch = g_strdup ("amd64");
      java->major_version = majors[i];
      g_ptr_array_add (list, java);
    }

  return list;
}

static void
test_best_compatible_prefers_closest (void)
{
  g_autoptr (GPtrArray) list = fake_java_list ();
  AmlJava *best;

  best = aml_java_best_compatible (list, 21);
  g_assert_nonnull (best);
  g_assert_cmpint (best->major_version, ==, 21);

  best = aml_java_best_compatible (list, 9);
  g_assert_nonnull (best);
  g_assert_cmpint (best->major_version, ==, 17);

  best = aml_java_best_compatible (list, 8);
  g_assert_nonnull (best);
  g_assert_cmpint (best->major_version, ==, 8);

  /* Newer than everything installed: there is no compatible Java. */
  g_assert_null (aml_java_best_compatible (list, 26));
}

static void
test_compatibility_rule (void)
{
  g_autoptr (GPtrArray) list = fake_java_list ();
  AmlJava *java = g_ptr_array_index (list, 0); /* major 8 */

  g_assert_true (aml_java_is_compatible (java, 8));
  g_assert_true (aml_java_is_compatible (java, 1));
  g_assert_false (aml_java_is_compatible (java, 17));
}

static void
test_select_instance_overrides_global (void)
{
  g_autoptr (GPtrArray) list = fake_java_list ();
  const AmlJava *picked;

  picked = aml_java_select (list, "/fake/jdk/25/bin/java", NULL, 8);
  g_assert_nonnull (picked);
  g_assert_cmpint (picked->major_version, ==, 25);

  picked = aml_java_select (list, NULL, "/fake/jdk/17/bin/java", 8);
  g_assert_nonnull (picked);
  g_assert_cmpint (picked->major_version, ==, 17);

  picked =
    aml_java_select (list, "/fake/jdk/25/bin/java", "/fake/jdk/17/bin/java", 8);
  g_assert_cmpint (picked->major_version, ==, 25);

  /* Nothing set: the closest compatible one wins. */
  picked = aml_java_select (list, NULL, NULL, 21);
  g_assert_nonnull (picked);
  g_assert_cmpint (picked->major_version, ==, 21);

  /* An instance path detection never saw is not something this module
   * can fake: selection answers nothing. */
  g_assert_null (aml_java_select (list, "/elsewhere/bin/java", NULL, 8));
  g_assert_null (aml_java_select (list, NULL, NULL, 30));
}

/* ---------------------------------------------------------- *
 * Fake executables that print a known report
 * ---------------------------------------------------------- */

static char *
fake_java_dir (void)
{
  g_autofree char *dir = g_dir_make_tmp ("aml-test-java-XXXXXX", NULL);

  return g_steal_pointer (&dir);
}

static char *
write_fake_java (const char *dir,
                 const char *name,
                 const char *report)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autofree char *script = NULL;

  script = g_strdup_printf ("#!/bin/sh\n"
                            "cat <<'AML_REPORT'\n"
                            "%s"
                            "AML_REPORT\n",
                            report);

  if (!g_file_set_contents (path, script, -1, NULL) ||
      g_chmod (path, 0755) != 0)
    g_error ("the fake java at \"%s\" cannot be written", path);

  return g_steal_pointer (&path);
}

typedef struct
{
  AsyncRun run;
  GPtrArray *javas;
  GError *error;
  AmlJava *single;
} JavaCall;

static void
list_finished (GObject *source G_GNUC_UNUSED,
               GAsyncResult *result,
               gpointer user_data)
{
  JavaCall *call = user_data;

  call->javas = aml_java_probe_list_finish (result, &call->error);
  g_main_loop_quit (call->run.loop);
}

static void
single_finished (GObject *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer user_data)
{
  JavaCall *call = user_data;

  call->single = aml_java_probe_finish (result, &call->error);
  g_main_loop_quit (call->run.loop);
}

static void
test_probing_fake_scripts_and_dedup (void)
{
  g_autofree char *dir = fake_java_dir ();
  g_autofree char *script8 = write_fake_java (dir, "java-8",
                                              report_oracle_18);
  g_autofree char *script17 = write_fake_java (dir, "java-17",
                                               report_openjdk_17);
  g_autofree char *link8 = g_build_filename (dir, "java-8-link", NULL);
  char *executables[5];
  JavaCall call = { 0 };
  guint found8 = 0;
  guint found17 = 0;

  symlink (script8, link8);

  /* script8 and its symlink are the same real path, so only one of
   * them can be listed; a script exiting badly is skipped. */
  executables[0] = script8;
  executables[1] = link8;
  executables[2] = script17;
  executables[3] = "/definitely/not/a/java";
  executables[4] = NULL;

  call.javas = NULL;
  aml_java_probe_list_async (g_strdupv (executables), NULL, list_finished,
                             &call);
  run_one (&call.run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.javas);
  g_assert_cmpuint (call.javas->len, ==, 2);

  for (guint i = 0; i < call.javas->len; i++)
    {
      AmlJava *java = g_ptr_array_index (call.javas, i);

      if (java->major_version == 8)
        found8++;
      else if (java->major_version == 17)
        found17++;
    }

  g_assert_cmpuint (found8, ==, 1);
  g_assert_cmpuint (found17, ==, 1);
  g_ptr_array_free (call.javas, TRUE);
}

static void
test_probing_parses_the_report (void)
{
  g_autofree char *dir = fake_java_dir ();
  g_autofree char *script = write_fake_java (dir, "java-show-settings",
                                             report_with_settings);
  JavaCall call = { 0 };

  aml_java_probe_async (script, NULL, single_finished, &call);
  run_one (&call.run);

  g_assert_no_error (call.error);
  g_assert_nonnull (call.single);
  g_assert_cmpint (call.single->major_version, ==, 21);
  g_assert_cmpstr (call.single->arch, ==, "amd64");
  g_assert_cmpstr (call.single->vendor, ==, "OpenJDK");
  g_assert_cmpstr (call.single->executable, ==, script);

  aml_java_free (call.single);
}

int
main (void)
{
  g_print ("# Java detection tests\n");

  test_parse_banner_modern ();
  g_print ("ok - modern banners parse like 25, 21 and 17\n");

  test_parse_banner_legacy_1_8 ();
  g_print ("ok - the 1.8 banner is treated as major 8\n");

  test_parse_show_settings_picks_os_arch ();
  g_print ("ok - os.arch wins over the 64-Bit line when both are there\n");

  test_parse_odd_reports_still_parse ();
  g_print ("ok - odd vendors parse and garbage does not\n");

  test_compatibility_rule ();
  g_print ("ok - compatible means major >= the required one\n");

  test_best_compatible_prefers_closest ();
  g_print ("ok - the closest compatible Java is preferred\n");

  test_select_instance_overrides_global ();
  g_print ("ok - instance, global then auto, in that order\n");

  test_probing_fake_scripts_and_dedup ();
  g_print ("ok - siblings and symlinks probe once, dead paths are skipped\n");

  test_probing_parses_the_report ();
  g_print ("ok - probing a script parses its known report\n");

  return 0;
}
