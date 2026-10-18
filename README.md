# Nuvasha Launcher

Nuvasha Launcher is a simple Minecraft launcher that aims to make instance
management easy without leaving out the features that matter.

## Features

- Modrinth browsing and installation for compatible modpacks, mods, resource
  packs, and shader packs.
- Automatic installation and version resolution for Fabric, Quilt, Forge, and
  NeoForge.
- `.mrpack` import and export with verified downloads and safe archive handling.
- Optional bubblewrap sandboxing for the game process.

## Dependencies

- C17-compatible compiler (GCC is used by default)
- GNU Make
- pkg-config
- gettext
- GLib
- GTK 4.10 or newer
- libadwaita 1.5 or newer
- libepoxy
- json-glib
- libsoup
- libarchive

Debian:

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
    libsoup-devel libarchive-devel
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
git clone https://github.com/NuvashaLauncher/NuvashaLauncher.git NuvashaLauncher
cd NuvashaLauncher
make
sudo make install PREFIX=/usr/local
```

The launcher is installed as `/usr/local/bin/NuvashaLauncher`.

To uninstall it:

```sh
sudo make uninstall PREFIX=/usr/local
```

## License

Nuvasha is free software released under the GNU General Public License,
version 3.0 only. See [LICENSE](LICENSE).
