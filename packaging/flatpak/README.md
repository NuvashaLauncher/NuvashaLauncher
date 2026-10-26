# Flatpak packaging

The manifest builds nuvashalauncher with the GNOME 51 runtime and the Flatpak-specific
build settings.

## Local build

Install `flatpak` and `flatpak-builder`, then run from the repository root:

```sh
flatpak-builder --user --install --force-clean \
  build/flatpak \
  packaging/flatpak/com.github.nuvashalauncher.nuvashalauncher.yml
flatpak run com.github.nuvashalauncher.nuvashalauncher
```
