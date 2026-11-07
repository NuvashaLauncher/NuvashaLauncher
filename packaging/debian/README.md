# Debian / Ubuntu (.deb) packaging

The files in `packaging/debian/` build a native `.deb` package for Debian and Ubuntu.

## Local build

Install the Debian packaging tools and development dependencies:

```sh
sudo apt install build-essential debhelper devscripts desktop-file-utils \
  pkgconf gettext libglib2.0-dev libgtk-4-dev libadwaita-1-dev \
  libepoxy-dev libjson-glib-dev libsoup-3.0-dev libarchive-dev libsecret-1-dev
```

From the repository root, copy the packaging metadata into `debian/` and run `dpkg-buildpackage`:

```sh
cp -r packaging/debian ./debian
chmod +x debian/rules
dpkg-buildpackage -us -uc -b
rm -rf ./debian
```

The resulting `.deb` package is written to the parent directory (`../nuvashalauncher_<version>-1_<arch>.deb`) and can be installed with:

```sh
sudo apt install ../nuvashalauncher_*.deb
```
