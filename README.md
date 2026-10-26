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

## Installation

### Flatpak

The repository includes a Flatpak manifest using the GNOME 51 runtime. Build it
from the `main` branch:

```sh
git clone --branch main https://github.com/NuvashaLauncher/NuvashaLauncher.git
cd NuvashaLauncher
flatpak-builder --user --install --force-clean \
    build/flatpak \
    packaging/flatpak/com.github.nuvashalauncher.Nuvasha.yml
flatpak run com.github.nuvashalauncher.nuvasha
```

Nuvasha does not bundle a Java runtime. Add a compatible runtime in the
launcher and select it for an instance when needed.

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

### Runtime dependencies

- A Java runtime compatible with the Minecraft version being launched

#### Debian

```sh
sudo apt install build-essential pkg-config gettext \
    libglib2.0-dev libgtk-4-dev libadwaita-1-dev libepoxy-dev \
    libjson-glib-dev libsoup-3.0-dev libarchive-dev
```

#### Fedora

```sh
sudo dnf install gcc make gettext pkgconf-pkg-config \
    glib2-devel gtk4-devel libadwaita-devel libepoxy-devel \
    json-glib-devel libsoup3-devel libarchive-devel
```

#### openSUSE

```sh
sudo zypper install gcc make gettext-tools pkg-config glib2-devel \
    gtk4-devel libadwaita-devel libepoxy-devel json-glib-devel \
    libsoup-devel libarchive-devel
```

#### Arch

```sh
sudo pacman -S base-devel gettext pkgconf gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive
```

#### Gentoo

```sh
sudo emerge --ask sys-devel/gcc sys-devel/gettext dev-build/make \
    dev-util/pkgconf dev-libs/glib gui-libs/gtk:4 gui-libs/libadwaita \
    gui-libs/libepoxy dev-libs/json-glib net-libs/libsoup:3 \
    app-arch/libarchive
```

#### NixOS

```sh
nix-shell -p gcc gnumake gettext pkg-config glib gtk4 libadwaita libepoxy \
    json-glib libsoup libarchive
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
NuvashaLauncher --list-accounts
NuvashaLauncher --switch-account ACCOUNT_UUID_OR_NAME
NuvashaLauncher --remove-account ACCOUNT_UUID_OR_NAME
NuvashaLauncher --remove-instance INSTANCE_ID
NuvashaLauncher --launch INSTANCE_ID --account ACCOUNT_UUID_OR_NAME
NuvashaLauncher --version
```

`--launch` starts the instance while keeping Nuvasha running in the
background, without presenting its window. `.mrpack` files are also registered
with the desktop as files Nuvasha can open. Use `--list-instances` and
`--list-accounts` to find the UUIDs used by the other commands. Account names
are accepted where an account is specified. `--account` selects an account for
one launch without changing the active account.

If installed, Bash completion is available for `NuvashaLauncher` options and
file arguments.

Microsoft account tokens are stored in the user's `accounts.json` with mode
`0600`. Nuvasha must read the tokens to authenticate Minecraft, so keeping them
in a separate desktop secret store would not prevent the launcher from having
access to them during a launch.

## License

Nuvasha is free software released under the GNU General Public License,
version 3.0 only. See [LICENSE](LICENSE).
