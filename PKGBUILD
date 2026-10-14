# Maintainer: Flia <43163973-anotherminecraftlauncher@users.noreply.gitlab.com>
pkgname=aml
pkgver=$(<"$startdir/VERSION")
pkgrel=1
pkgdesc='Another Minecraft Launcher for Linux'
arch=('x86_64')
options=('!debug')
url='https://gitlab.com/anotherminecraftlauncher/aml'
license=('GPL-3.0-only')
depends=('gtk4' 'libadwaita' 'json-glib' 'libsoup3' 'libarchive' 'wayland')
makedepends=('gcc' 'make' 'gettext' 'pkgconf')
optdepends=('java-runtime: launch Minecraft instances')
source=()

build() {
  cd "$startdir"
  make all
}

package() {
  cd "$startdir"
  make DESTDIR="$pkgdir" PREFIX=/usr install
}
