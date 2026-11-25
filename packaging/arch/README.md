# Arch Linux packaging

Builds a native Arch Linux package (`.pkg.tar.zst`) using `packaging/arch/PKGBUILD`.

## Local build

Install the build and runtime dependencies:

```sh
sudo pacman -S --needed base-devel gettext pkgconf desktop-file-utils \
  gtk4 libadwaita libepoxy json-glib libsoup3 libarchive libsecret \
  hicolor-icon-theme shared-mime-info
```

From the repository root, create the source archive and run `makepkg`:

```sh
VERSION=$(tr -d '[:space:]' < VERSION)
git archive --format=tar.gz --prefix="nuvashalauncher-${VERSION}/" HEAD \
  -o packaging/arch/"nuvashalauncher-${VERSION}.tar.gz"
cd packaging/arch
_pkgver="${VERSION}" makepkg -sf
```

Install the built package:

```sh
sudo pacman -U packaging/arch/nuvashalauncher-*.pkg.tar.zst
```
