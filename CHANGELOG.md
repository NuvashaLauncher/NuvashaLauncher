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

### Highlights
- Localized user-facing error handling across accounts, instances, Java, loaders, launching, content browsing, modpacks, skins, cache, and export flows.
- Replaced raw GUI filesystem, network, HTTP, service, and backend errors with translated summaries.
- Revealed previously hidden offline-account and bulk-operation failures.
- Improved Microsoft authentication state handling without invalidating saved credentials during temporary outages.
- Prevented invalid or oversized downloads from being retried unnecessarily.
- Fixed browser filter icon layout and translated filters across all locales, improved content browsing and keyboard shortcuts, refined skin uploads and selection controls, and fixed crash report detection on filesystems with coarse modification times.
- Added regression coverage for localized errors, authentication, downloads, skins, and launch failures.

### Build Verification
- Tests: **127 passed, 0 skipped**
- Version: `0.9` (`v0.9`)

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v0.8...v0.9

## [0.8] - 2026-10-28

Nuvasha Launcher 0.8 replaces the account JSON/keyring split with a complete Secret Service-backed account store and strengthens headless and sanitizer test coverage.

### Account Storage
- Store every account, including offline accounts, in the desktop Secret Service.
- Keep complete account records together: identity, access and refresh tokens, expiry, avatar, skin model, and cape.
- Store the active account ID in a separate Secret Service item.
- Persist account updates without overwriting other accounts, including skin and cape changes.
- Use new account and active-account schemas without migrating legacy JSON or keyring entries.

### Packaging and Documentation
- Add `libsecret` to native build dependencies and CI installation.
- Grant Flatpak access to `org.freedesktop.secrets`.
- Document the Secret Service account architecture and runtime requirement.
- Update the CLI branding and Flatpak version smoke test for the spaced "Nuvasha Launcher" name.

### Testing and Reliability
- Add persistence coverage for complete account records across restart.
- Cover account replacement, active-account transitions, authentication persistence, and cosmetic updates.
- Make account integration tests skip cleanly when Secret Service is absent.
- Fix sanitizer cleanup for skipped authentication tests and asynchronous launcher cleanup.
- Keep strict CI builds working with `-Werror`.

### Verification
- 121 tests pass in the normal test suite.
- Headless sanitizer runs pass with account/display-dependent tests skipped where their external services are unavailable.
- Static analysis passes without project warnings.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v0.7...v0.8

## [0.7] - 2026-10-27

Nuvasha Launcher 0.7 is a broad reliability, usability, testing, and native GTK polish release.

### User-Facing Improvements
- Improved Microsoft device-code sign-in behavior and testability.
- The sign-in verification code now uses a native libadwaita card surface instead of an accent-colored custom background.
- Sign-in errors remain neutral and readable instead of being forced into a red error style.
- Improved content and modpack details navigation, including clearer close and return behavior.
- Preserved instance versions that are no longer present in Mojang's current manifest.
- Prevented temporary version-selection changes from being treated as user selections while lists reload.
- Kept skin previews and avatar-derived skin entries stable while menus are rebuilt.
- Direct development launches now automatically find the build GSettings schema when `GSETTINGS_SCHEMA_DIR` is not configured.

### Reliability and Correctness
- Fixed file-descriptor leaks in modpack archive reading and loader JAR manifest patching.
- Improved archive extraction and native-file handling while preserving safe path checks.
- Fixed ownership of in-flight Modrinth JSON request keys so completed requests are released exactly once.
- Improved cleanup of generated skin texture data.
- Added explicit error checking for Java discovery test symlink creation.
- Fixed a GCC analyzer warning in native extraction cleanup.

### Testing and CI
- Added coverage for archive file descriptors, authentication transports, installed content, content identification/indexing/metadata, loader artifacts and processors, modpack details, Modrinth networking, and skin dialogs.
- Expanded download, instance-manager, modpack, window, and UI test coverage.
- Added isolated `XDG_RUNTIME_DIR` directories for test subprocesses.
- Kept LeakSanitizer enabled with narrow suppressions for confirmed third-party GTK, Pango, Fontconfig, Mesa, and EGL process-global state.
- Added Xvfb support and improved Flatpak CI execution on Ubuntu 26.04.
- Removed an unnecessary CI `g++` dependency and cleaned up compiler and static-analyzer warnings.
- Updated project documentation and architecture notes.

### Verification
- 121 tests pass in normal, debug, and LeakSanitizer modes.
- Static analysis passes without project warnings.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v0.6...v0.7

## [0.6] - 2026-10-26

0.6 splits several launcher modules, tightens filesystem/archive/cache handling, adds command-line controls for accounts and instances, and adds session caches for skins and capes. The technical branding was also unified under the `nuvashalauncher` application ID.

### Command-Line Controls
- New flags: `--list-instances`, `--list-accounts`, `--switch-account`, `--remove-account`, `--remove-instance`, and `--account` for per-launch account selection.
- Bash completion for the launcher's CLI (`nuvashalauncher`), installed with `make install`.
- CLI-only actions (`--version`, listing, removing, switching) now work without a display.

### Skins and Capes
- Skin and cape data is cached per launcher session, so previews stay available when the account menu is reopened without refetching.
- Cape caches are kept separate from saved skin assets to avoid collisions.

### Security
- Archive extraction and filesystem operations now validate paths and block traversal during deletion.
- Download cache and asset paths are validated.
- Process launch, termination, and launcher-finish paths handle failure cases.
- Runtime flows reauthenticate accounts when required.

### Branding and Identity
- Unified technical branding under the `nuvashalauncher` name; the application ID is now `com.github.nuvashalauncher.nuvashalauncher` (desktop file, GSettings schema, metainfo, MIME type, GResources, and UI resources follow suit).
- Window title finalized as "Nuvasha Launcher" and the logo asset corrected (two-line wordmark, no clipped README logo).

### Documentation and Packaging
- README updated with feature details and repository/Flatpak instructions.
- Architecture diagrams refreshed in `docs/architecture.md`.
- Flatpak: manifest builds from `main` (no more separate `flatpak` branch), MIME metadata is exportable, uninstall instructions documented.
- Account token storage and CLI controls documented.

### Development and CI
- Large modules split into separate translation units (java discovery/parse, resolve, skins service, tasks, main window, content pages, Modrinth requests, and more).
- Test suite reorganized into granular suites: 25 new test files covering loaders, modpacks, Modrinth, resolution, and versions; all suites pass.
- CI: sanitizer (ASan/UBSan) runtime install fixed, headless display-test workaround removed, Makefile skips `pkg-config` probing for non-building goals.
- Translation catalogs cleaned up across all 22 languages.

### Notes
- A Java runtime compatible with the Minecraft version you launch is still required; Flatpak builds do not bundle Java.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v0.5...v0.6

## [0.5] - 2026-10-25

### Flatpak
- Added a Flatpak manifest for the GNOME 51 runtime, with install instructions in the README and a packaging guide under `packaging/flatpak`.
- The Flatpak build uses the application sandbox Flatpak provides and does not ask for access to the home directory.
- Added a `FLATPAK=1` build flag for the few differences a sandboxed build has.
- Java runtime directories are shared by all instances instead of being picked per instance, and are remembered across restarts.
- Skin previews load over HTTP through libsoup, and local files through their portal, so the preview works inside the sandbox.
- Instance icons fall back to the bundled `pack.png` when there is no skin.
- Java runtimes report their vendor name.

### Removed
- Removed the nested bubblewrap sandbox: the sandbox modules, the sandbox settings and their UI, the dedicated test and the matching documentation and translations are gone.
- Removed libsecret: tokens are stored in `accounts.json` again, and the Flatpak no longer asks for a Secret Service.

### Interface and Documentation
- Export metadata now defaults to off.
- The AppStream metainfo gained a license, a description, a homepage and release entries.
- Updated the architecture documentation for the Flatpak boundary, and the README for the Flatpak install.

### Validation
- Full test suite passes: 38 test suites.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v0.4...v0.5

## [0.4] - 2026-10-25

### Instance Management
- Added instance edit mode with multi-selection checkboxes.
- Added bulk duplicate, delete, and export actions.
- Bulk export asks for one destination folder and writes one `.mrpack` per selected instance.
- Added drag-and-drop instance reordering with persistent ordering.
- Added row drag handles and row-shaped drag previews using the instance icon.
- Added selection footers with selected counts and bulk delete actions.
- Added circular instance icons with clipping for square source images.

### Installed Content
- Added multi-selection for installed mods, shader packs, and resource packs.
- Added bulk deletion with dependency information in the confirmation dialog.
- Added animated selection footers with bulk delete actions.
- Added circular icons for installed content and content browser results.
- Added a cancellable Update All progress popover with progress counts and a progress bar.

### Interface and Documentation
- Improved content toolbar spacing and action placement.
- Matched logs and content search-bar spacing.
- Added translations for the new interface and progress messages.
- Updated the architecture documentation for CLI launching, instance ordering, and content operations.

### Validation
- Full test suite passes: 39 test suites.

## [0.3] - 2026-10-24

### Branding & UI Architecture
- Renamed project to **Nuvasha** (`0cbfac3`) with updated application metadata, icons, repository links, and README logo lockup.
- Migrated launcher UI construction to compiled `GResource` GtkBuilder `.ui` templates and `AdwViewStack` pages (`727fc9b`, `ead8f5d`).
- Moved desktop preferences to `GSettings` (`568781c`) and added initial `libsecret` Secret Service token storage (`727fc9b`).
- Rebuilt the logs tab layout (`5c9b1b3`), fixed modpack browser Adwaita header and dialog sizing (`a5427ca`, `c3ce271`), and fixed instance list hover painting (`46773c7`).

### Accounts, Skins & Capes
- Added Minecraft skin and cape synchronization (`9c5b23b`) and centralized Minecraft account/skin services (`282e9c1`).
- Fixed slim skin classic arm rendering and upload throttling (`9bc2147`, `0e6e0b1`).
- Preserved selected offline skins and saved skins in account headers and rows (`a7693de`, `9bb1534`, `bc69561`), validated offline account names (`7e2c8d5`), and used the skin filename when exporting (`a8ce75b`).

### CLI, Modpacks & Documentation
- Added CLI instance launching (`472b47e`), live log auto-scrolling, and background work application hold (`3eb22c3`).
- Preserved modpack manifests across import/export round trips (`9312d85`) and hardened download validation, sandbox, and skin upload flows (`0a27ee6`, `63a2799`).
- Added comprehensive architecture documentation in `docs/architecture.md` (`86b14f7`).

## [0.2] - 2026-10-18

### 3D Skin Preview & Skin Management
- Added an OpenGL (`libepoxy`) 3D skin preview renderer (`c97a594`) with legacy 64x32 skin texture support (`2a7a049`) and fixed skin layer atlas dimensions (`81da60f`).
- Added an account skin management menu and redesigned skin manager dialog (`3eb09fc`, `d979170`) with cached account avatar textures (`b14e3d1`).

### Display Backends & Sandbox
- Added X11 display backend support alongside Wayland (`a0c700a`).
- Added an optional Bubblewrap (`bwrap`) game sandbox with isolated per-instance native libraries, restricted user sockets, GPU metadata passthrough, and a safe folder-opening broker (`a7f5c0b`, `e17d12a`, `a0d3681`, `de815d5`, `089f802`).

### Packaging, CI & Branding
- Updated application icon assets (`e774bf5`, `81420fc`), expanded Linux distribution installation guides, and migrated CI pipelines to GitHub Actions across Debian, Fedora, and Arch Linux (`e8bb956`, `bd91740`, `1997fc9`).

## [0.1] - 2026-10-14

Initial release of the launcher.

### Core Launcher & Instance Management
- GTK 4 and libadwaita Linux Minecraft launcher with isolated per-instance directories, atomic JSON persistence (`instances.json`), instance creation, duplication, deletion, safe directory moving, and custom instance icons.
- Mojang version manifest fetching, version inheritance and rules evaluation, library/native/asset resolution with SHA-1 verification, and `GSubprocess` game launching with crash detection.
- Live `stdout`/`stderr` game log viewer with configurable line limits, access token and offline UUID redaction, and `Ctrl+F` log search.

### Mod Loaders
- Built-in version listing and automated installation for **Fabric**, **Quilt**, **Forge**, and **NeoForge** (including non-GUI installer processor execution), with optional unstable/prerelease loader filtering.

### Modrinth Browser, Content Management & Modpacks
- Integrated Modrinth browser for **Modpacks**, **Mods**, **Resource Packs**, and **Shaders** with strict loader/Minecraft version compatibility filtering, category filters, Markdown project descriptions, and version selection.
- Local content management for installed mods, resource packs, and shaders with enable/disable toggles, indexed metadata caching, lazy icon loading, dependency resolution, optional cascade deletion or automatic disabling of dependent mods, and one-click **Update All**.
- Full Modrinth `.mrpack` modpack import and export with hash verification, override extraction, optional metadata inclusion, and rollback on failure.

### Accounts, Java Discovery & Localization
- Microsoft OAuth 2.0 device-code sign-in with Xbox Live / XSTS / Minecraft profile chain and offline account support.
- Automatic system Java runtime discovery across standard Linux paths, compatibility selection per Minecraft version, headless JRE warnings, and custom Java binary selection.
- Task-scope (`GTask`) asynchronous operation architecture, `libsoup-3.0` / `libarchive` download and extraction engine with retries and checksums, localization across 22 languages via `gettext`, and comprehensive unit, integration, and sanitizer test suites.
