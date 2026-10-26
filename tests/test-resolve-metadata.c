#include "test-resolve-common.h"

static void
test_maven_paths (void)
{
  g_autofree char *plain = nuvashalauncher_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3");
  g_autofree char *classified =
    nuvashalauncher_resolve_maven_path ("com.mojang:jtracy:1.14.38:natives-linux");
  g_autofree char *zip = nuvashalauncher_resolve_maven_path (
      "de.oceanlabs.mcp:mcp_config:1.21-20240808.103037@zip");
  g_autofree char *added =
    nuvashalauncher_resolve_maven_path_classified ("org.lwjgl.lwjgl:lwjgl-platform:2.9.0",
                                       "natives-linux");
  g_autofree char *with_extension = nuvashalauncher_resolve_maven_path_classified (
      "org.lwjgl:lwjgl:3.3.3", "natives-linux@zip");
  g_autofree char *kept = nuvashalauncher_resolve_maven_path_classified (
      "com.mojang:jtracy:1.14.38:natives-linux", "natives-windows");

  g_assert_cmpstr (plain, ==, "org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar");
  g_assert_cmpstr (classified, ==,
                   "com/mojang/jtracy/1.14.38/jtracy-1.14.38-natives-linux.jar");
  g_assert_cmpstr (zip, ==,
                   "de/oceanlabs/mcp/mcp_config/1.21-20240808.103037/"
                   "mcp_config-1.21-20240808.103037.zip");
  g_assert_cmpstr (added, ==,
                   "org/lwjgl/lwjgl/lwjgl-platform/2.9.0/"
                   "lwjgl-platform-2.9.0-natives-linux.jar");
  g_assert_cmpstr (with_extension, ==,
                   "org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3-natives-linux.zip");
  /* A coordinate that names a classifier keeps it. */
  g_assert_cmpstr (kept, ==,
                   "com/mojang/jtracy/1.14.38/jtracy-1.14.38-natives-linux.jar");

  /* Nothing that would step out of the libraries directory is a
     coordinate. */
  g_assert_null (nuvashalauncher_resolve_maven_path ("org.lwjgl:lwjgl"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("org.lwjgl::3.3.3"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("org.lwjgl:lwjgl:"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("../evil:lwjgl:3.3.3"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("org.lwjgl:..:3.3.3"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3:../../x"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("org/lwjgl:lwjgl:3.3.3"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3@a/b"));
  g_assert_null (nuvashalauncher_resolve_maven_path ("org.lwjgl:lwjgl:3.3.3:natives/../x"));
  g_assert_null (nuvashalauncher_resolve_maven_path (NULL));

  g_print ("ok - maven paths are built from coordinates\n");
}

static void
test_entry_names (void)
{
  g_assert_true (nuvashalauncher_resolve_entry_name_is_safe ("liblwjgl.so"));
  g_assert_true (nuvashalauncher_resolve_entry_name_is_safe ("org/lwjgl/liblwjgl.so"));
  g_assert_true (nuvashalauncher_resolve_entry_name_is_safe ("...weird name..."));

  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe (NULL));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe (""));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe ("/etc/passwd"));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe ("../escape.so"));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe ("a/../../escape.so"));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe ("a/.."));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe ("a//b.so"));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe ("a\\b.so"));
  g_assert_false (nuvashalauncher_resolve_entry_name_is_safe ("a/./b.so"));

  g_print ("ok - only safe entry names are accepted\n");
}

static void
test_native_classifiers (void)
{
  g_autoptr (nuvashalauncherVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (nuvashalauncherVersionDescription) arch = architecture_document ();
  g_autoptr (nuvashalauncherRuleContext) x86 =
    nuvashalauncher_rule_context_new_for ("linux", "x86_64", NULL);
  g_autoptr (nuvashalauncherRuleContext) arm =
    nuvashalauncher_rule_context_new_for ("linux", "aarch64", NULL);
  g_autoptr (nuvashalauncherRuleContext) other =
    nuvashalauncher_rule_context_new_for ("linux", "riscv64", NULL);
  g_autoptr (nuvashalauncherRuleContext) windows =
    nuvashalauncher_rule_context_new_for ("windows", "x86_64", NULL);
  nuvashalauncherLibrary *platform =
    find_library (legacy, "org.lwjgl.lwjgl:lwjgl-platform:2.9.0");
  nuvashalauncherLibrary *scala =
    find_library (legacy, "org.scala-lang:scala-library:2.10.2");
  nuvashalauncherLibrary *per_machine = find_library (arch, "org.lwjgl:lwjgl:3.3.3");

  /* The natives table names the classifier directly. */
  g_assert_cmpstr (nuvashalauncher_resolve_native_classifier (platform, x86), ==,
                   "natives-linux");

  g_assert_cmpstr (nuvashalauncher_resolve_native_classifier (platform, windows), ==,
                   "natives-windows");
  /* A library that publishes no classifier has no natives to pick. */
  g_assert_null (nuvashalauncher_resolve_native_classifier (scala, x86));

  /* One jar per machine: the architecture decides the name, with every
     spelling that is published for it tried. */
  g_assert_cmpstr (nuvashalauncher_resolve_native_classifier (per_machine, x86), ==,
                   "natives-linux-x86_64");
  g_assert_cmpstr (nuvashalauncher_resolve_native_classifier (per_machine, arm), ==,
                   "natives-linux-arm64");

  g_assert_null (nuvashalauncher_resolve_native_classifier (per_machine, other));
  /* The names of another system are not picked up. */
  g_assert_null (nuvashalauncher_resolve_native_classifier (per_machine, windows));

  g_print ("ok - the natives classifier of this machine is chosen\n");
}

static void
test_classifier_is_not_natives (void)
{
  g_autoptr (nuvashalauncherVersionDescription) description = version_from_json (
      "{\"id\": \"x\", \"type\": \"release\", \"libraries\": ["
      " {\"name\": \"io.netty:netty-transport-native-epoll:4.2.16:linux-x86_64\","
      "  \"downloads\": {\"artifact\": {\"path\":"
      "   \"io/netty/epoll/4.2.16/epoll-linux-x86_64.jar\","
      "   \"sha1\": \"aa\", \"url\": \"http://x/epoll.jar\"}}},"
      " {\"name\": \"org.lwjgl:lwjgl:3.4.3:natives-linux\","
      "  \"downloads\": {\"artifact\": {\"path\":"
      "   \"org/lwjgl/lwjgl/3.4.3/lwjgl-3.4.3-natives-linux.jar\","
      "   \"sha1\": \"bb\", \"url\": \"http://x/lwjgl-natives.jar\"}}}]}");
  g_autoptr (nuvashalauncherRuleContext) context =
    nuvashalauncher_rule_context_new_for ("linux", "x86_64", NULL);
  g_autofree char *libraries =
    g_build_filename ("/data", "libraries", NULL);
  g_autofree char *classpath =
    nuvashalauncher_resolve_classpath (description, context, libraries, NULL);
  g_autoptr (GPtrArray) files =
    nuvashalauncher_resolve_library_files (description, context, libraries);
  g_auto (GStrv) entries = NULL;

  g_assert_cmpuint (description->libraries->len, ==, 2);
  g_assert_cmpuint (files->len, ==, 2);

  /* Neither of them is a jar to unpack. */
  g_assert_false (((nuvashalauncherResolveFile *) g_ptr_array_index (files, 0))->natives);
  g_assert_false (((nuvashalauncherResolveFile *) g_ptr_array_index (files, 1))->natives);

  entries = g_strsplit (classpath, ":", -1);
  g_assert_cmpuint (g_strv_length (entries), ==, 2);
  g_assert_cmpstr (entries[0], ==,
                   "/data/libraries/io/netty/epoll/4.2.16/"
                   "epoll-linux-x86_64.jar");
  g_assert_cmpstr (entries[1], ==,
                   "/data/libraries/org/lwjgl/lwjgl/3.4.3/"
                   "lwjgl-3.4.3-natives-linux.jar");

  g_print ("ok - a coordinate classifier does not make a jar one to unpack\n");
}

static void
test_rule_filtering (void)
{
  g_autoptr (nuvashalauncherVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (nuvashalauncherRuleContext) linux =
    nuvashalauncher_rule_context_new_for ("linux", "x86_64", NULL);
  g_autoptr (nuvashalauncherRuleContext) mac =
    nuvashalauncher_rule_context_new_for ("osx", "x86_64", NULL);
  nuvashalauncherLibrary *windows_only =
    find_library (legacy, "com.example:windows-only:1.0");
  nuvashalauncherLibrary *linux_only = find_library (legacy, "com.example:linux-only:1.0");
  nuvashalauncherLibrary *scala =
    find_library (legacy, "org.scala-lang:scala-library:2.10.2");
  g_autofree char *libraries =
    g_build_filename ("/data", "libraries", NULL);
  g_autoptr (GPtrArray) files = NULL;

  /* A rule that names another system does not match, and when no rule
     matches the library is out. */
  g_assert_false (nuvashalauncher_library_allowed (windows_only, linux));
  g_assert_false (nuvashalauncher_library_allowed (windows_only, mac));
  g_assert_true (nuvashalauncher_library_allowed (linux_only, linux));
  g_assert_false (nuvashalauncher_library_allowed (linux_only, mac));
  /* No rules at all means allowed everywhere. */
  g_assert_true (nuvashalauncher_library_allowed (scala, mac));

  /* Only the libraries that apply here are planned, and a library that
     names no jar of its own contributes nothing. */
  files = nuvashalauncher_resolve_library_files (legacy, linux, libraries);
  g_assert_nonnull (files);
  g_assert_cmpuint (files->len, ==, 2); /* the scala jar, the natives jar */

  for (guint i = 0; i < files->len; i++)
    {
      const nuvashalauncherResolveFile *file = g_ptr_array_index (files, i);

      g_assert_nonnull (file->url);
      g_assert_nonnull (file->path);
      g_assert_true (g_str_has_prefix (file->path, "/data/libraries/"));
      g_assert_null (strstr (file->path, "windows-only"));
      g_assert_null (strstr (file->path, "linux-only"));
    }

  /* The one natives jar of the document is marked as one, and it is the
     first library of the document that has a jar at all. */
  {
    const nuvashalauncherResolveFile *file = g_ptr_array_index (files, 0);

    g_assert_true (file->natives);
    g_assert_cmpstr (g_ptr_array_index (file->exclude, 0), ==, "META-INF/");
    g_assert_false (((nuvashalauncherResolveFile *) g_ptr_array_index (files, 1))->natives);
  }

  g_print ("ok - rules decide which libraries this machine uses\n");
}

static void
test_classpath_order (void)
{
  g_autoptr (nuvashalauncherVersionDescription) modern =
    fixture_version ("version-modern.json");
  g_autoptr (nuvashalauncherVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (nuvashalauncherRuleContext) context =
    nuvashalauncher_rule_context_new_for ("linux", "x86_64", NULL);
  g_autofree char *libraries =
    g_build_filename ("/data", "libraries", NULL);
  g_autofree char *client =
    g_build_filename ("/data", "versions", "1.21.1", "1.21.1.jar", NULL);
  g_autofree char *classpath = NULL;
  g_autofree char *legacy_classpath = NULL;
  g_auto (GStrv) entries = NULL;

  classpath = nuvashalauncher_resolve_classpath (modern, context, libraries, client);
  g_assert_nonnull (classpath);

  entries = g_strsplit (classpath, ":", -1);
  g_assert_cmpuint (g_strv_length (entries), ==, 4);

  /* The libraries of the document, in the order it lists them. The
     library of another system is left out. jtracy is written as a
     library whose coordinate carries the classifier, which since 1.20.5
     is a jar for the classpath: the document tells LWJGL and JNA where
     to unpack the natives they find in it. */
  g_assert_cmpstr (entries[0], ==,
                   "/data/libraries/org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar");
  g_assert_cmpstr (entries[1], ==,
                   "/data/libraries/com/mojang/jtracy/1.14.38/"
                   "jtracy-1.14.38-natives-linux.jar");
  g_assert_cmpstr (entries[2], ==,
                   "/data/libraries/org/lwjgl/lwjgl-glfw/3.3.3/"
                   "lwjgl-glfw-3.3.3.jar");
  g_assert_null (strstr (classpath, "java-objc-bridge"));
  /* The client jar goes last. */
  g_assert_cmpstr (entries[3], ==, client);

  legacy_classpath = nuvashalauncher_resolve_classpath (legacy, context, libraries, client);

  g_strfreev (entries);
  entries = g_strsplit (legacy_classpath, ":", -1);
  g_assert_cmpuint (g_strv_length (entries), ==, 2);
  g_assert_cmpstr (entries[0], ==,
                   "/data/libraries/org/scala-lang/scala-library/2.10.2/"
                   "scala-library-2.10.2.jar");
  g_assert_cmpstr (entries[1], ==, client);

  /* A natives jar is unpacked, not put on the classpath. */
  g_assert_null (strstr (legacy_classpath, "natives-linux"));
  g_assert_null (strstr (legacy_classpath, "windows-only"));
  g_assert_null (strstr (legacy_classpath, "linux-only"));

  g_print ("ok - the classpath holds the libraries in order, client last\n");
}

static void
test_arguments (void)
{
  g_autoptr (nuvashalauncherVersionDescription) modern =
    fixture_version ("version-modern.json");
  g_autoptr (nuvashalauncherVersionDescription) legacy =
    fixture_version ("version-legacy.json");
  g_autoptr (nuvashalauncherRuleContext) context =
    nuvashalauncher_rule_context_new_for ("linux", "x86_64", NULL);
  g_autoptr (GPtrArray) jvm = nuvashalauncher_resolve_jvm_arguments (modern, context);
  g_autoptr (GPtrArray) game = nuvashalauncher_resolve_game_arguments (modern, context);
  g_autoptr (GPtrArray) old_game =
    nuvashalauncher_resolve_game_arguments (legacy, context);

  /* The jvm arguments of this machine: the plain ones plus the one that
     asks for a bigger stack. The ones for macOS, for Windows and for a
     demo account are left out. */
  g_assert_cmpuint (jvm->len, ==, 4);
  g_assert_cmpstr (g_ptr_array_index (jvm, 0), ==,
                   "-Djava.library.path=${natives_directory}");
  g_assert_cmpstr (g_ptr_array_index (jvm, 2), ==, "${classpath}");
  g_assert_cmpstr (g_ptr_array_index (jvm, 3), ==, "-Xss1M");

  g_assert_cmpuint (game->len, ==, 8);
  g_assert_cmpstr (g_ptr_array_index (game, 0), ==, "--username");
  g_assert_cmpstr (g_ptr_array_index (game, 1), ==, "${auth_player_name}");
  g_assert_false (holds (game, "--demo"));
  g_assert_true (holds (game, "--assetsDir"));

  /* A version old enough to have one line of arguments instead of an
     array keeps it, placeholders and all. */
  g_assert_cmpuint (old_game->len, ==, 12);
  g_assert_cmpstr (g_ptr_array_index (old_game, 0), ==, "--username");
  g_assert_cmpstr (g_ptr_array_index (old_game, 10), ==, "--tweakClass");

  g_print ("ok - the arguments of this machine are kept in order\n");
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  cache_start ();
  fixture_start ();
  test_maven_paths ();
  test_entry_names ();
  test_native_classifiers ();
  test_classifier_is_not_natives ();
  test_rule_filtering ();
  test_classpath_order ();
  test_arguments ();
  fixture_stop ();
  cache_stop ();
  return EXIT_SUCCESS;
}
