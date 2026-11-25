# Debian / Ubuntu (.deb) packaging

Builds a native `.deb` package using the metadata in `packaging/debian/`.

## Local build

Install the build dependencies:

```sh
sudo apt install build-essential debhelper devscripts desktop-file-utils \
  pkgconf gettext libglib2.0-dev libgtk-4-dev libadwaita-1-dev \
  libepoxy-dev libjson-glib-dev libsoup-3.0-dev libarchive-dev libsecret-1-dev
```

From the repository root:

```sh
cp -r packaging/debian ./debian
chmod +x debian/rules
dpkg-buildpackage -us -uc -b
rm -rf ./debian
```

Install the resulting package from the parent directory:

```sh
sudo apt install ../nuvashalauncher_*.deb
```
