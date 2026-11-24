/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-i18n.h"

/* Keep template-owned strings visible to the project's lightweight POT audit,
 * which intentionally scans C sources without requiring a Blueprint parser. */
void
nuvashalauncher_i18n_template_strings (void)
{
  (void) _("Sign in"); (void) _("Open in browser"); (void) _("Try again");
  (void) _("Add offline account"); (void) _("Name"); (void) _("Cancel");
  (void) _("Add"); (void) _("Create instance"); (void) _("Directory");
  (void) _("Choose a folder"); (void) _("Create");
  (void) _("Download modpack"); (void) _("Import modpack");
  (void) _("Importing modpack…"); (void) _("Search modpacks");
  (void) _("Close");
  (void) _("Loading more…"); (void) _("Nothing to show yet");
  (void) _("No modpack matches what was asked for. Try another word.");
  (void) _("Choose files"); (void) _("Could not install");
  (void) _("Retry"); (void) _("Back"); (void) _("Search the log");
  (void) _("Classic model"); (void) _("Slim model");
  (void) _("Customize"); (void) _("Delete");
  (void) _("Import skin…"); (void) _("Export skin…");
  (void) _("Minecraft Services error");
  (void) _("The folder could not be opened: %s");
  (void) _("The selected modpack file is not local.");
  (void) _("The log folder could not be opened: %s");
  (void) _("The duplicate file “%s” could not be removed: %s");
  (void) _("The replaced file “%s” could not be removed");
  (void) _("Could not copy the instance icon");
}

#include <locale.h>

#ifndef NUVASHALAUNCHER_LOCALE_DIR
#define NUVASHALAUNCHER_LOCALE_DIR "/usr/local/share/locale"
#endif

static const NuvashaLauncherLanguage languages[] = {
  { "system", N_("System default") },
  { "en", N_("English") },
  { "es", N_("Español") },
  { "es_AR", N_("Español (Argentina)") },
  { "es_ES", N_("Español (España)") },
  { "pt", N_("Português") },
  { "pt_BR", N_("Português (Brasil)") },
  { "pt_PT", N_("Português (Portugal)") },
  { "fr", N_("Français") },
  { "de", N_("Deutsch") },
  { "it", N_("Italiano") },
  { "ja", N_("日本語") },
  { "ko", N_("한국어") },
  { "zh_CN", N_("简体中文") },
  { "zh_TW", N_("繁體中文") },
  { "ru", N_("Русский") },
  { "pl", N_("Polski") },
  { "cs", N_("Čeština") },
  { "hu", N_("Magyar") },
  { "uk", N_("Українська") },
  { "tr", N_("Türkçe") },
  { "nl", N_("Nederlands") },
  { "sv", N_("Svenska") },
  { "da", N_("Dansk") },
  { "fi", N_("Suomi") },
  { "nb", N_("Norsk (bokmål)") },
  { "ro", N_("Română") },
  { "vi", N_("Tiếng Việt") },
  { "id", N_("Bahasa Indonesia") },
  { "ar", N_("العربية") },
};

const NuvashaLauncherLanguage *
nuvashalauncher_i18n_languages (gsize *length)
{
  if (length != NULL)
    *length = G_N_ELEMENTS (languages);

  return languages;
}

void
nuvashalauncher_i18n_init (const char *override_code)
{
  g_autofree char *cwd = NULL;
  g_autofree char *local_locale_dir = NULL;
  g_autofree char *executable = NULL;
  g_autofree char *executable_dir = NULL;
  g_autofree char *executable_locale_dir = NULL;
  g_autofree char *development_locale_dir = NULL;
  const char *locale_dir = NUVASHALAUNCHER_LOCALE_DIR;

  setlocale (LC_ALL, "");

  if (override_code != NULL &&
      *override_code != '\0' &&
      g_strcmp0 (override_code, "system") != 0)
    {
      if (g_str_has_prefix (override_code, "es_"))
        {
          g_autofree char *compound = g_strdup_printf ("%s:es", override_code);
          g_setenv ("LANGUAGE", compound, TRUE);
        }
      else if (g_str_has_prefix (override_code, "pt_"))
        {
          g_autofree char *compound = g_strdup_printf ("%s:pt", override_code);
          g_setenv ("LANGUAGE", compound, TRUE);
        }
      else
        {
          g_setenv ("LANGUAGE", override_code, TRUE);
        }
      g_setenv ("LC_MESSAGES", override_code, TRUE);
    }
  else
    {
      const char * const *langs = g_get_language_names ();
      if (langs != NULL && langs[0] != NULL)
        {
          g_autofree char *lang_list = g_strjoinv (":", (char **) langs);
          g_setenv ("LANGUAGE", lang_list, TRUE);
        }
    }

  cwd = g_get_current_dir ();
  local_locale_dir = g_build_filename (cwd, "build", "locale", NULL);
  if (g_file_test (local_locale_dir, G_FILE_TEST_IS_DIR))
    locale_dir = local_locale_dir;

  executable = g_file_read_link ("/proc/self/exe", NULL);
  if (executable != NULL)
    {
      executable_dir = g_path_get_dirname (executable);
      executable_locale_dir = g_build_filename (executable_dir, "..", "share",
                                                "locale", NULL);

      if (g_file_test (executable_locale_dir, G_FILE_TEST_IS_DIR))
        locale_dir = executable_locale_dir;

      /* A development binary lives in build/ and its catalogs beside it. */
      development_locale_dir = g_build_filename (executable_dir, "locale", NULL);
      if (g_file_test (development_locale_dir, G_FILE_TEST_IS_DIR))
        locale_dir = development_locale_dir;
    }

  bindtextdomain ("nuvashalauncher", locale_dir);
  bind_textdomain_codeset ("nuvashalauncher", "UTF-8");
  textdomain ("nuvashalauncher");
}
