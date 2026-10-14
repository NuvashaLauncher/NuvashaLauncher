# Maintainer: Flia <43163973-anotherminecraftlauncher@users.noreply.gitlab.com>
export PACKAGER='Flia <43163973-anotherminecraftlauncher@users.noreply.gitlab.com>'
pkgname=aml
pkgver=0.1
pkgrel=1
pkgdesc='Another Minecraft Launcher for Linux'
arch=('x86_64')
options=('!debug')
url='https://gitlab.com/anotherminecraftlauncher/aml'
license=('GPL-3.0-only')
depends=('gtk4>=4.10' 'libadwaita>=1.5' 'json-glib' 'libsoup3' 'libarchive' 'wayland')
makedepends=('gcc' 'make' 'gettext' 'pkgconf')
optdepends=('java-runtime: launch Minecraft instances')
source=('aml-main.tar.gz::https://gitlab.com/anotherminecraftlauncher/aml/-/archive/main/aml-main.tar.gz')
sha256sums=('SKIP')

build() {
  cd "$srcdir/aml-main"
  make all
}

package() {
  cd "$srcdir/aml-main"
  make DESTDIR="$pkgdir" PREFIX=/usr install
}
