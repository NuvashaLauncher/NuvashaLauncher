# Changelog

All notable changes to Nuvasha Launcher are documented in this file.

## [1.0] - 2026-11-07

### Packaging & Distribution
- **Linux Distribution Packaging**: Added native build recipes for Fedora/RHEL (`packaging/rpm/nuvashalauncher.spec`), Debian/Ubuntu (`packaging/debian/`), Arch Linux (`packaging/arch/PKGBUILD`), Gentoo (`packaging/gentoo/nuvashalauncher-9999.ebuild`), and NixOS (`flake.nix`).
- **Automated Release Workflow**: Added `.github/workflows/release.yml` to automatically build and publish `.deb` (`amd64`, `arm64`), `.rpm` (`x86_64`, `aarch64`), Arch Linux (`.pkg.tar.zst`), and Flatpak (`x86_64`, `aarch64`) packages to GitHub Releases whenever `VERSION` is updated.
- **NixOS Java Runtime Discovery**: Added `/nix/store` scanning (`*-openjdk-*/bin/java`, `*-temurin-*/bin/java`, `*-zulu-*/bin/java`, `*-graalvm-*/bin/java`, `*-jetbrains-jdk-*/bin/java`) and `$JAVA_HOME` probing so NixOS systems detect installed JDKs automatically.

### Bug Fixes & UI Improvements
- **Modpack Browser Close Crash Fix**: Fixed `g_task_get_cancellable: assertion 'G_IS_TASK (task)' failed` when closing the Modpack Browser dialog while an asynchronous operation was in progress by clearing dangling task pointers on completion and validating task lifetimes before cancellation.
- **Removed Custom JDK Auto-Deselection**: Deleting a custom-added JDK now automatically clears stale `java_path` selections from instances in memory and on disk (`instances.json`) and refreshes open settings pages so instances immediately fall back to auto-detected Java.
- **Content Toolbar Button Styling**: Fixed the "Update All" button in the Mods, Resource Packs, and Shaders tabs so it uses the `flat` icon button style and only highlights on hover.
- **GSettings Schema Discovery**: Installed and compiled GSettings schemas are now automatically discovered across custom prefixes (`PREFIX`), `XDG_DATA_DIRS`, and relative binary paths so package builds outside `/usr` never abort on missing schemas.

### Security, Stability & Portability
- **Archive & Filesystem Hardening**:
  - Blocked symlink traversal when deleting instance directories, copying files, or backing up `.mrpack` overrides.
  - Enforced `O_NOFOLLOW` when writing extracted archive files and rejected hardlinks during tar/zip extraction.
  - Bounded manifest JSON reads (64 MiB limit) and capped individual Modrinth `.mrpack` files at 512 MiB and total unpacked size at 4 GiB.
- **OAuth & Network Hardening**:
  - Replaced manual `sscanf` HTTP request parsing in the loopback OAuth callback server with `SoupServer`.
  - Validated `https://` schemes on download URLs, enabled strict TLS certificate checks, and rejected `..` path segments in Maven identifiers and remote version IDs.
- **Portable Instance Paths**: Default instance paths are now stored relative to the data directory and resolved dynamically at load time so moving a home directory or Flatpak sandbox preserves instances.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v0.9...v1.0

## [0.9] - 2026-10-30

- Improved error handling throughout the launcher with localized user-facing error messages across account management, instances, Java and loaders, Minecraft launching, content browsing, modpacks, skins, and settings.
- Presented technical failures with clear translated summaries, surfaced previously hidden errors, and handled temporary authentication or invalid-download failures more safely.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v0.8...v0.9

## [0.8] - 2026-10-28

- Migrated account records to the desktop Secret Service (`libsecret`), preserving offline and Microsoft identities, tokens, cosmetics, and active-account state.
- Improved account persistence tests, headless test handling, and sanitizer cleanup.

## [0.7] - 2026-10-27

- Improved sign-in, content navigation, instance version handling, and skin previews.
- Strengthened archive safety, download cleanup, Java discovery, and test coverage.
