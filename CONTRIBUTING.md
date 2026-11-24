# Contributing to Nuvasha Launcher

Thank you for considering contributing to Nuvasha Launcher! Whether you are fixing a bug, improving accessibility, updating translations, or proposing a new feature, your help is welcome.

Before making substantial changes, please read [docs/architecture.md](docs/architecture.md) to understand module boundaries, asynchronous ownership rules, and security invariants.

## Ways to Contribute

- **Bug Reports:** Open an issue with your launcher version, installation format (Flatpak, DEB, RPM, Arch, or source build), Linux distribution, steps to reproduce, and relevant logs. (For security issues, see [SECURITY.md](SECURITY.md).)
- **Code & Bug Fixes:** Submit a Pull Request targeting `main`.
- **Translations:** Update or add gettext catalogs in `po/` (see [Translations](#translations) below).

## Building from Source

Nuvasha Launcher uses a standard `Makefile` and requires GTK 4, libadwaita, GLib/GIO, `json-glib`, `libsoup-3.0`, `libarchive`, `libsecret-1`, and `libepoxy`.

```bash
# Build release binary, resources, schemas, and locales
make -j$(nproc)

# Build debug binary with AddressSanitizer and UndefinedBehaviorSanitizer
make -j$(nproc) debug
```

## Running Tests & Checks

Every Pull Request must pass the full CI suite with zero compiler warnings (`-std=c17 -Wall -Wextra -Wpedantic -Werror`):

```bash
# Run unit and integration tests
make test

# Run tests under AddressSanitizer / LeakSanitizer
make test-debug

# Run GCC static analyzer (-fanalyzer)
make static-analysis

# Verify po/POTFILES.in includes all translatable source and UI files
make check-potfiles

# Repeat tests under an active Wayland/Broadway display session (when available)
make test-display
```

## Code Style & Architecture Rules

- **C Standard:** Write strict ISO C17 (`-std=c17`) with no compiler warnings.
- **GNOME HIG:** Follow the GNOME Human Interface Guidelines for UI layout, sentence-case labels, and standard Adwaita widgets (`AdwDialog`, `AdwAlertDialog`, `AdwToastOverlay`, `GtkActionBar`).
- **Async Ownership:** Never assume a widget that started an async task still exists when the callback runs. Use `NuvashaLauncherTaskScope` / `NuvashaLauncherTaskSequence` and keep long-lived operations owned by their manager or launcher.
- **Security Invariants:** Never bypass path traversal checks, symlink/hardlink protections (`O_NOFOLLOW`, `NOFOLLOW_SYMLINKS`), HTTPS/TLS verification, or checksum validation. Keep tokens and credentials out of logs and error messages.
- **License Headers:** Include `SPDX-License-Identifier: GPL-3.0-only` on new source and header files.

## Translations

Translatable strings in C code must be written in English using `_("...")`, `N_("...")` (for static tables), or `ngettext("...", "...", n)` (for plurals).

1. Regenerate the template and merge existing catalogs:
   ```bash
   xgettext --from-code=UTF-8 --keyword=_ --keyword=N_ --keyword=ngettext:1,2 \
     --files-from=po/POTFILES.in --output=po/nuvashalauncher.pot
   for f in po/*.po; do msgmerge --update --backup=none "$f" po/nuvashalauncher.pot; done
   ```
2. Edit the relevant `po/<lang>.po` file(s) and remove `#, fuzzy` markers once verified.
3. Validate with `msgfmt --check -o /dev/null po/<lang>.po` and run `make test TESTS=test-i18n`.

## Pull Request Process

1. Fork the repository and create a descriptive branch from `main`.
2. Keep commits focused and write clear commit messages (e.g., `fix(ui): ...`, `feat(modpack): ...`, `i18n: ...`).
3. Ensure `make test`, `make static-analysis`, and `make check-potfiles` pass before opening your PR.

## Contact

For general maintainer inquiries, you can reach out at `nuvashalauncher@gmail.com`.
