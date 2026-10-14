# AML

Another Minecraft Launcher, for Linux.

AML creates instances, installs modpacks and mods from Modrinth, resolves
mod loaders, and runs the game. It is written in C against GTK4 and
libadwaita, and it uses the public APIs of the services it talks to.

Requires a Wayland session. GTK4 must be built with Wayland support,
which is the default on all major distributions. X11 is not supported.

## Features

- Instances with their own directory, Minecraft version, mod loader and
  Java runtime.
- Modrinth browsing for modpacks, mods, resource packs and shader packs,
  filtered to what fits the instance it is opened for.
- Modpack installation from `.mrpack`, including loader resolution and the
  file overrides the pack declares.
- Fabric, Quilt, Forge and NeoForge.
- The game log of a running session, in a tab of its own.
- Microsoft account sign-in, or offline play under any name.

## Design notes

AML is Wayland only. It does not link, load or talk to X11 or XWayland,
and it exits when no Wayland display is available instead of falling back
to another backend.

There is no companion service, no updater and no telemetry. AML only makes
requests when a task needs them, and only to the service that task needs.

Downloads are checked against the hashes their manifest publishes before
being written into an instance. Archives that would escape the instance
directory are refused.

The build needs a Makefile and the libraries listed below. Nothing else.

## Translations

The interface uses gettext. Fourteen languages are shipped: Spanish
(Argentina and Spain), Brazilian Portuguese, French, German, Italian,
Japanese, Korean, Simplified Chinese, Russian, Polish, Ukrainian, Turkish
and Dutch.

The language follows the system by default and can be changed in Settings.
A new language takes effect on the next launch, because widgets hold the
strings they were built with. To correct or add a translation, edit the
catalogs in `po/` and run `make`.

`make test` checks that every string the launcher hands to gettext is
translated in every catalog, and that every translation takes the same
arguments as the string it translates. A string left in English reaches
the interface as English, so the catalogs are checked against the source
rather than against a file that has to be kept in step with it.

## Build dependencies

The build needs a C17 compiler, GNU make, and the development packages
for these libraries, named as `pkg-config` knows them:

- `gtk4`
- `libadwaita-1`
- `json-glib-1.0`
- `libsoup-3.0`
- `libarchive`
- `gio-unix-2.0`
- `gettext` (for `msgfmt`, which builds the translation catalogs)

On Debian and Ubuntu:

```
sudo apt install build-essential pkg-config \
    gettext \
    libgtk-4-dev libadwaita-1-dev libjson-glib-dev libsoup-3.0-dev \
    libarchive-dev
```

On Fedora:

```
sudo dnf install gcc make gettext pkgconf-pkg-config \
    gtk4-devel libadwaita-devel json-glib-devel libsoup3-devel \
    libarchive-devel
```

On Arch:

```
sudo pacman -S base-devel gettext pkgconf gtk4 libadwaita json-glib libsoup \
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

On Arch Linux, the repository includes a `PKGBUILD` for building a pacman
package from the current checkout. From the repository root, run:

```
makepkg -si
```

The `-s` option installs missing build dependencies and `-i` installs the
resulting package. The `PKGBUILD` packages the release binary, desktop entry,
metadata and translations under `/usr`; it does not run the test suite or
create a debug package. The generated package is named like
`aml-0.1-1-x86_64.pkg.tar.zst` and can also be installed directly with:

```
sudo pacman -U ./aml-0.1-1-x86_64.pkg.tar.zst
```

Run `makepkg -si` from the repository root. The PKGBUILD uses the checkout
directly so makepkg does not treat the tracked `src/` directory as a temporary
source tree.

## Tests

```
make test            # run every test against the release build
make test-debug      # the same tests under AddressSanitizer and UBSan
make test-lsan       # sanitizer tests with LeakSanitizer enabled
make static-analysis # GCC analyzer
```

Both are headless: no display is opened and no test touches the network
or the real data directory.

LeakSanitizer is the authority on ownership here: GLib's
`g_autoptr` and `g_autofree` hide their cleanup from the analyzer, which
reports those sites as leaks.

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

AML signs in with the Microsoft device code flow. The launcher shows a
code, the user enters it on a Microsoft page in their own browser, and the
launcher polls for the result. There is no embedded web view, and the
browser is never captured.

The client ID and tenant are compiled in as `AML_MICROSOFT_CLIENT_ID` and
`AML_MICROSOFT_TENANT` in `src/aml-auth-microsoft.h`. A build with an
empty client ID says so when a sign-in is attempted.

The scopes requested are `XboxLive.signin` and `offline_access`. The
refresh token is stored so a sign-in outlives the access token it starts
with. Accounts can be removed from the menu in the header.

## License

AML is free software, released under the GNU General Public License v3.0
only. See [LICENSE](LICENSE).
