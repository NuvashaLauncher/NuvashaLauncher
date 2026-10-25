# Flatpak packaging

The Flatpak build uses the GNOME runtime and disables Nuvasha's optional
bubblewrap integration. Flatpak itself is the sandbox for this build.

## Local build

Install `flatpak`, `flatpak-builder`, and the GNOME 51 runtime and SDK, then
run from the repository root:

```sh
flatpak-builder --user --install --force-clean \
  build/flatpak \
  packaging/flatpak/com.github.nuvashalauncher.Nuvasha.yml
flatpak run com.github.nuvashalauncher.nuvasha
```

The manifest follows the `flatpak` branch so the packaging changes and the
Flatpak-specific build flag are included. Native builds continue to use the
normal Makefile path and retain their sandbox settings.
