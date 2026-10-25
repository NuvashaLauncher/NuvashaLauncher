<p align="center">
  <img alt="Nuvasha Launcher" src="data/nuvasha-logo.svg" width="40%">
</p>

<p align="center">
  Nuvasha Launcher is a simple Minecraft launcher that aims to make instance<br>
  management easy without leaving out the features that matter.
</p>

## Features

- Modrinth browsing and installation for compatible modpacks, mods, resource
  packs, and shader packs.
- Automatic installation and version resolution for Fabric, Quilt, Forge, and
  NeoForge.
- `.mrpack` import and export with verified downloads and safe archive handling.
- Optional bubblewrap sandboxing for the game process.

## Installation

### Dependencies

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
- libsecret
- bubblewrap (`bwrap`)

### Runtime dependencies

- A Java runtime compatible with the Minecraft version being launched

#### Debian

```sh
sudo apt install build-essential pkg-config gettext \
    libglib2.0-dev libgtk-4-dev libadwaita-1-dev libepoxy-dev \
    libjson-glib-dev libsoup-3.0-dev libarchive-dev libsecret-1-dev \
    bubblewrap
```

#### Fedora

```sh
sudo dnf install gcc make gettext pkgconf-pkg-config \
    glib2-devel gtk4-devel libadwaita-devel libepoxy-devel \
    json-glib-devel libsoup3-devel libarchive-devel libsecret-devel \
    bubblewrap
```

#### openSUSE

```sh
sudo zypper install gcc make gettext-tools pkg-config glib2-devel \
    gtk4-devel libadwaita-devel libepoxy-devel json-glib-devel \
    libsoup-devel libarchive-devel libsecret-devel bubblewrap
```

#### Arch

```sh
sudo pacman -S base-devel gettext pkgconf gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive libsecret bubblewrap
```

#### Gentoo

```sh
sudo emerge --ask sys-devel/gcc sys-devel/gettext dev-build/make \
    dev-util/pkgconf dev-libs/glib gui-libs/gtk:4 gui-libs/libadwaita \
    gui-libs/libepoxy dev-libs/json-glib net-libs/libsoup:3 \
    app-arch/libarchive app-crypt/libsecret sys-apps/bubblewrap
```

#### NixOS

```sh
nix-shell -p gcc gnumake gettext pkg-config glib gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive libsecret bubblewrap
```

## Building from source

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

## Command line

```sh
NuvashaLauncher                 # open the launcher
NuvashaLauncher --import pack.mrpack
NuvashaLauncher --launch INSTANCE_ID
NuvashaLauncher --list-instances
NuvashaLauncher --version
```

`--launch` starts the instance while keeping Nuvasha running in the
background, without presenting its window. `.mrpack` files are also registered
with the desktop as files Nuvasha can open. Use `--list-instances` to find the
UUID required by `--launch`.

## License

Nuvasha is free software released under the GNU General Public License,
version 3.0 only. See [LICENSE](LICENSE).
