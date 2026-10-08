# AML

Another Minecraft Launcher, for Linux.

AML installs and plays Minecraft: it creates instances, downloads
modpacks and mods from Modrinth, resolves and installs loaders, and runs
the game.

Requires a Wayland session. GTK4 must be built with Wayland support,
which is the default on all major distributions. X11 is not supported.

## About

AML is a launcher for the Java edition of Minecraft, written in C against
GTK4 and libadwaita. It uses the public APIs of the services it talks to
and has no companion service, updater or telemetry.

**Features**

- Instances with their own directory, Minecraft version, mod loader and
  Java runtime.
- Modrinth browsing for modpacks, mods, resource packs and shader packs,
  filtered to what fits the instance it is opened for.
- Modpack installation from `.mrpack`, including loader resolution and
  the file overrides the pack declares.
- Fabric, Quilt, Forge and NeoForge.
- The game log of a running session, kept in a tab of its own.
- Microsoft account sign-in, or offline play under any name.
- Translated interface; see **Translations** below.

**Design notes**

- Wayland only. AML does not link, load or talk to X11 or XWayland, and
  exits rather than falling back when no Wayland display is available.
- Instances, caches and settings live in the usual XDG directories. See
  [Where AML keeps its files](#where-aml-keeps-its-files).
- Downloads are checked against the hashes their manifest publishes
  before being written into an instance, and archives that would escape
  the instance directory are refused.
- No build system beyond a Makefile, and no dependency beyond the
  libraries listed below.

**Translations**

The interface uses gettext. Fourteen languages are shipped: Spanish
(Argentina and Spain), Brazilian Portuguese, French, German, Italian,
Japanese, Korean, Simplified Chinese, Russian, Polish, Ukrainian, Turkish
and Dutch.

The language follows the system by default and can be changed in
Settings, which takes effect on the next launch. To correct or add a
translation, edit the catalogs in `po/` and run `make`.

## Build dependencies

The build needs a C17 compiler, GNU make, and the development packages
for these libraries, named as `pkg-config` knows them:

- `gtk4`
- `libadwaita-1`
- `json-glib-1.0`
- `libsoup-3.0`
- `libarchive`
- `gio-unix-2.0`

On Debian and Ubuntu:

```
sudo apt install build-essential pkg-config \
    libgtk-4-dev libadwaita-1-dev libjson-glib-dev libsoup-3.0-dev \
    libarchive-dev
```

On Fedora:

```
sudo dnf install gcc make pkgconf-pkg-config \
    gtk4-devel libadwaita-devel json-glib-devel libsoup3-devel \
    libarchive-devel
```

On Arch:

```
sudo pacman -S base-devel pkgconf gtk4 libadwaita json-glib libsoup \
    libarchive
```

## Building

```
make
```

The launcher is `build/AML`.

To install it:

```
sudo make install PREFIX=/usr/local
```

This installs the launcher, its desktop entry and its translation
catalogs. To uninstall:

```
sudo make uninstall PREFIX=/usr/local
```

## Tests

```
make test          # run every test against the release build
make test-debug    # the same tests under AddressSanitizer and UBSan
```

Both are headless: no display is opened and no test touches the network
or the real data directory.

Other targets:

```
make debug         # a debug build with the sanitizers, for running AML
make clean         # remove build/ and build-debug/
```

## Where AML keeps its files

AML keeps its data under `~/.local/share/AML`, its caches under
`~/.cache/AML` and its settings under `~/.config/AML`. The accounts file
holds tokens and is written readable only by its owner, but the tokens
are not encrypted: anyone who can read your user's files can read them.

## Signing in

AML signs in with the Microsoft device code flow: the launcher shows a
code, the user enters it on a Microsoft page in their own browser, and the
launcher polls for the result. No embedded web view is involved and the
browser is never captured.

The client ID and tenant are compiled in as `AML_MICROSOFT_CLIENT_ID` and
`AML_MICROSOFT_TENANT` in `src/aml-auth-microsoft.h`. A build with an
empty client ID reports that when a sign-in is attempted rather than
failing silently.

The scopes requested are `XboxLive.signin` and `offline_access`. The
refresh token is stored so a sign-in outlives the access token it starts
with; accounts can be removed from the menu in the header.

## License

AML is free software, released under the GNU General Public License v3.0
only. See [LICENSE](LICENSE).
