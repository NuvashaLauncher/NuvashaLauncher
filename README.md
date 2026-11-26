<p align="center">
  <img alt="Nuvasha Launcher" src="data/nuvashalauncher-logo.svg" width="40%">
</p>

<p align="center">
  A lightweight, native Linux Minecraft launcher built with GTK 4 and libadwaita.
</p>

## Features

- **Instance management:** Keep separate Minecraft setups with their own worlds, datapacks, servers, mods, settings, mod loaders, Java runtimes, icons, and logs. Duplicate, rename, move, export, or delete instances independently.
- **Mod loaders & Java downloader:** Install and launch Vanilla, Fabric, Quilt, Forge, and NeoForge, and download verified Eclipse Temurin Java runtimes directly from inside the launcher with automatic recommended-version detection.
- **Modrinth browser & `.mrpack` support:** Search and install mods, resource packs, shader packs, per-world datapacks, and modpacks directly from Modrinth, or import and export local `.mrpack` files with checksum verification.
- **Worlds, servers & performance:** Manage worlds and per-world datapacks, check live server status/ping/player counts, reorder worlds and servers by dragging, configure global and per-instance launch defaults, and toggle dedicated GPU offload, GameMode, and MangoHud.
- **Accounts & cosmetics:** Sign in with Microsoft accounts or create offline accounts, switch accounts per launch, and manage skins, skin models, and capes.
- **CLI & scripting:** List or manage instances, accounts, worlds, and servers, or launch directly into a world or server from the command line (`nuvashalauncher --help`).

## Installation

### Flatpak

```sh
curl -fLO "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-$(uname -m).flatpak"
flatpak install ./nuvashalauncher-$(uname -m).flatpak
```

### Fedora

```sh
sudo dnf install "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-$(uname -m).rpm"
```

### Debian / Ubuntu

```sh
curl -fLO "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-$(dpkg --print-architecture).deb"
sudo apt install ./nuvashalauncher-$(dpkg --print-architecture).deb
```

### Arch Linux

```sh
sudo pacman -U "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/latest/download/nuvashalauncher-x86_64.pkg.tar.zst"
```

## Documentation

- [Building & contributing](CONTRIBUTING.md)
- [Architecture overview](docs/architecture.md)
- [Security policy](SECURITY.md)
- Packaging guides: [Flatpak](packaging/flatpak/README.md), [Debian/Ubuntu](packaging/debian/README.md), [Fedora/RPM](packaging/rpm/README.md), [Arch Linux](packaging/arch/README.md)

## License

Nuvasha Launcher is licensed under the GNU General Public License v3.0 only. See [LICENSE](LICENSE).
