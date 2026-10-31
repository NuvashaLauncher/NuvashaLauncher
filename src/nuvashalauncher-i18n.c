/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvashalauncher-i18n.h"

/* Keep template-owned strings visible to the project's lightweight POT audit,
 * which intentionally scans C sources without requiring a Blueprint parser. */
void
nuvashalauncher_i18n_template_strings (void)
{
  (void) _("Sign In"); (void) _("Open in browser"); (void) _("Try Again");
  (void) _("Add Offline Account"); (void) _("Name"); (void) _("Cancel");
  (void) _("Add"); (void) _("Create Instance"); (void) _("Directory");
  (void) _("Choose a folder"); (void) _("Create");
  (void) _("Download Modpack"); (void) _("Import Modpack");
  (void) _("Importing modpack…"); (void) _("Search modpacks");
  (void) _("Close"); (void) _("Loading modpacks…");
  (void) _("Loading more…"); (void) _("Nothing to show yet");
  (void) _("No modpack matches what was asked for. Try another word");
  (void) _("Choose Files"); (void) _("Could not install");
  (void) _("Retry"); (void) _("Back"); (void) _("Search the log");
  (void) _("Classic model"); (void) _("Slim model");
  (void) _("Customize"); (void) _("Delete");
  (void) _("Import skin…"); (void) _("Export skin…");
  (void) _("Minecraft Services error");
  (void) _("The folder could not be opened: %s");
  (void) _("The selected modpack file is not local.");
  (void) _("The log folder could not be opened: %s");
  (void) _("The duplicate file \"%s\" could not be removed: %s");
  (void) _("The replaced file \"%s\" could not be removed");
  (void) _("Could not copy the instance icon");
}

#include <locale.h>

#define N_(string) string

#ifndef nuvashalauncher_LOCALE_DIR
#define nuvashalauncher_LOCALE_DIR "/usr/local/share/locale"
#endif

static const nuvashalauncherLanguage languages[] = {
  { "system", N_("System default") },
  { "en", N_("English") },
  { "es_AR", N_("Español (Argentina)") },
  { "es_ES", N_("Español (España)") },
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

const nuvashalauncherLanguage *
nuvashalauncher_i18n_languages (gsize *length)
{
  if (length != NULL)
    *length = G_N_ELEMENTS (languages);

  return languages;
}

const char *
nuvashalauncher_i18n_language_name (const char *code)
{
  for (gsize i = 0; i < G_N_ELEMENTS (languages); i++)
    if (g_strcmp0 (languages[i].code, code) == 0)
      return languages[i].name;

  return languages[0].name;
}

void
nuvashalauncher_i18n_init (const char *override_code)
{
  g_autofree char *local_locale_dir = NULL;
  g_autofree char *executable = NULL;
  g_autofree char *executable_dir = NULL;
  g_autofree char *executable_locale_dir = NULL;
  g_autofree char *development_locale_dir = NULL;
  const char *locale_dir = nuvashalauncher_LOCALE_DIR;

  if (override_code != NULL &&
      *override_code != '\0' &&
      g_strcmp0 (override_code, "system") != 0)
    {
      g_setenv ("LANGUAGE", override_code, TRUE);
      g_setenv ("LC_MESSAGES", override_code, TRUE);
    }
  else
    {
      /* An explicit system-default choice must not keep an old LANGUAGE
       * environment override from a previous launch. */
      g_unsetenv ("LANGUAGE");
    }

  setlocale (LC_ALL, "");
  local_locale_dir = g_build_filename (g_get_current_dir (), "build",
                                       "locale", NULL);
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
