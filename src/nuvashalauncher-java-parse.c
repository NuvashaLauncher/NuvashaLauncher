/* Parsing Java runtime version reports. */
#define _GNU_SOURCE

#include "nuvashalauncher-java.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- *
 * Parsing `java -version`
 * ---------------------------------------------------------- */

/* The major number out of the banner's version string: "1.8.0_x" is 8,
 * everything else starts with the major itself. Returns FALSE when the
 * string has no usable number. */
static gboolean
parse_major (const char *version, int *major)
{
  g_auto (GStrv) parts = g_strsplit (version, ".", 0);

  /* An empty or all-dots version string names no Java version. */
  if (parts == NULL || parts[0] == NULL || *parts[0] == '\0')
    return FALSE;

  if (g_strcmp0 (parts[0], "1") == 0 && parts[1] != NULL)
    return sscanf (parts[1], "%d", major) == 1;

  return sscanf (parts[0], "%d", major) == 1;
}

gboolean
nuvashalauncher_java_parse_version (const char  *output,
                        gssize       length,
                        int         *major_version,
                        char       **architecture,
                        char       **vendor)
{
  g_auto (GStrv) lines =
    g_strsplit (output != NULL ? output : "", "\n", 0);
  int major = 0;
  char *arch = NULL;
  char *arch_fallback = NULL;
  char *vendor_out = NULL;
  char *vendor_property = NULL;
  char *vendor_version = NULL;

  g_return_val_if_fail (output != NULL, FALSE);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *banner;
      char *start;
      char *end;

      /* The banner around `version "x.y.z"`. */
      if (major == 0 && (banner = g_strstr_len (lines[i], length, "version \"")) != NULL)
        {
          start = banner + strlen ("version \"");
          end = strchr (start, '"');

          if (end != NULL)
            {
              g_autofree char *version = g_strndup (start, end - start);

              parse_major (version, &major);
            }
        }

      if (arch == NULL && g_strstr_len (lines[i], length, "os.arch =") != NULL)
        {
          char *equals = strchr (lines[i], '=');

          arch = g_strdup (g_strstrip (equals + 1));
        }
      else if (arch_fallback == NULL &&
               g_strstr_len (lines[i], length, "64-Bit") != NULL)
        arch_fallback = g_strdup ("64-bit");
      else if (arch_fallback == NULL &&
               g_strstr_len (lines[i], length, "32-Bit") != NULL)
        arch_fallback = g_strdup ("32-bit");

      if (vendor_property == NULL &&
          g_strstr_len (lines[i], length, "java.vendor =") != NULL)
        {
          char *equals = strchr (lines[i], '=');
          vendor_property = g_strdup (g_strstrip (equals + 1));
        }

      if (vendor_version == NULL &&
          g_strstr_len (lines[i], length, "java.vendor.version =") != NULL)
        {
          char *equals = strchr (lines[i], '=');
          vendor_version = g_strdup (g_strstrip (equals + 1));
        }
    }

  if (vendor_property != NULL)
    {
      /* Temurin identifies itself in java.vendor.version as
       * "Temurin-<version>"; use that distribution name instead of the
       * generic OpenJDK runtime banner. Other vendors use java.vendor. */
      if (vendor_version != NULL &&
          g_str_has_prefix (vendor_version, "Temurin-"))
        vendor_out = g_strdup ("Temurin");
      else
        vendor_out = g_steal_pointer (&vendor_property);
    }
  g_free (vendor_property);
  g_free (vendor_version);

  /* The explicit os.arch wins; the 64-Bit/32-Bit banner is the
   * fallback for reports where the JVM did not print its properties. */
  if (arch == NULL)
    arch = arch_fallback;
  else
    g_free (arch_fallback);

  /* The vendor sits in front of the runtime name on the
   * "Runtime Environment" line: "OpenJDK Runtime Environment (build
   * ...)". The property dump from -XshowSettings shows
   * java.runtime.name = OpenJDK Runtime Environment  too, but that
   * line does not belong to the exit report, so skip it. */
  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *cut = g_strstr_len (lines[i], length, "Runtime Environment");

      if (vendor_out == NULL && cut != NULL && strchr (lines[i], '=') == NULL)
        {
          g_autofree char *prefix = g_strndup (lines[i], cut - lines[i]);

          vendor_out = g_strdup (g_strstrip (prefix));

          if (vendor_out[0] == '\0')
            g_clear_pointer (&vendor_out, g_free);

          break;
        }
    }

  if (major == 0)
    {
      g_free (arch);
      g_free (vendor_out);
      return FALSE;
    }

  if (major_version != NULL)
    *major_version = major;

  if (architecture != NULL)
    *architecture = g_steal_pointer (&arch);
  else
    g_free (arch);

  if (vendor != NULL)
    *vendor = g_steal_pointer (&vendor_out);
  else
    g_free (vendor_out);

  return TRUE;
}
