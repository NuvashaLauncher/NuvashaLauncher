# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit optfeature toolchain-funcs xdg

DESCRIPTION="Minecraft launcher for Linux"
HOMEPAGE="https://github.com/NuvashaLauncher/NuvashaLauncher"

if [[ ${PV} == 9999 ]]; then
	inherit git-r3
	EGIT_REPO_URI="https://github.com/NuvashaLauncher/NuvashaLauncher.git"
else
	SRC_URI="https://github.com/NuvashaLauncher/NuvashaLauncher/archive/v${PV}.tar.gz -> ${P}.tar.gz"
	S="${WORKDIR}/NuvashaLauncher-${PV}"
	KEYWORDS="~amd64 ~arm64"
fi

LICENSE="GPL-3 CC0-1.0"
SLOT="0"

DEPEND="
	app-arch/libarchive:=
	app-crypt/libsecret
	dev-libs/glib:2
	dev-libs/json-glib
	>=gui-libs/gtk-4.10:4
	>=gui-libs/libadwaita-1.5:1
	gui-libs/libepoxy
	net-libs/libsoup:3.0
"
RDEPEND="
	${DEPEND}
	x11-misc/shared-mime-info
	x11-themes/hicolor-icon-theme
"
BDEPEND="
	sys-devel/gettext
	virtual/pkgconfig
"

src_compile() {
	local pkg_libs
	pkg_libs="$($(tc-getPKG_CONFIG) --libs gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libarchive gio-unix-2.0 epoxy libsecret-1) -lm"
	emake \
		CC="$(tc-getCC)" \
		PKG_CONFIG="$(tc-getPKG_CONFIG)" \
		PREFIX="${EPREFIX}/usr" \
		RELFLAGS="${CFLAGS}" \
		PKG_LIBS="${LDFLAGS} ${pkg_libs}"
}

src_test() {
	local pkg_libs
	pkg_libs="$($(tc-getPKG_CONFIG) --libs gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libarchive gio-unix-2.0 epoxy libsecret-1) -lm"
	emake test \
		CC="$(tc-getCC)" \
		PKG_CONFIG="$(tc-getPKG_CONFIG)" \
		PREFIX="${EPREFIX}/usr" \
		RELFLAGS="${CFLAGS}" \
		PKG_LIBS="${LDFLAGS} ${pkg_libs}"
}

src_install() {
	emake DESTDIR="${D}" PREFIX="${EPREFIX}/usr" install
	rm -f "${ED}/usr/share/glib-2.0/schemas/gschemas.compiled" || die
	rm -f "${ED}/usr/share/applications/mimeinfo.cache" || die
	find "${ED}/usr/share/mime" -mindepth 1 -maxdepth 1 ! -name packages -exec rm -rf {} + || die
	einstalldocs
}

pkg_postinst() {
	xdg_pkg_postinst
	optfeature "Java 8 runtime for Minecraft 1.16.5 and older" virtual/jre:1.8 dev-java/openjdk-bin:8 dev-java/openjdk:8
	optfeature "Java 17 runtime for Minecraft 1.17 to 1.20.4" virtual/jre:17 dev-java/openjdk-bin:17 dev-java/openjdk:17
	optfeature "Java 21 runtime for Minecraft 1.20.5 to 1.21.x" virtual/jre:21 dev-java/openjdk-bin:21 dev-java/openjdk:21
	optfeature "Java 25 runtime for Minecraft 26.1 and newer" virtual/jre:25 dev-java/openjdk-bin:25 dev-java/openjdk:25
}
