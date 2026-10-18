# Nuvasha Launcher

Nuvasha Launcher is a Linux launcher for Minecraft. It manages separate
instances, installs content from Modrinth, and starts the game with the
selected Java runtime.

It uses GTK4 and libadwaita and works with both Wayland and X11.

## Features

- Separate instances with their own Minecraft version, loader, Java runtime,
  and directory.
- Modrinth browsing and installation for modpacks, mods, resource packs, and
  shader packs.
- Fabric, Quilt, Forge, and NeoForge support.
- Microsoft account sign-in and offline accounts.
- Logs for running games.
- Optional bubblewrap sandboxing.

## Requirements

- Linux
- GTK 4.10 or newer
- libadwaita 1.5 or newer
- A C compiler, Make, pkg-config, gettext, and the development packages for
  GLib, GTK, libadwaita, libepoxy, json-glib, libsoup, and libarchive

On Debian, install the dependencies with:

```sh
sudo apt install build-essential pkg-config gettext \
    libglib2.0-dev libgtk-4-dev libadwaita-1-dev libepoxy-dev \
    libjson-glib-dev libsoup-3.0-dev libarchive-dev
```

Fedora:

```sh
sudo dnf install gcc make gettext pkgconf-pkg-config \
    glib2-devel gtk4-devel libadwaita-devel libepoxy-devel \
    json-glib-devel libsoup3-devel libarchive-devel
```

openSUSE:

```sh
sudo zypper install gcc make gettext-tools pkg-config glib2-devel \
    gtk4-devel libadwaita-devel libepoxy-devel json-glib-devel \
    libsoup3-devel libarchive-devel
```

Arch:

```sh
sudo pacman -S base-devel gettext pkgconf gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive
```

Gentoo:

```sh
sudo emerge --ask sys-devel/gcc sys-devel/gettext dev-build/make \
    dev-util/pkgconf dev-libs/glib gui-libs/gtk:4 gui-libs/libadwaita \
    gui-libs/libepoxy dev-libs/json-glib net-libs/libsoup:3 \
    app-arch/libarchive
```

NixOS:

```sh
nix-shell -p gcc gnumake gettext pkg-config glib gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive
```

## Build and install

```sh
git clone https://github.com/nuvashalauncher/nuvasha-launcher.git NuvashaLauncher
cd NuvashaLauncher
make
sudo make install PREFIX=/usr/local
```

The launcher is installed as `/usr/local/bin/NuvashaLauncher`.

To uninstall it:

```sh
sudo make uninstall PREFIX=/usr/local
```

## Development

Run the test suite with:

```sh
make test
```

Other useful targets are `make debug`, `make test-debug`,
`make test-lsan`, `make static-analysis`, and `make clean`.

## Files

Nuvasha stores its data in:

- `~/.local/share/nuvasha`
- `~/.cache/nuvasha`
- `~/.config/nuvasha`

Account tokens are stored in the accounts file with owner-only permissions.
They are not encrypted.

## Translations

Translations are stored in `po/`. To update a translation, edit its catalog
and run `make`.

## License

Nuvasha is free software released under the GNU General Public License,
version 3.0 only. See [LICENSE](LICENSE).
