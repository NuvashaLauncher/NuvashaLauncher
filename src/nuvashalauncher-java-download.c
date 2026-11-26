/* Copyright (C) 2026 aKpobzwviq
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Eclipse Temurin JDK discovery and installation via the Adoptium v3 API. */
#define _GNU_SOURCE

#include "nuvashalauncher-java-download.h"
#include "nuvashalauncher-fs.h"
#include "nuvashalauncher-java.h"
#include "nuvashalauncher-json.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define JAVA_DOWNLOAD_MAX_ARCHIVE_SIZE (G_GINT64_CONSTANT (1024) * 1024 * 1024)
#define JAVA_DOWNLOAD_MAX_UNPACKED_SIZE (G_GUINT64_CONSTANT (2) * 1024 * 1024 * 1024)
#define JAVA_DOWNLOAD_MAX_ENTRIES 65536U

G_DEFINE_QUARK (nuvashalauncher-java-download-error-quark, nuvashalauncher_java_download_error)

NuvashaLauncherJavaRelease *
nuvashalauncher_java_release_copy (const NuvashaLauncherJavaRelease *release)
{
  NuvashaLauncherJavaRelease *copy;

  if (release == NULL)
    return NULL;

  copy = g_new0 (NuvashaLauncherJavaRelease, 1);
  copy->major_version = release->major_version;
  copy->lts = release->lts;
  copy->release_name = g_strdup (release->release_name);
  copy->version_display = g_strdup (release->version_display);
  copy->package_name = g_strdup (release->package_name);
  copy->download_url = g_strdup (release->download_url);
  copy->sha256 = g_strdup (release->sha256);
  copy->size = release->size;
  copy->arch = g_strdup (release->arch);
  return copy;
}

void
nuvashalauncher_java_release_free (NuvashaLauncherJavaRelease *release)
{
  if (release == NULL)
    return;

  g_free (release->release_name);
  g_free (release->version_display);
  g_free (release->package_name);
  g_free (release->download_url);
  g_free (release->sha256);
  g_free (release->arch);
  g_free (release);
}

const char *
nuvashalauncher_java_download_host_arch (void)
{
  struct utsname uts;

  if (uname (&uts) == 0)
    {
      if (g_strcmp0 (uts.machine, "x86_64") == 0 ||
          g_strcmp0 (uts.machine, "amd64") == 0)
        return "x64";
      if (g_strcmp0 (uts.machine, "aarch64") == 0 ||
          g_strcmp0 (uts.machine, "arm64") == 0)
        return "aarch64";
    }

#if defined(__x86_64__)
  return "x64";
#elif defined(__aarch64__)
  return "aarch64";
#else
  return NULL;
#endif
}

char *
nuvashalauncher_java_download_install_dir_name (const NuvashaLauncherJavaRelease *release)
{
  GString *out;
  const char *source;

  g_return_val_if_fail (release != NULL, NULL);

  source = (release->release_name != NULL && *release->release_name != '\0')
             ? release->release_name
             : release->version_display;

  out = g_string_new ("temurin-");
  if (source == NULL || *source == '\0')
    {
      g_string_append_printf (out, "jdk-%d", release->major_version);
      return g_string_free (out, FALSE);
    }

  for (const char *p = source; *p != '\0'; p++)
    {
      unsigned char c = (unsigned char) *p;

      if (g_ascii_isalnum (c) || c == '.' || c == '-' || c == '_' || c == '+')
        g_string_append_c (out, (char) c);
      else
        g_string_append_c (out, '_');
    }

  /* Avoid degenerate trailing dots or "..". */
  if (strstr (out->str, "..") != NULL)
    {
      g_string_printf (out, "temurin-jdk-%d", release->major_version);
    }

  return g_string_free (out, FALSE);
}

static char *
find_java_executable_in_dir (const char *root)
{
  static const char *const suffixes[] = {
    "bin/java",
    "jre/bin/java"
  };

  for (gsize i = 0; i < G_N_ELEMENTS (suffixes); i++)
    {
      g_autofree char *candidate = g_build_filename (root, suffixes[i], NULL);

      if (g_file_test (candidate, G_FILE_TEST_IS_REGULAR) &&
          g_file_test (candidate, G_FILE_TEST_IS_EXECUTABLE))
        return g_steal_pointer (&candidate);
    }

  return NULL;
}

gboolean
nuvashalauncher_java_download_is_installed (const NuvashaLauncherJavaRelease *release,
                                            const char *target_dir)
{
  g_autofree char *default_dir = NULL;
  g_autofree char *dir_name = NULL;
  g_autofree char *install_dir = NULL;
  g_autofree char *java_exe = NULL;
  const char *base_dir = target_dir;

  if (release == NULL)
    return FALSE;

  if (base_dir == NULL)
    {
      default_dir = nuvashalauncher_java_get_dir ();
      base_dir = default_dir;
    }
  if (base_dir == NULL)
    return FALSE;

  dir_name = nuvashalauncher_java_download_install_dir_name (release);
  install_dir = g_build_filename (base_dir, dir_name, NULL);
  java_exe = find_java_executable_in_dir (install_dir);
  return java_exe != NULL;
}

/* ---------------------------------------------------------- *
 * Listing available Eclipse Temurin releases
 * ---------------------------------------------------------- */

static gboolean
is_valid_sha256_hex (const char *hex)
{
  if (hex == NULL || strlen (hex) != 64)
    return FALSE;

  for (gsize i = 0; i < 64; i++)
    {
      if (!g_ascii_isxdigit ((unsigned char) hex[i]))
        return FALSE;
    }

  return TRUE;
}

static int
compare_releases_desc (gconstpointer a, gconstpointer b)
{
  const NuvashaLauncherJavaRelease *ra = *(const NuvashaLauncherJavaRelease *const *) a;
  const NuvashaLauncherJavaRelease *rb = *(const NuvashaLauncherJavaRelease *const *) b;

  if (ra->major_version != rb->major_version)
    return rb->major_version - ra->major_version;

  return g_strcmp0 (rb->release_name, ra->release_name);
}

static JsonObject *
json_object_field (JsonObject *object, const char *name)
{
  JsonNode *node;

  if (object == NULL || !json_object_has_member (object, name))
    return NULL;
  node = json_object_get_member (object, name);
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return NULL;
  return json_node_get_object (node);
}

static JsonArray *
json_array_field (JsonObject *object, const char *name)
{
  JsonNode *node;

  if (object == NULL || !json_object_has_member (object, name))
    return NULL;
  node = json_object_get_member (object, name);
  if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
    return NULL;
  return json_node_get_array (node);
}

static const char *
json_string_field (JsonObject *object, const char *name)
{
  JsonNode *node;

  if (object == NULL || !json_object_has_member (object, name))
    return NULL;
  node = json_object_get_member (object, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (node);
}

static NuvashaLauncherJavaRelease *
parse_latest_asset_response (JsonParser *parser,
                             int major_version,
                             gboolean lts,
                             const char *expected_arch)
{
  JsonNode *root = json_parser_get_root (parser);
  JsonArray *items;
  guint len;

  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    return NULL;

  items = json_node_get_array (root);
  len = json_array_get_length (items);

  for (guint i = 0; i < len; i++)
    {
      JsonNode *item_node = json_array_get_element (items, i);
      JsonObject *item;
      JsonObject *binary;
      JsonObject *pkg;
      JsonObject *version_obj;
      const char *os;
      const char *arch;
      const char *image_type;
      const char *link;
      const char *checksum;
      const char *pkg_name;
      const char *release_name;
      const char *semver = NULL;
      gint64 size = 0;
      g_autoptr (NuvashaLauncherJavaRelease) release = NULL;

      if (item_node == NULL || !JSON_NODE_HOLDS_OBJECT (item_node))
        continue;
      item = json_node_get_object (item_node);

      binary = json_object_field (item, "binary");
      if (binary == NULL)
        continue;

      os = json_string_field (binary, "os");
      arch = json_string_field (binary, "architecture");
      image_type = json_string_field (binary, "image_type");
      if (g_strcmp0 (os, "linux") != 0 ||
          g_strcmp0 (arch, expected_arch) != 0 ||
          g_strcmp0 (image_type, "jdk") != 0)
        continue;

      pkg = json_object_field (binary, "package");
      if (pkg == NULL)
        continue;

      link = json_string_field (pkg, "link");
      checksum = json_string_field (pkg, "checksum");
      pkg_name = json_string_field (pkg, "name");
      if (link == NULL || *link == '\0' ||
          pkg_name == NULL || !g_str_has_suffix (pkg_name, ".tar.gz") ||
          !is_valid_sha256_hex (checksum))
        continue;

      if (json_object_has_member (pkg, "size"))
        {
          JsonNode *size_node = json_object_get_member (pkg, "size");
          if (size_node != NULL && JSON_NODE_HOLDS_VALUE (size_node))
            size = json_node_get_int (size_node);
        }

      release_name = json_string_field (item, "release_name");
      version_obj = json_object_field (item, "version");
      if (version_obj != NULL)
        {
          semver = json_string_field (version_obj, "semver");
          if (semver == NULL || *semver == '\0')
            semver = json_string_field (version_obj, "openjdk_version");
        }

      release = g_new0 (NuvashaLauncherJavaRelease, 1);
      release->major_version = major_version;
      release->lts = lts;
      release->release_name = g_strdup (release_name != NULL ? release_name : "");
      release->version_display = g_strdup ((semver != NULL && *semver != '\0')
                                             ? semver
                                             : (release_name != NULL ? release_name : ""));
      release->package_name = g_strdup (pkg_name);
      release->download_url = g_strdup (link);
      release->sha256 = g_ascii_strdown (checksum, -1);
      release->size = size > 0 ? (guint64) size : 0;
      release->arch = g_strdup (expected_arch);
      return g_steal_pointer (&release);
    }

  return NULL;
}

typedef struct {
  NuvashaLauncherTaskScope *scope;
  NuvashaLauncherDownloadService *service;
  char *api_base;
  char *arch;
  GPtrArray *releases;
  GError *first_error;
  guint pending;
  gboolean finished;
} ListState;

typedef struct {
  ListState *state;
  int major_version;
  gboolean lts;
} MajorFetch;

static void
list_state_free (ListState *state)
{
  if (state == NULL)
    return;

  g_clear_pointer (&state->scope, nuvashalauncher_task_scope_unref);
  g_free (state->api_base);
  g_free (state->arch);
  g_clear_pointer (&state->releases, g_ptr_array_unref);
  g_clear_error (&state->first_error);
  g_free (state);
}

static void
list_state_maybe_complete (ListState *state)
{
  if (state->finished || state->pending > 0)
    return;

  state->finished = TRUE;

  if (nuvashalauncher_task_scope_return_error_if_cancelled (state->scope))
    {
      list_state_free (state);
      return;
    }

  if (state->releases->len == 0 && state->first_error != NULL)
    {
      GError *err = g_steal_pointer (&state->first_error);
      nuvashalauncher_task_scope_return_error (state->scope, err);
      list_state_free (state);
      return;
    }

  g_ptr_array_sort (state->releases, compare_releases_desc);
  nuvashalauncher_task_scope_return_pointer (state->scope,
                                             g_steal_pointer (&state->releases),
                                             (GDestroyNotify) g_ptr_array_unref);
  list_state_free (state);
}

static void
on_major_asset_loaded (GObject *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer user_data)
{
  g_autofree MajorFetch *fetch = user_data;
  ListState *state = fetch->state;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = nuvashalauncher_download_json_finish (result, &error);

  if (parser != NULL)
    {
      NuvashaLauncherJavaRelease *release = parse_latest_asset_response (
        parser, fetch->major_version, fetch->lts, state->arch);
      if (release != NULL)
        g_ptr_array_add (state->releases, release);
    }
  else if (error != NULL &&
           !g_error_matches (error, NUVASHALAUNCHER_DOWNLOAD_ERROR,
                             NUVASHALAUNCHER_DOWNLOAD_ERROR_CANCELLED) &&
           !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
           state->first_error == NULL)
    {
      /* HTTP 404 simply means this major version has no build for the host arch. */
      if (!(g_error_matches (error, NUVASHALAUNCHER_DOWNLOAD_ERROR,
                             NUVASHALAUNCHER_DOWNLOAD_ERROR_HTTP) &&
            strstr (error->message, "HTTP 404") != NULL))
        state->first_error = g_steal_pointer (&error);
    }

  state->pending--;
  list_state_maybe_complete (state);
}

static void
on_available_releases_loaded (GObject *source G_GNUC_UNUSED,
                              GAsyncResult *result,
                              gpointer user_data)
{
  ListState *state = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = nuvashalauncher_download_json_finish (result, &error);
  JsonNode *root;
  JsonObject *object;
  JsonArray *available;
  JsonArray *lts_array;
  g_autoptr (GHashTable) lts_set = NULL;
  g_autoptr (GArray) majors = NULL;
  GCancellable *cancellable;

  if (nuvashalauncher_task_scope_return_error_if_cancelled (state->scope))
    {
      list_state_free (state);
      return;
    }

  if (parser == NULL)
    {
      nuvashalauncher_task_scope_return_error (state->scope, g_steal_pointer (&error));
      list_state_free (state);
      return;
    }

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      nuvashalauncher_task_scope_return_error (
        state->scope,
        g_error_new_literal (NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                             NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_FORMAT,
                             "unexpected response from Adoptium"));
      list_state_free (state);
      return;
    }

  object = json_node_get_object (root);
  available = json_array_field (object, "available_releases");
  if (available == NULL)
    {
      nuvashalauncher_task_scope_return_error (
        state->scope,
        g_error_new_literal (NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                             NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_FORMAT,
                             "Adoptium release list was missing"));
      list_state_free (state);
      return;
    }

  lts_set = g_hash_table_new (g_direct_hash, g_direct_equal);
  lts_array = json_array_field (object, "available_lts_releases");
  if (lts_array != NULL)
    {
      guint n_lts = json_array_get_length (lts_array);
      for (guint i = 0; i < n_lts; i++)
        {
          JsonNode *n = json_array_get_element (lts_array, i);
          if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
            {
              gint64 v = json_node_get_int (n);
              if (v >= 8 && v <= 1000)
                g_hash_table_add (lts_set, GINT_TO_POINTER ((int) v));
            }
        }
    }

  majors = g_array_new (FALSE, FALSE, sizeof (int));
  {
    guint n_avail = json_array_get_length (available);
    for (guint i = 0; i < n_avail; i++)
      {
        JsonNode *n = json_array_get_element (available, i);
        if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
          {
            gint64 v = json_node_get_int (n);
            if (v >= 8 && v <= 1000)
              {
                int major = (int) v;
                g_array_append_val (majors, major);
              }
          }
      }
  }

  if (majors->len == 0)
    {
      list_state_maybe_complete (state);
      return;
    }

  cancellable = nuvashalauncher_task_scope_get_cancellable (state->scope);
  state->pending = majors->len;

  for (guint i = 0; i < majors->len; i++)
    {
      int major = g_array_index (majors, int, i);
      g_autofree char *url = g_strdup_printf (
        "%s/assets/latest/%d/hotspot?os=linux&architecture=%s&image_type=jdk&vendor=eclipse",
        state->api_base, major, state->arch);
      MajorFetch *fetch = g_new0 (MajorFetch, 1);
      NuvashaLauncherTaskScope *child;

      fetch->state = state;
      fetch->major_version = major;
      fetch->lts = g_hash_table_contains (lts_set, GINT_TO_POINTER (major));

      child = nuvashalauncher_download_json_metadata_cached_async (
        state->service, url, on_major_asset_loaded, fetch);
      nuvashalauncher_task_scope_release_when_done (child, cancellable);
    }
}

NuvashaLauncherTaskScope *
nuvashalauncher_java_download_list_async (NuvashaLauncherDownloadService *service,
                                          const char *api_base,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data)
{
  NuvashaLauncherTaskScope *scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  const char *arch = nuvashalauncher_java_download_host_arch ();
  ListState *state;
  g_autofree char *url = NULL;
  NuvashaLauncherTaskScope *child;

  if (arch == NULL)
    {
      nuvashalauncher_task_scope_return_error_deferred (
        scope,
        g_error_new_literal (NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                             NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_UNSUPPORTED_ARCH,
                             "unsupported host architecture"));
      return scope;
    }

  state = g_new0 (ListState, 1);
  state->scope = nuvashalauncher_task_scope_ref (scope);
  state->service = service;
  state->api_base = g_strdup ((api_base != NULL && *api_base != '\0')
                                ? api_base
                                : NUVASHALAUNCHER_ADOPTIUM_API_BASE);
  state->arch = g_strdup (arch);
  state->releases = g_ptr_array_new_with_free_func (
    (GDestroyNotify) nuvashalauncher_java_release_free);

  url = g_strdup_printf ("%s/info/available_releases", state->api_base);
  child = nuvashalauncher_download_json_metadata_cached_async (
    service, url, on_available_releases_loaded, state);
  nuvashalauncher_task_scope_release_when_done (
    child, nuvashalauncher_task_scope_get_cancellable (scope));

  return scope;
}

GPtrArray *
nuvashalauncher_java_download_list_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---------------------------------------------------------- *
 * Safe .tar.gz extraction and installation
 * ---------------------------------------------------------- */

static gboolean
relative_path_is_safe (const char *path)
{
  const char *p = path;

  if (path == NULL || *path == '\0' || path[0] == '/')
    return FALSE;

  if (strchr (path, '\\') != NULL)
    return FALSE;

  while (*p != '\0')
    {
      const char *slash = strchr (p, '/');
      gsize len = slash != NULL ? (gsize) (slash - p) : strlen (p);

      if (len == 0 ||
          (len == 1 && p[0] == '.') ||
          (len == 2 && p[0] == '.' && p[1] == '.'))
        return FALSE;

      if (slash == NULL)
        break;
      p = slash + 1;
    }

  return TRUE;
}

static gboolean
symlink_target_stays_inside_root (const char *rel_link_path, const char *target)
{
  gint depth = 0;
  g_autofree char *parent = NULL;
  const char *p;

  if (target == NULL || *target == '\0' || target[0] == '/' || strchr (target, '\\') != NULL)
    return FALSE;

  parent = g_path_get_dirname (rel_link_path);
  if (g_strcmp0 (parent, ".") != 0)
    {
      p = parent;
      while (*p != '\0')
        {
          const char *slash = strchr (p, '/');
          depth++;
          if (slash == NULL)
            break;
          p = slash + 1;
        }
    }

  p = target;
  while (*p != '\0')
    {
      const char *slash = strchr (p, '/');
      gsize len = slash != NULL ? (gsize) (slash - p) : strlen (p);

      if (len == 2 && p[0] == '.' && p[1] == '.')
        {
          depth--;
          if (depth < 0)
            return FALSE;
        }
      else if (len > 0 && !(len == 1 && p[0] == '.'))
        {
          depth++;
        }

      if (slash == NULL)
        break;
      p = slash + 1;
    }

  return TRUE;
}

static char *
strip_top_directory (const char *raw_name, char **top_prefix, gboolean *is_root_dir)
{
  const char *p = raw_name;
  const char *slash;
  gsize prefix_len;
  const char *rest;
  gsize rest_len;

  *is_root_dir = FALSE;

  if (p == NULL)
    return NULL;

  while (p[0] == '.' && p[1] == '/')
    p += 2;

  if (*p == '\0' || *p == '/')
    return NULL;

  slash = strchr (p, '/');
  if (slash == NULL)
    {
      if (*top_prefix == NULL)
        *top_prefix = g_strdup (p);
      if (g_strcmp0 (*top_prefix, p) == 0)
        {
          *is_root_dir = TRUE;
          return NULL;
        }
      return NULL;
    }

  prefix_len = (gsize) (slash - p);
  if (prefix_len == 0)
    return NULL;

  if (*top_prefix == NULL)
    *top_prefix = g_strndup (p, prefix_len);
  else if (strlen (*top_prefix) != prefix_len || strncmp (*top_prefix, p, prefix_len) != 0)
    return NULL;

  rest = slash + 1;
  rest_len = strlen (rest);
  while (rest_len > 0 && rest[rest_len - 1] == '/')
    rest_len--;

  if (rest_len == 0)
    {
      *is_root_dir = TRUE;
      return NULL;
    }

  return g_strndup (rest, rest_len);
}

static gboolean
write_all_fd (int fd, const void *buffer, gsize size, GError **error)
{
  const guint8 *ptr = buffer;
  gsize remaining = size;

  while (remaining > 0)
    {
      ssize_t written = write (fd, ptr, remaining);

      if (written < 0)
        {
          if (errno == EINTR)
            continue;
          g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                       NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_IO,
                       "write failed: %s", g_strerror (errno));
          return FALSE;
        }
      ptr += written;
      remaining -= (gsize) written;
    }

  return TRUE;
}

static gboolean
extract_temurin_tar_gz (const char *archive_path,
                        const char *dest_dir,
                        GCancellable *cancellable,
                        GError **error)
{
  struct archive *archive = NULL;
  struct archive_entry *header = NULL;
  g_autofree char *top_prefix = NULL;
  guint entries = 0;
  guint64 total_bytes = 0;
  int fd = -1;
  int status;
  gboolean ok = FALSE;

  fd = open (archive_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    {
      g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                   NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                   "cannot open archive: %s", g_strerror (errno));
      return FALSE;
    }

  archive = archive_read_new ();
  archive_read_support_filter_gzip (archive);
  archive_read_support_format_tar (archive);

  if (archive_read_open_fd (archive, fd, 65536) != ARCHIVE_OK)
    {
      g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                   NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                   "cannot read archive: %s", archive_error_string (archive));
      goto out;
    }

  while ((status = archive_read_next_header (archive, &header)) == ARCHIVE_OK ||
         status == ARCHIVE_WARN)
    {
      const char *raw_name;
      mode_t filetype;
      gboolean is_root_dir = FALSE;
      g_autofree char *rel_path = NULL;

      if (g_cancellable_is_cancelled (cancellable))
        {
          g_set_error_literal (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                               NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_CANCELLED,
                               "download cancelled");
          goto out;
        }

      if (++entries > JAVA_DOWNLOAD_MAX_ENTRIES)
        {
          g_set_error_literal (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                               NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                               "Java archive contains too many entries");
          goto out;
        }

      if (archive_entry_hardlink (header) != NULL)
        {
          g_set_error_literal (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                               NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                               "Java archive contains an unsupported hard link");
          goto out;
        }

      raw_name = archive_entry_pathname (header);
      filetype = archive_entry_filetype (header);
      rel_path = strip_top_directory (raw_name, &top_prefix, &is_root_dir);

      if (is_root_dir)
        {
          if (archive_read_data_skip (archive) != ARCHIVE_OK)
            {
              g_set_error_literal (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                                   NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                                   "Java archive is truncated");
              goto out;
            }
          continue;
        }

      if (rel_path == NULL || !relative_path_is_safe (rel_path))
        {
          g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                       NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                       "Java archive contains an unsafe path: %s",
                       raw_name != NULL ? raw_name : "");
          goto out;
        }

      if (filetype == AE_IFDIR)
        {
          if (!nuvashalauncher_fs_make_directory_tree_nofollow (dest_dir, rel_path, 0755, error))
            goto out;
        }
      else if (filetype == AE_IFREG)
        {
          int perm = archive_entry_perm (header);
          int file_mode = (perm & 0111) ? 0755 : 0644;
          int out_fd = nuvashalauncher_fs_open_file_nofollow (
            dest_dir, rel_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
            0755, file_mode, error);
          const void *block = NULL;
          size_t block_size = 0;
          la_int64_t offset = 0;
          int data_status;

          if (out_fd < 0)
            goto out;

          while ((data_status = archive_read_data_block (archive, &block,
                                                         &block_size, &offset)) == ARCHIVE_OK)
            {
              if (g_cancellable_is_cancelled (cancellable))
                {
                  close (out_fd);
                  g_set_error_literal (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                                       NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_CANCELLED,
                                       "download cancelled");
                  goto out;
                }

              if (block_size > JAVA_DOWNLOAD_MAX_UNPACKED_SIZE - total_bytes)
                {
                  close (out_fd);
                  g_set_error_literal (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                                       NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                                       "Java archive exceeds maximum unpacked size");
                  goto out;
                }

              total_bytes += block_size;
              if (!write_all_fd (out_fd, block, block_size, error))
                {
                  close (out_fd);
                  goto out;
                }
            }

          close (out_fd);
          if (data_status != ARCHIVE_EOF)
            {
              g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                           NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                           "cannot extract file: %s", archive_error_string (archive));
              goto out;
            }
        }
      else if (filetype == AE_IFLNK)
        {
          const char *link_target = archive_entry_symlink (header);
          g_autofree char *parent_rel = g_path_get_dirname (rel_path);
          g_autofree char *base_name = g_path_get_basename (rel_path);
          g_autofree char *parent_abs = NULL;
          int dir_fd = -1;

          if (!symlink_target_stays_inside_root (rel_path, link_target))
            {
              g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                           NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                           "Java archive contains an unsafe symbolic link: %s",
                           rel_path);
              goto out;
            }

          if (g_strcmp0 (parent_rel, ".") != 0)
            {
              if (!nuvashalauncher_fs_make_directory_tree_nofollow (dest_dir, parent_rel, 0755, error))
                goto out;
              parent_abs = g_build_filename (dest_dir, parent_rel, NULL);
            }
          else
            {
              parent_abs = g_strdup (dest_dir);
            }

          dir_fd = nuvashalauncher_fs_open_directory_nofollow (parent_abs, error);
          if (dir_fd < 0)
            goto out;

          if (symlinkat (link_target, dir_fd, base_name) < 0 && errno != EEXIST)
            {
              int saved = errno;
              close (dir_fd);
              g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                           NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_IO,
                           "cannot create symbolic link: %s", g_strerror (saved));
              goto out;
            }
          close (dir_fd);
        }
      else
        {
          g_set_error_literal (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                               NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                               "Java archive contains an unsupported entry type");
          goto out;
        }
    }

  if (status != ARCHIVE_EOF)
    {
      g_set_error (error, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                   NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                   "archive read error: %s", archive_error_string (archive));
      goto out;
    }

  ok = TRUE;

out:
  if (archive != NULL)
    {
      archive_read_close (archive);
      archive_read_free (archive);
    }
  if (fd >= 0)
    close (fd);
  return ok;
}

typedef struct {
  NuvashaLauncherTaskScope *scope;
  NuvashaLauncherDownloadService *service;
  NuvashaLauncherJavaRelease *release;
  char *target_dir;
  char *archive_path;
  NuvashaLauncherJavaDownloadProgressFunc progress;
  gpointer user_data;
} InstallState;

static void
install_state_free (InstallState *state)
{
  if (state == NULL)
    return;

  if (state->archive_path != NULL)
    {
      nuvashalauncher_fs_unlink (state->archive_path, NULL);
      g_free (state->archive_path);
    }

  g_clear_pointer (&state->scope, nuvashalauncher_task_scope_unref);
  g_clear_pointer (&state->release, nuvashalauncher_java_release_free);
  g_free (state->target_dir);
  g_free (state);
}

typedef struct {
  char *archive_path;
  char *target_dir;
  char *dir_name;
} ExtractJob;

static void
extract_job_free (gpointer data)
{
  ExtractJob *job = data;

  if (job == NULL)
    return;

  if (job->archive_path != NULL)
    {
      nuvashalauncher_fs_unlink (job->archive_path, NULL);
      g_free (job->archive_path);
    }
  g_free (job->target_dir);
  g_free (job->dir_name);
  g_free (job);
}

static void
extract_worker (GTask *task,
                gpointer source_object G_GNUC_UNUSED,
                gpointer task_data,
                GCancellable *cancellable)
{
  ExtractJob *job = task_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *temp_dir = NULL;
  g_autofree char *final_dir = NULL;
  g_autofree char *temp_java = NULL;
  g_autofree char *final_java = NULL;
  g_autofree char *backup_dir = NULL;

  temp_dir = nuvashalauncher_fs_make_temporary_directory (
    job->target_dir, ".temurin-extract-XXXXXX", &error);
  if (temp_dir == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (!extract_temurin_tar_gz (job->archive_path, temp_dir, cancellable, &error))
    {
      nuvashalauncher_fs_remove_tree (temp_dir);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  temp_java = find_java_executable_in_dir (temp_dir);
  if (temp_java == NULL)
    {
      nuvashalauncher_fs_remove_tree (temp_dir);
      g_task_return_new_error (task, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                               NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_ARCHIVE,
                               "downloaded archive did not contain a Java executable");
      return;
    }

  final_dir = g_build_filename (job->target_dir, job->dir_name, NULL);

  if (g_file_test (final_dir, G_FILE_TEST_EXISTS))
    {
      backup_dir = g_strdup_printf ("%s.old-%" G_GINT64_FORMAT,
                                    final_dir, g_get_monotonic_time ());
      if (!nuvashalauncher_fs_rename (final_dir, backup_dir, &error))
        {
          nuvashalauncher_fs_remove_tree (temp_dir);
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }
    }

  if (!nuvashalauncher_fs_rename (temp_dir, final_dir, &error))
    {
      if (backup_dir != NULL)
        nuvashalauncher_fs_rename (backup_dir, final_dir, NULL);
      nuvashalauncher_fs_remove_tree (temp_dir);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (backup_dir != NULL)
    nuvashalauncher_fs_remove_tree (backup_dir);

  final_java = find_java_executable_in_dir (final_dir);
  if (final_java == NULL)
    {
      g_task_return_new_error (task, NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                               NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_IO,
                               "installed Java executable could not be found");
      return;
    }

  g_task_return_pointer (task, g_steal_pointer (&final_java), g_free);
}

static void
on_extract_done (GObject *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer user_data)
{
  InstallState *state = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *java_exe = g_task_propagate_pointer (G_TASK (result), &error);

  if (java_exe != NULL)
    nuvashalauncher_task_scope_return_pointer (state->scope,
                                               g_steal_pointer (&java_exe),
                                               g_free);
  else
    nuvashalauncher_task_scope_return_error (state->scope, g_steal_pointer (&error));

  install_state_free (state);
}

static void
on_archive_progress (goffset bytes_done, goffset total, gpointer user_data)
{
  InstallState *state = user_data;

  if (state->progress != NULL)
    {
      goffset effective_total = total > 0 ? total : (goffset) state->release->size;
      state->progress (NUVASHALAUNCHER_JAVA_DOWNLOAD_STAGE_DOWNLOADING,
                       bytes_done, effective_total, state->user_data);
    }
}

static void
on_archive_downloaded (GObject *source G_GNUC_UNUSED,
                       GAsyncResult *result,
                       gpointer user_data)
{
  InstallState *state = user_data;
  g_autoptr (GError) error = NULL;
  ExtractJob *job;
  g_autoptr (GTask) worker_task = NULL;

  if (!nuvashalauncher_download_file_finish (result, &error))
    {
      nuvashalauncher_task_scope_return_error (state->scope, g_steal_pointer (&error));
      install_state_free (state);
      return;
    }

  if (nuvashalauncher_task_scope_return_error_if_cancelled (state->scope))
    {
      install_state_free (state);
      return;
    }

  if (state->progress != NULL)
    state->progress (NUVASHALAUNCHER_JAVA_DOWNLOAD_STAGE_EXTRACTING, 0, 0, state->user_data);

  job = g_new0 (ExtractJob, 1);
  job->archive_path = g_steal_pointer (&state->archive_path);
  job->target_dir = g_strdup (state->target_dir);
  job->dir_name = nuvashalauncher_java_download_install_dir_name (state->release);

  worker_task = g_task_new (NULL,
                            nuvashalauncher_task_scope_get_cancellable (state->scope),
                            on_extract_done, state);
  g_task_set_task_data (worker_task, job, extract_job_free);
  g_task_run_in_thread (worker_task, extract_worker);
}

NuvashaLauncherTaskScope *
nuvashalauncher_java_download_install_async (
  NuvashaLauncherDownloadService *service,
  const NuvashaLauncherJavaRelease *release,
  const char *target_dir,
  NuvashaLauncherJavaDownloadProgressFunc progress,
  GAsyncReadyCallback callback,
  gpointer user_data)
{
  NuvashaLauncherTaskScope *scope =
    nuvashalauncher_task_scope_new (NULL, NULL, callback, user_data);
  g_autoptr (GError) error = NULL;
  g_autofree char *resolved_dir = NULL;
  g_autofree char *tmp_archive = NULL;
  int tmp_fd = -1;
  InstallState *state;
  NuvashaLauncherTaskScope *child;

  if (release == NULL ||
      release->download_url == NULL || *release->download_url == '\0' ||
      !is_valid_sha256_hex (release->sha256))
    {
      nuvashalauncher_task_scope_return_error_deferred (
        scope,
        g_error_new_literal (NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                             NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_FORMAT,
                             "invalid Java release metadata"));
      return scope;
    }

  resolved_dir = (target_dir != NULL && *target_dir != '\0')
                   ? g_strdup (target_dir)
                   : nuvashalauncher_java_get_dir ();
  if (resolved_dir == NULL ||
      !nuvashalauncher_fs_make_directory (resolved_dir, 0755, &error))
    {
      nuvashalauncher_task_scope_return_error_deferred (
        scope,
        error != NULL
          ? g_steal_pointer (&error)
          : g_error_new_literal (NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR,
                                 NUVASHALAUNCHER_JAVA_DOWNLOAD_ERROR_IO,
                                 "could not create Java directory"));
      return scope;
    }

  tmp_fd = nuvashalauncher_fs_open_temporary_file (
    resolved_dir, ".temurin-archive-XXXXXX", &tmp_archive, &error);
  if (tmp_fd < 0)
    {
      nuvashalauncher_task_scope_return_error_deferred (scope, g_steal_pointer (&error));
      return scope;
    }
  close (tmp_fd);
  /* Remove placeholder so download service writes fresh bytes to it. */
  nuvashalauncher_fs_unlink (tmp_archive, NULL);

  state = g_new0 (InstallState, 1);
  state->scope = nuvashalauncher_task_scope_ref (scope);
  state->service = service;
  state->release = nuvashalauncher_java_release_copy (release);
  state->target_dir = g_steal_pointer (&resolved_dir);
  state->archive_path = g_steal_pointer (&tmp_archive);
  state->progress = progress;
  state->user_data = user_data;

  child = nuvashalauncher_download_file_limited_async (
    service,
    state->release->download_url,
    state->archive_path,
    state->release->sha256,
    G_CHECKSUM_SHA256,
    JAVA_DOWNLOAD_MAX_ARCHIVE_SIZE,
    on_archive_progress,
    on_archive_downloaded,
    state);
  nuvashalauncher_task_scope_release_when_done (
    child, nuvashalauncher_task_scope_get_cancellable (scope));

  return scope;
}

char *
nuvashalauncher_java_download_install_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
