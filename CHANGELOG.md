# Changelog

All notable changes to Nuvasha Launcher are documented in this file.

## [2.0] - 2026-11-26

### Java Runtime Downloader
- **Built-in Eclipse Temurin Downloader**: Added an in-app Java downloader in Instance Settings that fetches Adoptium Eclipse Temurin releases for the host architecture (`x64` / `aarch64`), verifies SHA-256 checksums, extracts `.tar.gz` archives safely into the launcher data directory, and automatically detects installed runtimes.
- **Dynamic Recommended Java Label**: Automatically highlights the best Java major version for the instance's Minecraft version with `(recommended)` next to the version/LTS label.
- **Fail-Fast Java Verification**: Checks for a compatible Java runtime before downloading Minecraft game files and libraries.

### Per-World Datapacks Browser
- **Datapacks in World Menu**: Added a **Datapacks** option below **Duplicate** in each world's 3-dots menu that slides via `AdwNavigationView` (with a top-left `<` back button) into a full Modrinth datapack browser and **Installed** manager scoped to `<world>/datapacks`.
- **Hybrid Modrinth Datapack Support**: Queries Modrinth with `categories:datapack` and filters versions and updates with `loaders=["datapack"]` so both standalone datapacks and hybrid mod/datapack projects only offer compatible `.zip` datapack files, with local `pack.mcmeta` and `pack.png` metadata extraction.

### Global Launch Defaults & Performance Controls
- **Global Launch Defaults**: Added global defaults in **Settings → Launch defaults** for RAM (`2048 MiB`), JVM arguments, window width/height, fullscreen, **Use dedicated GPU**, **Use GameMode**, and **Use MangoHud**, with direct per-instance editing in Instance Settings.
- **Linux GPU Offload & Wrappers**: Launches Minecraft with `DRI_PRIME=1`, `__NV_PRIME_RENDER_OFFLOAD=1`, `__GLX_VENDOR_LIBRARY_NAME=nvidia`, and `__VK_LAYER_NV_optimus=NVIDIA_only` when dedicated GPU is enabled, and wraps execution with `gamemoderun` / `mangohud` when enabled and installed on `PATH` (showing **Not installed** when unavailable).

### Servers, Worlds & Browser Polish
- **Live Server Status Ping**: Added Minecraft Server List Ping (SLP) to the Servers tab displaying live online/offline status, latency in milliseconds, player counts, and fetched server favicons, alongside a 3-dots server action menu.
- **Floating-Ghost Drag Reordering**: Added smooth `GtkGestureDrag` row reordering with a floating ghost preview for both Servers and Worlds without Wayland `GtkDropTargetAsync` warnings.
- **Direct Version-Picker Downloads**: Updated version pickers across mods, resource packs, shader packs, datapacks, and modpacks to use a per-row **Download** button with an inline `AdwSpinner`, starting downloads directly without closing the dialog or showing redundant confirmation popups.
- **Persistent Batch Selection & Open Modpack Browser**: Multi-selected items across all Modrinth browsers now remain selected until **Install** is confirmed, and batch-installing modpacks starts downloads in the background without auto-closing the modpack dialog.
- **`pack.png` Default Icons & Instant Cached Scrolling**: Switched fallback icons for servers, mods, resource packs, shader packs, datapacks, and modpacks to `pack.png`, applied disk-cached icons synchronously on row creation, added an `800px` scroll preload margin, and increased the in-memory texture cache to 1024 entries to eliminate icon flicker during fast scrolling.
- **Extracted Folder Packs & Resource Pack Naming**: Detected extracted resource pack, datapack (`pack.mcmeta`), and shader pack (`shaders/`) directories alongside `.zip` archives in the **Installed** tab, and displayed installed resource packs by their folder/file name (excluding `.zip`).
- **Skin Dialog Polish & Instant Player Heads**: Hid the redundant `"Skin"` header title while keeping the close button, tightened dialog spacing, placed fetched Microsoft account skins at the top of the skin list, removed all bundled/auto-fetched Mojang default skins (`Steve`, `Alex`, etc.), kept the standard user icon on the main header button, and switched account heads in both the header dropdown and skin dialog to load immediately from cached skin PNGs (or over `libsoup` HTTP when uncached).
- **Faster First-Launch Downloads & Header Progress**: Increased concurrent transfer limits (`MAX_CONCURRENT` 8 → 24, `MAX_BULK_CONCURRENT` 6 → 20), added `[state]` log lines for `dependencies` and `assets` download stages, and tracked Minecraft dependency/native/asset downloads in the header downloads popover (`AdwSpinner`) with live counts and cancellation.
- **Auto-Dismissing Bottom Toasts**: Replaced sticky top error/sign-in banners in the main window and content pages with bottom `AdwToast` notifications that auto-dismiss after 5 seconds.
- **Microsoft Account Requirement & Asset/Trademark Compliance**: Required at least one Microsoft account to be stored before launching the game or creating offline accounts in both the UI and CLI (matching Prism Launcher / MultiMC), replaced `data/pack.png` with a grayscale custom placeholder image, and added the official Mojang non-affiliation disclaimer to `README.md` and AppStream metadata.

### Dependencies, Cache & Localization Fixes
- **Modpack & Fabric Dependency Tracking**: Persisted `local_id` and `provided_ids` (`provides` aliases and Jar-in-Jar bundled sub-modules from `fabric.mod.json`/`quilt.mod.json`) in `.index` and switched dependent lookup to transitive resolution, fixing dependent listing and cleanup for modpack mods (such as Fabric API dependents).
- **Disable Dependents Setting**: Added **Disabling a dependency disables the mod that depended on it** (off by default) in **Settings → General** across all 28 translation catalogs; re-enabling a dependency automatically re-enables the dependents it disabled.
- **No Duplicate Minecraft Assets in File Cache**: Skipped copying globally shared Minecraft `assets/`, `libraries/`, and `versions/` into `~/.cache/nuvashalauncher/files/`, eliminating duplicate cached audio/library files while keeping hash caching for per-instance content.
- **Clean Language Overrides**: Stopped mutating `LC_MESSAGES` when switching the interface language and terminated `LANGUAGE` with `:C`, eliminating `Locale not supported by C library` GTK warnings on non-system languages.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.9...v2.0

## [1.9] - 2026-11-25

### Security & Flatpak Sandbox
- **XDG Secret Portal in Flatpak**: Removed `--talk-name=org.freedesktop.secrets` from `packaging/flatpak/com.github.NuvashaLauncher.NuvashaLauncher.yml` so `libsecret` uses the sandboxed XDG Secret Portal instead of direct host keyring access.

### UI & Modpack Import
- **Headless Local `.mrpack` Import**: Importing a local `.mrpack` file now runs directly in the background via the header downloads indicator instead of opening the modpack browser dialog.
- **Version Picker Icon**: Replaced the download icon on mod, resource pack, shader pack, and modpack browser row install buttons with `pan-down-symbolic`.
- **Cleaner Empty States**: Removed small description subtitles from empty instance tabs (Mods, Resource packs, Shader packs, Worlds, Servers) and updated the empty instances prompt to "Press the + to create an instance." across all 28 translation catalogs (removing unused strings).

### Documentation & CI
- **Documentation Refresh**: Updated and streamlined `README.md`, `SECURITY.md`, `CONTRIBUTING.md`, `docs/architecture.md`, and packaging READMEs.
- **Headless Display Tests in CI**: Enabled display-backed GTK tests under `dbus-run-session` and `xvfb-run` in `.github/workflows/build.yml` on both `x86_64` and `ARM64`.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.8...v1.9

## [1.8] - 2026-11-24

### Flatpak & Display Compatibility
- **X11/XWayland Socket Access**: Replaced `--socket=fallback-x11` with `--socket=x11` in `packaging/flatpak/com.github.NuvashaLauncher.NuvashaLauncher.yml` so XWayland (`DISPLAY`) remains accessible inside the Flatpak sandbox on Wayland desktop sessions when Minecraft/LWJGL initializes GLFW over X11.

### Content Identification & Modpack Metadata
- **Automatic Modpack Content Metadata Fetch**: Updated `identify_start()` in `nuvashalauncher-content-page-identify.c` so installed files that already carry a Modrinth `project_id` from modpack indexing (`seed_manifest_indexes`) automatically resolve missing project titles (`project_name`) and icons (`icon_url`) from the browser cache or Modrinth API without requiring a separate content download.
- **Chunked Project Metadata Requests**: Batched `request_missing_project_titles()` requests in `nuvashalauncher-content-page-identify.c` into chunks of up to 50 project IDs (`PROJECT_TITLES_BATCH_SIZE`) to safely handle large modpacks.

### Instance Settings & Skin Dialog UI
- **Default RAM Display**: Added `output` and `input` handlers on the `RAM (MiB)` `AdwSpinRow` (`nuvashalauncher-instance-page-settings.c`) so instances with `0` configured memory display `Default` instead of `0`.
- **Skin Dialog Action Icons**: Added `data/icons/scalable/actions/import-symbolic.svg`, converted the **Import skin…** and **Export skin…** buttons in `data/ui/nuvashalauncher-skin-content.ui` and `nuvashalauncher-users-popover-skin.c` into icon buttons (`import-symbolic` and `export-symbolic`), and aligned all four action buttons to the right (`import`, `export`, `open-skins`, `remove`).

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.7...v1.8

## [1.7] - 2026-11-24

### Multi-Selection & Batch Installation
- **Browser Checkboxes & `GtkActionBar` Selection Footer**: Added a `GtkCheckButton` to each Modrinth result row for mods, resource packs, shaders, and modpacks (`nuvashalauncher-content-page-browser.c`, `nuvashalauncher-download-modpack-browser.c`), paired with a native `GtkActionBar` bottom selection footer showing the active selection count and an **Install** action button. Clicking anywhere on a result row toggles its selection checkbox.
- **Batch Content Install with Dependencies**: Selecting multiple mods, resource packs, or shaders resolves compatible versions and required dependencies in advance, presenting a unified confirmation dialog listing selected items and their dependencies cleanly before downloading (`nuvashalauncher-content-page-install.c`).
- **Batch Modpack Install**: Selecting multiple modpacks displays a confirmation dialog listing the chosen modpacks and installs each one asynchronously in the background (`nuvashalauncher-download-modpack-details.c`).

### Download UI, Instance Sidebar & Modpack Fixes
- **Modpack Instance Icon Fix**: Decoupled modpack icon fetching from dialog close cancellation and preserved `icon_url`/`icon_path` inside background `InstallJob` tasks (`nuvashalauncher-download-modpack-details.c`, `nuvashalauncher-download-modpack-install.c`, `nuvashalauncher-download-modpack-dialog.c`), ensuring downloaded modpacks properly copy and display their project icon.
- **Streamlined Row & Version Picker UX**: Removed the separate versions dropdown button on browser rows, routed the download button directly to the version picker, removed redundant download icons from version selector rows, unified confirmation prompts to always display **Install** even when replacing an existing file, and removed the version subtitle description from single-modpack install confirmation dialogs.
- **Instance Sidebar Ellipsization**: Added GNOME-standard `PANGO_ELLIPSIZE_END` truncation (with a 18-character max width and hover tooltip) to instance sidebar tabs (`nuvashalauncher-instance-page.c`) so long translated tab names truncate cleanly with `…`.

### Java & Runtime Discovery
- **Launcher Java Directory**: Automatically creates and scans `$XDG_DATA_HOME/nuvashalauncher/java` (and its Flatpak data directory equivalent) for installed Java runtimes (`nuvashalauncher-java.c`, `nuvashalauncher-java-discovery.c`, `main.c`).

### Localization & Documentation
- **Instance Sidebar Tab Translations**: Fixed missing `N_()` markers and sentence-case keys (`Resource packs`, `Shader packs`) in `nuvashalauncher-instance-page.c` so all instance sidebar tabs translate properly.
- **Translation Updates**: Added full plural-aware (`ngettext`) translations for all new multi-selection and batch install strings across all 28 gettext catalogs (`po/*.po`).
- **Architecture Documentation**: Updated `docs/architecture.md` to reflect `gtk_show_uri()` URI handling, multi-selection browser rows with `GtkActionBar` footers, and the dedicated launcher Java directory.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.6...v1.7

## [1.6] - 2026-11-24

### GNOME HIG Alignment & UI Polish
- **Header Bar Dialogs**: Modernized dialog windows (`nuvashalauncher-create-instance.ui`, `nuvashalauncher-offline-account.ui`, `nuvashalauncher-modpack-optional.ui`, `nuvashalauncher-modpack-error.ui`) by moving action buttons to `AdwHeaderBar` and eliminating legacy button boxes.
- **In-App Toast Notifications**: Integrated `AdwToastOverlay` into the main shell (`nuvashalauncher-main-shell.ui`, `nuvashalauncher-main-window-ui.c`) to provide non-destructive in-app toast feedback for copy and status operations.
- **Improved Empty States**: Upgraded empty state views across pages with clear actionable descriptions and pill-shaped action buttons.
- **HIG Typography & Sentence Casing**: Standardized interface text, dialog labels, error messages, and descriptions to adhere strictly to GNOME Human Interface Guidelines, adopting sentence casing and Unicode typographic quotation marks.
- **Icons & Visual Styling**: Standardized icon usage to `user-trash-symbolic`, refined squircle corner radii in `data/nuvashalauncher.css`, and updated the Help shortcut accelerator to `<primary>question`.

### Modpack Management & Download UX
- **Download Confirmation Dialog**: Added an `AdwAlertDialog` confirmation step when initiating modpack downloads from Modrinth (`nuvashalauncher-download-modpack-details.c`), displaying the pack title and version before beginning download and installation.

### Accounts & Secret Store
- **Unified Secret Password Search**: Switched account credentials lookup in `nuvashalauncher-secret-store.c` to `secret_password_search_sync()` with `account-id` attribute filtering, fixing offline and sandboxed account restoration across Flatpak and host desktop sessions.
- **Account Persistence Testing**: Expanded `tests/test-accounts.c` to validate secret store roundtrip persistence and retrieval for offline accounts alongside Microsoft accounts.

### Localization & Flatpak Packaging
- **Base Spanish & Portuguese Translations**: Added base Spanish (`po/es.po`) and base Portuguese (`po/pt.po`) catalogs alongside regional variants, and registered `"es"` and `"pt"` in the language selector (`nuvashalauncher-i18n.c`).
- **Translation Catalogs Synchronization**: Synchronized all 28 gettext translation files with the latest HIG string refinements and new dialog labels.
- **Flatpak Locale Packaging**: Updated Flatpak packaging workflows (`.github/workflows/flatpak.yml`, `.github/workflows/release.yml`) to include locale runtime extensions, ensuring translations render correctly in Flatpak builds.

### Bug Fixes & Build Cleanliness
- **Secret Service API Signature**: Fixed the `secret_password_search_sync()` call signature in `nuvashalauncher-secret-store.c` to properly terminate the attribute varargs list with `NULL`.
- **Header & Compiler Warning Fixes**: Resolved an unclosed comment in `nuvashalauncher-modrinth.h`, included missing `nuvashalauncher-modpack-private.h` in `nuvashalauncher-modpack-manifest.c`, and removed an unused variable in `nuvashalauncher-main-window-ui.c`.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.5...v1.6

## [1.5] - 2026-11-23

### UI Streamlining & Selection Simplification
- **Removal of Multi-Selection Overhead**: Removed complex multi-selection checkboxes, circular selection badge overlays, and sticky bottom selection footers across the Instance list, Worlds tab, Servers tab, and Installed Content tabs (`nuvashalauncher-content-page-installed.c`, `nuvashalauncher-worlds-page.c`, `nuvashalauncher-servers-page.c`, `nuvashalauncher-main-window-instance-row.c`).
- **Direct Interaction Model**: Clicking an instance row now selects it immediately without intermediate badge states or row selection mode, removing the separate settings button in favor of unified context actions and cleaner row layouts.
- **Cleanup**: Eliminated obsolete selection CSS classes from `data/nuvashalauncher.css` and removed outdated multi-selection helper modules (`nuvashalauncher-content-page-picks.c`, `nuvashalauncher-content-page-installed-delete.c`, `nuvashalauncher-ui-feedback.c`).

### Performance & Background Offloading
- **Worker-Thread JSON Parsing & Cache Encoding**: Offloaded HTTP JSON response parsing and Modrinth cache envelope serialization to background worker threads via `g_task_run_in_thread()` (`nuvashalauncher-download-json.c`, `nuvashalauncher-download-cache-operations.c`), preventing UI stutter during large API lookups and cache refreshes.
- **Worker-Thread Asset Pre-Verification**: Moved Minecraft asset index parsing and on-disk SHA1 verification (`file_matches_sha1`) to background worker threads (`nuvashalauncher-minecraft-resolve-assets.c`), pre-populating verified hashes to skip redundant downloads and avoiding blocking main-thread I/O.
- **Copy-on-Write (`FICLONE`) Acceleration**: Added kernel reflink cloning helpers (`nuvashalauncher_fs_try_clone_fd`, `nuvashalauncher_fs_try_clone_path` in `nuvashalauncher-fs.c`) across download cache restores, virtual asset directory copies, and world duplication, falling back gracefully to streaming copies when unsupported by the filesystem.
- **Async World Operations**: Converted blocking world directory deletion, world renaming, and ZIP extraction to asynchronous tasks (`nuvashalauncher-worlds-page.c`), keeping the UI responsive during large world imports or deletions.
- **Fast-Path Java Selection**: Skipped redundant candidate discovery sweeps during instance launch when an explicitly configured or currently selected Java path is already verified and valid (`nuvashalauncher-java.c`).
- **Icon Lookup & Caching**: Upgraded installed content icon resolution to an O(1) hash table lookup, optimized startup custom icon loading with an in-memory scaled icon cache (`nuvashalauncher-main-window-instance-row.c`), and eliminated redundant file stat lookups.
- **I/O & Link-Time Optimization**: Parallelized modpack extraction and loader artifact downloads; enabled Link-Time Optimization (`-flto=auto`) in production release builds.

### Codebase Normalization & Consistency
- **SPDX Headers & C Encapsulation**: Added standard `SPDX-License-Identifier: GPL-3.0-only` headers across 36 source and header files, and added `G_BEGIN_DECLS` / `G_END_DECLS` guards to all public library headers.
- **Task Scope Unification**: Replaced the legacy `NuvashaLauncherOperation` wrapper with direct `NuvashaLauncherTaskScope` references throughout the codebase.
- **Standardized UI Patterns**: Unified `AdwAlertDialog` dialog presentation using `adw_alert_dialog_choose()` and explicit markup strings, standardized delete icon naming to `"edit-delete-symbolic"`, normalized instance GObject property names with hyphens (`java-path`, `java-version`, `java-auto`, `custom-icon`), and precomputed lowercase search keys for Worlds and Servers filtering.
- **Settings & Launching**: Centralized `GSettings` access through `nuvashalauncher_settings_open_gsettings()` and switched external link launching to asynchronous `gtk_show_uri()`.

### Test Suite & CI Hardening
- **Broadway Headless Display Fix**: Installed structured log filtering in `tests/test-filter-entry.c` via `g_log_set_writer_func()` to handle Broadway frame clock duplicate timing warnings (`gdk_frame_timings_submitted`), allowing display tests to pass cleanly in headless environments.
- **New Unit Test Coverage**: Added dedicated unit tests for kernel `FICLONE` reflink helpers in `tests/test-fs.c`, worker-thread JSON cache encoding and error handling in `tests/test-download-cache.c`, and worker-thread asset pre-verification in `tests/test-resolve-integration.c`.
- **Zero Warnings & Sanitizer Clean**: Maintained strict zero compiler warnings (`-std=c17 -Wall -Wextra -Wpedantic -Werror`), full pass under GCC `-fanalyzer` static analysis, and clean AddressSanitizer/UBSan/LeakSanitizer test runs.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.4...v1.5

## [1.4] - 2026-11-23

### Instance Management & UI
- **Sidebar Groups & Stats Tab**: Added instance grouping in the sidebar (`nuvashalauncher-instance-manager.c`, `nuvashalauncher-main-window.c`) and a new **Stats** tab (`nuvashalauncher-instance-page-stats.c`) in the instance view, with updated translations across all 26 `po/*.po` files.

### Reliability & Bug Fixes
- **Task Scope & Download Cache**: Fixed a race between worker completion and `unref_when_done` in `nuvashalauncher-task.c`, and fell back to network downloads when a cached file disappears during restore in `nuvashalauncher-download-cache.c`.
- **Isolated Test Secret Store**: Implemented `NUVASHALAUNCHER_TEST_SECRET_STORE_FILE` JSON storage in `nuvashalauncher-secret-store.c` when `NUVASHALAUNCHER_TEST_SECRET_STORE=1`, preventing parallel test runs from racing on the host Secret Service.
- **Display Test Fixes**: Added `adw_init ()` in `tests/test-filter-entry.c` for `AdwHeaderBar` UI loading, and fixed `gdk_frame_clock_get_frame_time` teardown criticals and slow polling in `tests/test-window.c`.

### Build, Tests & CI
- **Pure C17 Parallel Test Runner & UX**: Replaced the Python test runner with `tests/runner.c` featuring live `[done/total]` progress, TTY colors, skip-reason extraction, failure highlighting/filtering, preserved failing workdirs, `Reproduce:` hints, and `TESTS=`, `CASE=`, `V=1`, `FAIL_FAST=1`, `KEEP_TMP=1`, `TIMEOUT=`, `REPEAT=`, `make test-list`, and `make help`.
- **Faster Builds & Test Execution**: Linked `make test` against `build/libnuvashalauncher.so` (preserving `-fPIC` on `RELFLAGS` overrides), skipped building `build/nuvashalauncher` when filtered tests don't invoke it, removed `make fast`, parallelized stateless CLI checks in `tests/test-cli.c`, lowered `NUVASHALAUNCHER_TEST_FAST_TIMERS` stop-timeout to `100 ms`, lowered the `test_archive_size_bomb_is_refused` fixture cap, and consolidated tiny unit tests (`tests/test-common.h`).
- **CI & Packaging**: Parallelized `static-analysis` across `x86_64` and `ARM64`, added `ccache` and `-Werror` on tests in `build.yml`, switched `flatpak.yml` and `release.yml` to direct `flatpak-builder` + `actions/cache@v4` (dropping `xvfb`, adding `elfutils`), and streamlined DEB/RPM/Arch release packaging (`--nocheck`, `debug_package %{nil}`, `nodocs`).

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.3...v1.4

## [1.3] - 2026-11-15

### Main Window & Instance Selection
- **Direct Multi-Selection & Row Settings Button**: Removed the main window's old edit mode (header edit toggle, `Ctrl+E`, per-row edit action buttons, and drag-and-drop row reordering). Clicking an instance row now toggles its circular selection badge directly and reveals the selection footer (`Delete`, `Export`, `Escape` to clear, `Ctrl+D` to delete), while a new Settings gear button (`emblem-system-symbolic`) to the left of the Play button opens instance settings.

### Instance Settings, Content Tabs & UI Polish
- **Easy Java Memory Setting**: Added a **Memory (MiB)** spin row (`memory-mib` property persisted in `instances.json`) to the Java section of instance settings. Explicit `-Xmx` flags in custom Java arguments take precedence over the memory setting.
- **Worlds & Servers Multi-Selection**: Added circular selection checkboxes and a shared selection footer (`nuvashalauncher-ui-feedback`) to the Worlds and Servers tabs for bulk deletion.
- **Search Bars, Spacing & Empty States**: Reduced excess top spacing in the Settings tab, improved header search entries across Settings and Mods/Resource Packs/Shader Packs/Datapacks tabs, and centered empty-state status pages in the Worlds tab and the "No Mod Loader" state.
- **Worlds Tab Cleanup**: Removed the "Reset Icon" option from world row menus.
- **GTK Critical Fixes**: Fixed `GTK_IS_WIDGET` assertions in content page UI and icon handling (`nuvashalauncher-icons.c`, `nuvashalauncher-content-page-ui.c`, `nuvashalauncher-content-page-identify.c`).

### Command-Line Interface
- **World & Server Listings**: Added `--list-worlds INSTANCE` and `--list-servers INSTANCE` CLI flags (with `--json` output support) to list an instance's singleplayer worlds and saved servers.

### Binary, Packaging & Tests
- **Lowercase `nuvashalauncher` Binary**: Renamed the built and installed binary and CLI command from `NuvashaLauncher` to `nuvashalauncher` across `Makefile`, `.desktop`, bash completion, RPM spec, Flatpak manifest, and Nix `flake.nix`.
- **Tests**: Updated `test-cli`, `test-desktop-entry`, `test-launch-args`, and `test-window` for the new CLI listing flags, `-Xmx` precedence, `Exec=nuvashalauncher %f`, and row selection/settings button behavior.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.2...v1.3

## [1.2] - 2026-11-14

### Instance Management & Content
- **Datapacks, Worlds & Servers Tabs**: Added dedicated `Datapacks`, `Worlds`, and `Servers` tabs to the instance view (`Settings`, `Resource Packs`, `Shader Packs`, `Datapacks`, `Worlds`, `Servers`, `Mods`, `Logs`) with `level.dat` / `servers.dat` NBT parsing (`nuvashalauncher-nbt`), server editing/reordering/deletion, world import/deletion, and direct QuickPlay launch into a world or server.
- **Lazy Version Loading & UI Responsiveness**: Added incremental/lazy loading for Modrinth version selectors in the modpack browser and content details to prevent UI stalls on projects with many versions, prevented duplicate version-selector popovers on rapid clicks, and shortened the modpack browser height.
- **Translations**: Added localized strings for the new tabs, actions, and QuickPlay controls across all 26 `po/*.po` translations.

### User Interface & Search
- **Unified Header Bar & Search**: Moved action buttons (`Update All`, `Open Folder`, filter controls, `Installed | Browse` switcher, `+`, `Refresh`, `Clear`) into the top `AdwHeaderBar` with `ADW_CENTERING_POLICY_STRICT`, placed the magnifying glass toggle at the top-left of the sidebar, and added centered header search across `Settings`, `Content` (`Installed` and `Browse`), `Worlds`, `Servers`, and `Logs`.

### Command-Line Interface
- **Headless `--help` & Option Parsing**: Replaced manual argument checks in `main.c` with `GOptionContext`-based parsing in `nuvashalauncher-cli.c`, allowing `--help` (`-h`, `-?`) and all CLI commands to run without a display.
- **Name-Based Instance Lookup**: Added `nuvashalauncher_instance_manager_find` so `--launch`, `--remove-instance`, and `--export-instance` accept either an instance UUID or display name.
- **Richer Listings & JSON Output**: Expanded `--list-instances` (UUID, name, Minecraft version, loader) and `--list-accounts` (UUID, name, account type, active marker), and added `--json` output support to both commands.
- **Safer Instance Removal**: Added `--keep-files` to `--remove-instance` so an instance can be unregistered while preserving its directory on disk.
- **Direct World & Server Launch**: Added `--server ADDRESS` (`-s`) and `--world WORLD` options to `--launch` (both GUI and `--headless`) using QuickPlay.
- **New CLI Commands**:
  - `--create-instance NAME --mc-version VERSION [--loader LOADER] [--loader-version VERSION]` to create instances headlessly.
  - `--export-instance INSTANCE --output FILE` (`-o`) to export instances as `.mrpack` archives from the CLI.
  - `--add-offline-account NAME` to create and activate an offline account.
  - `--clear-cache` to clear the global download cache without opening the GUI.
  - `--launch INSTANCE --headless [--account ACCOUNT]` to run an instance headlessly, streaming output to stdout/stderr and exiting with the game status.

### Build & Packaging
- **CI & Packaging Compatibility**: Fixed a GCC predefined `linux` macro collision in `tests/test-resolve-metadata.c` when packaging builds omit `-std=c17`, and guarded Secret Service CLI tests in `tests/test-cli.c` for headless containers without a Secret Service daemon.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.1...v1.2

## [1.1] - 2026-11-14

### Performance, Caching & Network Efficiency
- **Mojang Manifest & Modrinth Caching**:
  - Routed Minecraft version manifest fetches through `nuvashalauncher_download_json_metadata_cached_async` (TTL + ETag revalidation), eliminating redundant disk writes and network requests.
  - Added in-flight `json_flights` deduplication for Modrinth version and category requests via `nuvashalauncher_modrinth_get_with_data_async`.
  - Batched Modrinth content update checks into a single POST request and reused cached lookup results for modpacks, loaders, and Java probes.
  - Skipped parsing the inner `"response"` payload when checking expired API cache entries, avoiding double JSON parsing on stale cache hits.
- **Download & Cache I/O**:
  - Removed redundant post-download SHA re-hashing when publishing from the global hash cache.
  - Replaced unconditional sequential `mkdir` calls on every API/global cache and content index write with lazy directory creation on `G_IO_ERROR_NOT_FOUND`.
  - Respected HTTP 429 `Retry-After` headers when uploading skins in `nuvashalauncher-skin-service.c`.
  - Skipped re-downloading Forge/NeoForge installer JARs when a valid non-symlink installer already exists locally.
- **Content Scanning, Metadata & Instance Monitoring**:
  - Consolidated archive metadata and icon extraction into a single archive pass (`read_archive_members`) and cached scaled local icons on disk.
  - Coalesced `.index` writes during content identification (50 ms batching), updated `.index` and filenames directly on `.disabled` toggles, and removed redundant synchronous existence checks during scans.
  - Moved Java runtime candidate discovery off the GTK main thread into a worker thread, parallelized Java probing, and deduplicated custom Java path checks.
  - Replaced fast polling in `NuvashaLauncherInstanceManager` with `GFileMonitor` directory watching plus a 30-second fallback, single-pass `instances.json` loading, and batched cleanup saves.
- **UI Search & Filtering**:
  - Debounced Modrinth category toggles and modpack filter/sort changes, skipped unchanged search queries, and parsed Modrinth search responses directly from `JsonNode` without re-serializing JSON.
  - Updated installed content row refreshes to re-evaluate only the changed row rather than re-filtering the entire list.

### User Interface & Branding
- **Visual Polish & Assets**: Updated AppStream screenshots metadata, refined selection badges and row layouts, redesigned the README logo, and normalized the logo filename (`nuvashalauncher-logo.svg`).
- **Markdown Renderer**: Fixed task scope, markup, and memory leak bugs and improved inline/block rendering.

### Architecture, Refactoring & Build Tooling
- **Module Consolidation & Normalization**: Normalized app ID, GResource paths, binary name, and symbol prefixes across the codebase; deepened and consolidated Microsoft auth, skin service, content models/pages, loader install, download plumbing, and main-window model modules.
- **Build & QA Tooling**: Cleaned up the `Makefile`, refactored auth CLI/options, removed unused `.clang-format`, and added `make check-potfiles` to verify `po/POTFILES.in` stays in sync.

**Full Changelog**: https://github.com/NuvashaLauncher/NuvashaLauncher/compare/v1.0...v1.1

## [1.0] - 2026-11-08

### User Interface & GNOME Experience
- **Microsoft Sign-In Dialog**: Added an `external-link-symbolic` icon to the right of the "Open in browser" action button.
- **Instance Export Actions**: Replaced legacy save icons with GNOME standard `export-symbolic` across instance row actions, footer selection actions, and the instance settings page.
- **Modernized Loading Indicators**: Replaced small spinners and redundant "Loading…" labels with centered 64×64 `AdwSpinner` widgets in the modpack browser, modpack detail pages, and content details (mods, resourcepacks, shaderpacks) views, aligned with GNOME application design.
- **Bundled Symbolic Action Icons**: Bundled action icons (`export-symbolic` and `external-link-symbolic`) into application GResources and system icon directories, registering them dynamically with the GTK icon theme.

### Packaging & Distribution
- **Linux Distribution Packaging**: Added native build recipes for Fedora/RHEL (`packaging/rpm/nuvashalauncher.spec`), Debian/Ubuntu (`packaging/debian/`), Arch Linux (`packaging/arch/PKGBUILD`), and NixOS (`flake.nix`).
- **Streamlined Distro Dependencies**: Cleaned package definitions across RPM, Debian, Arch Linux, and NixOS by removing distro-specific optional Java dependencies in favor of user-managed JDKs and built-in Java discovery.
- **Automated Release Workflow**: Added `.github/workflows/release.yml` to automatically build and publish `.deb` (`amd64`, `arm64`), `.rpm` (`x86_64`, `aarch64`), Arch Linux (`.pkg.tar.zst`), and Flatpak (`x86_64`, `aarch64`) packages to GitHub Releases whenever `VERSION` is updated.
- **NixOS Java Runtime Discovery**: Added `/nix/store` scanning (`*-openjdk-*/bin/java`, `*-temurin-*/bin/java`, `*-zulu-*/bin/java`, `*-graalvm-*/bin/java`, `*-jetbrains-jdk-*/bin/java`) and `$JAVA_HOME` probing so NixOS systems detect installed JDKs automatically.

### Codebase Audit, Stability & Async Runtime Hardening
- **Core Async Task & Download Runtime**:
  - Prevented Use-After-Free in the download service by keeping strong task scope references across asynchronous transfers, SHA1/SHA256 hashing, and icon loading operations.
  - Cleared `queue_cancel_source` properly before removal to prevent stale event sources from firing against destroyed download requests.
  - Hardened task sequence cancellation and teardown to ensure scopes outlive in-flight operations safely without leaking state or memory.
  - Replaced shell command string formatting with direct `argv` spawning in GSettings schema resolution to guard against command injection.
  - Preserved `errno` values across filesystem error reporting in `fs` and `fs-tree` before invoking GLib helpers.
  - Fixed `-Wcast-function-type` compiler error in `nuvashalauncher_cancellable_watch` by utilizing GLib's `G_SOURCE_FUNC` macro.
- **Authentication, Accounts & 3D Skin Rendering**:
  - Prevented skin uploads from clearing the user's currently selected cape when no cape change was requested.
  - Released task scopes properly across asynchronous Minecraft profile and skin service requests, eliminating memory leaks.
  - Resolved `SecretValue` memory leaks in `nuvashalauncher_secret_store_load` by releasing referenced secrets after reading payloads.
  - Handled 0-byte HTTP responses and preserved specific Xbox and Microsoft authentication error domains without crashing or misreporting as generic service errors.
  - Eliminated memory leaks, dangling pointer references, and use-after-free hazards in the skin library dialog and texture generation.
  - Fixed Red/Blue channel swap (`GDK_MEMORY_DEFAULT`), left-arm slim model texture alignment, 64x32 cape transparency, and HiDPI `glViewport` scaling in the OpenGL 3D skin preview.
  - Ensured offline account dialogs and account manager activations properly retain and release account stores and persist active IDs.
- **Instances, Launcher, Java Discovery & Loaders**:
  - Fixed memory leak of patched JAR file names during loader construction.
  - Resolved zlib `deflateInit2` stream buffer leaks in loader JAR manifest attribute patching by cleanly letting libarchive's write close and free handlers finalize compression.
  - Guarded custom Java probe completion and process stop operations against cancellation races, ensuring instance crash logs and exit states are preserved.
  - Cleaned up dangling export weak references and cancelled background export operations on instance page disposal.
  - Added force-exit fallbacks when Java runtime discovery and loader processor subprocess pipes fail, preventing orphaned background processes.
  - Deduplicated asset hash downloads so multiple virtual paths sharing identical content hashes download only once.
  - Fixed bounds checks in loader selection, regex cache thread synchronization in version rule checks, and instance duplication metadata preservation.
- **Modpacks & Modrinth Integration**:
  - Fixed silent dialog sink in `nuvashalauncher_main_window_import_file` so modpack import dialogs present reliably to the user.
  - Fixed memory leak of Modrinth JSON response strings and guarded against out-of-bounds version index selections.
  - Guaranteed dialog states complete and refresh on network or parsing errors so modpack dialogs never freeze on indefinite loading spinners.
  - Removed dangling weak references, cleaned up temporary `.mrpack` files on extraction failure, and guarded division-by-zero during empty export runs.
- **Content Management, Main Window & Markdown**:
  - Removed orphaned and dead UI template (`nuvashalauncher-downloads-popover.ui`) and purged unreferenced functions across cache, fs, accounts, and i18n modules.
  - Fixed non-compatible function pointer casting (`GDestroyNotify`) in `nuvashalauncher_content_page_dispose` to prevent compiler warnings and crashes on strict GCC configurations.
  - Handled projects with no compatible versions gracefully using localized messaging rather than out-of-bounds access.
  - Preserved existing browser items when subsequent paginated searches fail, allowing seamless retry without clearing results.
  - Prevented thread pool dispatch failures from deadlocking pending operation counters in content file scanning.
  - Balanced nested parentheses in Markdown link parsers to ensure URLs containing parentheses (e.g. Wikipedia articles) parse accurately.
  - Fixed window close request handling to prioritize background download checks before accessing window settings.

### Security, Stability & Portability
- **Archive & Filesystem Hardening**:
  - Blocked symlink traversal when deleting instance directories, copying files, or backing up `.mrpack` overrides.
  - Enforced `O_NOFOLLOW` when writing extracted archive files and rejected hardlinks during tar/zip extraction.
  - Bounded manifest JSON reads (64 MiB limit) and capped individual Modrinth `.mrpack` files at 512 MiB and total unpacked size at 4 GiB.
- **OAuth & Network Hardening**:
  - Replaced manual `sscanf` HTTP request parsing in the loopback OAuth callback server with `SoupServer`.
  - Validated `https://` schemes on download URLs, enabled strict TLS certificate checks, and rejected `..` path segments in Maven identifiers and remote version IDs.
- **Portable Instance Paths**: Default instance paths are now stored relative to the data directory and resolved dynamically at load time so moving a home directory or Flatpak sandbox preserves instances.

### Verification & Testing
- **Comprehensive Headless Test Suite & Coverage**:
  - Expanded non-display backend test coverage across the entire codebase to over 70%.
  - Added test coverage for Modrinth async hash verification, modpack archive security, invalid argument validation, icon installation, and archive removal.
  - Added unit tests for Markdown HTML named and numeric character entities, inline tags, entity safety, and URL sanitization.
  - Added unit tests for in-memory JAR icon loading, mod dependency merging, and async background thread-pool metadata scanning.
  - Added unit tests for launcher loader manifest failures, unsupported loaders, Java runtime candidate selection, preset Java execution, and process failure states.
  - Added unit tests for filesystem directory creation, atomic file replacements, renames, and loader profile error diagnostics.
  - Fixed latent use-after-free in inline Markdown entity string conversion and synchronous task sequence completion during Java probe list traversal.

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
- Unified technical branding under the `nuvashalauncher` name; the application ID is now `com.github.NuvashaLauncher.NuvashaLauncher` (desktop file, GSettings schema, metainfo, MIME type, GResources, and UI resources follow suit).
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
- Renamed project to **Nuvasha Launcher** (`0cbfac3`) with updated application metadata, icons, repository links, and README logo lockup.
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
