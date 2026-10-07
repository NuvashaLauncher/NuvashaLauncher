# AML

Another Minecraft Launcher, for Linux.

AML installs and plays Minecraft: it creates instances, downloads
modpacks and mods from Modrinth, resolves and installs loaders, and runs
the game.

Requires a Wayland session. GTK4 must be built with Wayland support,
which is the default on all major distributions.

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

This installs the launcher and its desktop entry. Uninstallation is a
matter of removing `/usr/local/bin/AML` and
`/usr/local/share/applications/io.github.Flia7.AML.desktop`.

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
holds tokens and is written readable only by its owner.
