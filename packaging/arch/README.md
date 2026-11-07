# Arch Linux packaging

The `PKGBUILD` in `packaging/arch/PKGBUILD` builds a native Arch Linux package (`.pkg.tar.zst`).

## Local build

Install the dependencies and run `makepkg`:

```sh
sudo pacman -S --needed base-devel gettext pkgconf desktop-file-utils \
  gtk4 libadwaita libepoxy json-glib libsoup3 libarchive libsecret
```

From the repository root:

```sh
VERSION=$(tr -d '[:space:]' < VERSION)
git archive --format=tar.gz --prefix="nuvashalauncher-${VERSION}/" HEAD \
  -o packaging/arch/"nuvashalauncher-${VERSION}.tar.gz"
cd packaging/arch
_pkgver="${VERSION}" makepkg -sf
```

Install the built package with:

```sh
sudo pacman -U packaging/arch/nuvashalauncher-*.pkg.tar.zst
```
