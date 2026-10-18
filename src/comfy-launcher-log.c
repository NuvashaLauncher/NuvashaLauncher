/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "comfy-launcher-private.h"
#include "comfy-i18n.h"

#include <string.h>
#include <stdarg.h>

/* Replace per-launch credentials before a line is retained, emitted or
 * shown. This also catches old Minecraft versions that print their complete
 * legacy session as "Session ID is token:...". */
static char *
censor_private_text (ComfyLauncher *self, const char *text)
{
  g_autofree char *result = g_strdup (text != NULL ? text : "");
  const char *secrets[] = { self->log_access_token, self->log_session,
                            self->log_uuid };

  for (guint i = 0; i < G_N_ELEMENTS (secrets); i++)
    {
      g_auto (GStrv) parts = NULL;
      g_autofree char *joined = NULL;

      if (secrets[i] == NULL || secrets[i][0] == '\0')
        continue;

      parts = g_strsplit (result, secrets[i], -1);
      joined = g_strjoinv ("***", parts);
      g_free (g_steal_pointer (&result));
      result = g_steal_pointer (&joined);
    }

  return g_steal_pointer (&result);
}

/* Internal launcher log buffer and formatting workflow. */

/* ---------------------------------------------------------- *
 * The log
 * ---------------------------------------------------------- */

guint
comfy_launcher_get_line_limit (ComfyLauncher *self)
{
  guint limit;

  g_return_val_if_fail (comfy_IS_LAUNCHER (self), 0);

  /* Read for every line that comes in, so a limit the user changes
     while a game runs takes effect at once. */
  limit = self->settings != NULL
            ? comfy_settings_get_log_line_limit (self->settings)
            : comfy_SETTINGS_DEFAULT_LOG_LINE_LIMIT;

  return limit > 0 ? limit : comfy_SETTINGS_DEFAULT_LOG_LINE_LIMIT;
}

/* One line of the log: the time it happened, where it came from and the
 * line itself. Everything a game prints goes through here, and so does
 * everything comfy says about the launch. */
void
log_line (ComfyLauncher *self,
          const char  *tag,
          const char  *format,
          ...) G_GNUC_PRINTF (3, 4);

void
log_line (ComfyLauncher *self,
          const char  *tag,
          const char  *format,
          ...)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = NULL;
  g_autofree char *body = NULL;
  g_autofree char *line = NULL;
  va_list args;
  guint limit;

  va_start (args, format);
  body = g_strdup_vprintf (format, args);
  va_end (args);

  {
    g_autofree char *censored = censor_private_text (self, body);
    g_free (g_steal_pointer (&body));
    body = g_steal_pointer (&censored);
  }

  stamp = now != NULL ? g_date_time_format (now, "%H:%M:%S")
                      : g_strdup ("--:--:--");

  /* The time it happened, where it came from and the line itself. */
  line = g_strdup_printf ("%s [%s] %s", stamp, tag, body);

  limit = comfy_launcher_get_line_limit (self);

  g_ptr_array_add (self->lines, g_strdup (line));

  /* The oldest line goes first: a long game must not grow the log for
     ever. */
  while (self->lines->len > limit)
    g_ptr_array_remove_index (self->lines, 0);

  g_signal_emit (self, comfy_launcher_signals[comfy_LAUNCHER_SIGNAL_OUTPUT], 0,
                 line, g_strcmp0 (tag, TAG_STDERR) == 0);
}

guint
comfy_launcher_get_line_count (ComfyLauncher *self)
{
  g_return_val_if_fail (comfy_IS_LAUNCHER (self), 0);

  return self->lines->len;
}

char *
comfy_launcher_get_log_text (ComfyLauncher *self)
{
  GString *text;

  g_return_val_if_fail (comfy_IS_LAUNCHER (self), g_strdup (""));

  text = g_string_new (NULL);

  for (guint i = 0; i < self->lines->len; i++)
    {
      if (i > 0)
        g_string_append_c (text, '\n');

      g_string_append (text, g_ptr_array_index (self->lines, i));
    }

  return g_string_free (text, FALSE);
}

void
comfy_launcher_clear_log (ComfyLauncher *self)
{
  g_return_if_fail (comfy_IS_LAUNCHER (self));

  g_ptr_array_set_size (self->lines, 0);
}
