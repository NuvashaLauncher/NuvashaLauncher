/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NUVASHALAUNCHER_CLI_H
#define NUVASHALAUNCHER_CLI_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Registers the full CLI option set on @app so GApplication command-line
 * handling and --help stay consistent with headless CLI execution. */
void nuvashalauncher_cli_add_options (GApplication *app);

/* Parses @argc/@argv and executes headless CLI commands (including --help,
 * --version, instance/account management, cache clearing, export, and
 * --launch --headless) without requiring a display.
 *
 * Returns TRUE if the CLI handled the invocation and set *@exit_code;
 * returns FALSE when the GUI / GApplication flow should proceed. */
gboolean nuvashalauncher_cli_try_run (int *argc, char ***argv, int *exit_code);

G_END_DECLS

#endif /* NUVASHALAUNCHER_CLI_H */
