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
source=('aml-9b7fc49.tar.gz::https://gitlab.com/anotherminecraftlauncher/aml/-/archive/9b7fc49/aml-9b7fc49.tar.gz')
sha256sums=('aadbc4c3f21c3139b22cec94ca19b08630c04b254d109a0e4a28b29da27100b1')

build() {
  cd "$srcdir/aml-9b7fc49"
  make all
}

package() {
  cd "$srcdir/aml-9b7fc49"
  make DESTDIR="$pkgdir" PREFIX=/usr install
}
