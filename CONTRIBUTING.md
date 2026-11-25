# Contributing to Nuvasha Launcher

Contributions—bug fixes, features, UI improvements, and translations—are welcome. Before making larger changes, read [docs/architecture.md](docs/architecture.md) for module boundaries, async ownership rules, and security invariants.

## Building from Source

Nuvasha Launcher uses a hand-written `Makefile` and depends on GTK 4, libadwaita, GLib/GIO, `json-glib`, `libsoup-3.0`, `libarchive`, `libsecret-1`, and `libepoxy`.

```sh
# Build the release binary, resources, schemas, and locales
make -j$(nproc)

# Build the debug binary with ASan and UBSan
make -j$(nproc) debug
```

## Running Tests & Checks

CI builds with `-std=c17 -Wall -Wextra -Wpedantic -Werror` and runs the following checks:

```sh
# Build with strict warnings and run unit/integration tests
make -j$(nproc) all test CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -MMD -MP"

# Run tests with AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer
make -j$(nproc) test-debug

# Run GCC static analysis (-fanalyzer)
make -j$(nproc) static-analysis

# Verify po/POTFILES.in includes all translatable source and UI files
make check-potfiles

# Run the suite against an active Wayland session (skips if unavailable)
make test-display
```

Run `make help` to see test filtering flags (`TESTS=`, `CASE=`, `V=1`, `FAIL_FAST=1`, `TIMEOUT=`, `REPEAT=`).

## Coding Guidelines

- **Standard:** Strict ISO C17 (`-std=c17`) with zero compiler warnings.
- **UI:** Follow the GNOME Human Interface Guidelines (sentence case, standard Adwaita widgets).
- **Async ownership:** Callbacks must not assume the widget that started an operation still exists. Use `NuvashaLauncherTaskScope` / `NuvashaLauncherTaskSequence` and keep long-lived operations owned by their manager or launcher.
- **Security:** Preserve path traversal checks, symlink/hardlink protections (`O_NOFOLLOW`, `G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS`), HTTPS/TLS verification, and checksum validation. Never log tokens or credentials.
- **License headers:** Add `SPDX-License-Identifier: GPL-3.0-only` to new source and header files.

## Translations

Mark user-facing strings in C code with `_("...")`, `N_("...")` (for static tables), or `ngettext("...", "...", n)` (for plurals).

1. Update the POT template and merge catalogs:
   ```sh
   xgettext --from-code=UTF-8 --keyword=_ --keyword=N_ --keyword=ngettext:1,2 \
     --files-from=po/POTFILES.in --output=po/nuvashalauncher.pot
   for f in po/*.po; do msgmerge --update --backup=none "$f" po/nuvashalauncher.pot; done
   rm -f po/nuvashalauncher.pot
   ```
2. Translate the strings in `po/<lang>.po` and remove any `#, fuzzy` markers after verifying them.
3. Validate with `msgfmt --check -o /dev/null po/<lang>.po` and `make test TESTS=test-i18n`.

## Pull Requests

1. Branch from `main` and keep changes focused.
2. Make sure `make test`, `make test-debug`, `make static-analysis`, and `make check-potfiles` pass before opening a PR.
3. For security issues, follow [SECURITY.md](SECURITY.md) instead of opening a public issue or PR.

## Contact

For general maintainer inquiries: `nuvashalauncher@gmail.com`.
