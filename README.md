<p align="center">
  <img alt="nuvasha launcher" src="data/nuvasha-logo.svg" width="40%">
</p>

<p align="center">
  Nuvasha Launcher is a simple Minecraft launcher that aims to make instance<br>
  management easy without leaving out the features that matter.
</p>

## Features

### Modpack management

Browse Modrinth and install compatible modpacks without manually assembling
their files. Import and export `.mrpack` files with verified downloads,
dependency handling, and safe archive extraction.

### Mod and content management

Find and manage individual mods, resource packs, and shader packs
through Modrinth. Content is organized inside each instance and can be enabled
or disabled without mixing files between setups.

### Instance management

Create separate Minecraft instances with their own worlds, mods, settings,
loader versions, Java selection, icons, and logs. Duplicate, rename, move,
export, and remove instances without affecting the others.

### Loader and version support

Install and resolve Fabric, Quilt, Forge, and NeoForge versions alongside
vanilla Minecraft. Nuvasha Launcher handles version metadata, libraries, assets, native
files, and loader-specific dependencies for each instance.

### Accounts, skins, and capes

Play with Microsoft or offline accounts, switch accounts per launch, and manage
accounts from the command line. Customize skins, skin models, and capes while
keeping previews available when the menu is reopened.

### Native Linux application

Built with GTK 4 and libadwaita, Nuvasha Launcher runs on Linux. It can be built
as a regular installation or packaged with Flatpak.

## Installation

### Flatpak

The repository includes a Flatpak manifest using the GNOME 51 runtime. Build it
from the `main` branch:

```sh
git clone --branch main https://github.com/nuvashalauncher/nuvashalauncher.git
cd nuvashalauncher
flatpak-builder --user --install --force-clean \
    build/flatpak \
    packaging/flatpak/com.github.nuvashalauncher.nuvashalauncher.yml
flatpak run com.github.nuvashalauncher.nuvashalauncher
```

To uninstall the Flatpak and remove its sandbox data:

```sh
flatpak uninstall --user --delete-data \
    com.github.nuvashalauncher.nuvashalauncher
```

### Native

#### Dependencies

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

#### Runtime dependencies

- A Java runtime compatible with the Minecraft version being launched
- A Secret Service provider, such as GNOME Keyring or KWallet

#### Debian

```sh
sudo apt install build-essential pkg-config gettext \
    libglib2.0-dev libgtk-4-dev libadwaita-1-dev libepoxy-dev \
    libjson-glib-dev libsoup-3.0-dev libarchive-dev libsecret-1-dev
```

#### Fedora

```sh
sudo dnf install gcc make gettext pkgconf-pkg-config \
    glib2-devel gtk4-devel libadwaita-devel libepoxy-devel \
    json-glib-devel libsoup3-devel libarchive-devel libsecret-devel
```

#### openSUSE

```sh
sudo zypper install gcc make gettext-tools pkg-config glib2-devel \
    gtk4-devel libadwaita-devel libepoxy-devel json-glib-devel \
    libsoup-devel libarchive-devel libsecret-devel
```

#### Arch

```sh
sudo pacman -S base-devel gettext pkgconf gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive libsecret
```

#### Gentoo

```sh
sudo emerge --ask sys-devel/gcc sys-devel/gettext dev-build/make \
    dev-util/pkgconf dev-libs/glib gui-libs/gtk:4 gui-libs/libadwaita \
    gui-libs/libepoxy dev-libs/json-glib net-libs/libsoup:3 \
    app-arch/libarchive app-crypt/libsecret
```

#### NixOS

```sh
nix-shell -p gcc gnumake gettext pkg-config glib gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive libsecret
```

#### Building from source

```sh
git clone https://github.com/nuvashalauncher/nuvashalauncher.git nuvashalauncher
cd nuvashalauncher
make
sudo make install PREFIX=/usr/local
```

The install target also refreshes the desktop entry, MIME, icon, and GSettings
schemas used by the launcher when the corresponding system tools are available.
For a staged or minimal installation, these caches can be refreshed manually:

```sh
sudo update-desktop-database /usr/local/share/applications
sudo update-mime-database /usr/local/share/mime
sudo gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
sudo glib-compile-schemas /usr/local/share/glib-2.0/schemas
```

The launcher is installed as `/usr/local/bin/nuvashalauncher`.

To uninstall it:

```sh
sudo make uninstall PREFIX=/usr/local
```

## License

Nuvasha Launcher is free software released under the GNU General Public License,
version 3.0 only. See [LICENSE](LICENSE).
