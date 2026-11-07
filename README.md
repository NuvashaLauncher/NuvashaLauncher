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

```sh
curl -fLO "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-$(uname -m).flatpak"
flatpak install --user ./nuvashalauncher-$(uname -m).flatpak
```

### Fedora

```sh
sudo dnf install "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-$(uname -m).rpm"
```

### Debian

```sh
curl -fLO "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-$(dpkg --print-architecture).deb"
sudo apt install ./nuvashalauncher-$(dpkg --print-architecture).deb
```

### Arch

```sh
sudo pacman -U "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-x86_64.pkg.tar.zst"
```

### NixOS

```sh
nix profile install github:NuvashaLauncher/NuvashaLauncher
```

### Gentoo

```sh
sudo mkdir -p /var/db/repos/local/games-util/nuvashalauncher
sudo curl -fL "https://raw.githubusercontent.com/NuvashaLauncher/NuvashaLauncher/main/packaging/gentoo/nuvashalauncher-9999.ebuild" \
    -o /var/db/repos/local/games-util/nuvashalauncher/nuvashalauncher-9999.ebuild
sudo ebuild /var/db/repos/local/games-util/nuvashalauncher/nuvashalauncher-9999.ebuild manifest
sudo emerge --accept-keywords='**' games-util/nuvashalauncher
```

## License

Nuvasha Launcher is free software released under the GNU General Public License,
version 3.0 only. See [LICENSE](LICENSE).
