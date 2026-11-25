# Flatpak packaging

Builds Nuvasha Launcher from the `main` branch against the GNOME 51 runtime (`org.gnome.Platform//51`).

## Local build

Install `flatpak` and `flatpak-builder`, make sure Flathub is configured, and run from the repository root:

```sh
flatpak remote-add --user --if-not-exists flathub \
  https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install --force-clean \
  --install-deps-from=flathub \
  build/flatpak \
  packaging/flatpak/com.github.NuvashaLauncher.NuvashaLauncher.yml
flatpak run com.github.NuvashaLauncher.NuvashaLauncher
```
