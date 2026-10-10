/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-launcher-private.h"
#include "aml-i18n.h"

#include <string.h>
#include <stdarg.h>

/* Internal launcher log buffer and formatting workflow. */

/* ---------------------------------------------------------- *
 * The log
 * ---------------------------------------------------------- */

guint
aml_launcher_get_line_limit (AmlLauncher *self)
{
  guint limit;

  g_return_val_if_fail (AML_IS_LAUNCHER (self), 0);

  /* Read for every line that comes in, so a limit the user changes
     while a game runs takes effect at once. */
  limit = self->settings != NULL
            ? aml_settings_get_log_line_limit (self->settings)
            : AML_SETTINGS_DEFAULT_LOG_LINE_LIMIT;

  return limit > 0 ? limit : AML_SETTINGS_DEFAULT_LOG_LINE_LIMIT;
}

/* One line of the log: the time it happened, where it came from and the
 * line itself. Everything a game prints goes through here, and so does
 * everything AML says about the launch. */
void
log_line (AmlLauncher *self,
          const char  *tag,
          const char  *format,
          ...) G_GNUC_PRINTF (3, 4);

void
log_line (AmlLauncher *self,
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

  stamp = now != NULL ? g_date_time_format (now, "%H:%M:%S")
                      : g_strdup ("--:--:--");

  /* The time it happened, where it came from and the line itself. */
  line = g_strdup_printf ("%s [%s] %s", stamp, tag, body);

  limit = aml_launcher_get_line_limit (self);

  g_ptr_array_add (self->lines, g_strdup (line));

  /* The oldest line goes first: a long game must not grow the log for
     ever. */
  while (self->lines->len > limit)
    g_ptr_array_remove_index (self->lines, 0);

  g_signal_emit (self, aml_launcher_signals[AML_LAUNCHER_SIGNAL_OUTPUT], 0,
                 line, g_strcmp0 (tag, TAG_STDERR) == 0);
}

guint
aml_launcher_get_line_count (AmlLauncher *self)
{
  g_return_val_if_fail (AML_IS_LAUNCHER (self), 0);

  return self->lines->len;
}

char *
aml_launcher_get_log_text (AmlLauncher *self)
{
  GString *text;

  g_return_val_if_fail (AML_IS_LAUNCHER (self), g_strdup (""));

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
aml_launcher_clear_log (AmlLauncher *self)
{
  g_return_if_fail (AML_IS_LAUNCHER (self));

  g_ptr_array_set_size (self->lines, 0);
}
