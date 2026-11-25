# RPM packaging

Builds an RPM package for Fedora using `packaging/rpm/nuvashalauncher.spec`.

## Local build

Install the build dependencies:

```sh
sudo dnf install rpm-build rpmdevtools \
  gcc make gettext pkgconf-pkg-config \
  glib2-devel gtk4-devel libadwaita-devel libepoxy-devel \
  json-glib-devel libsoup3-devel libarchive-devel libsecret-devel \
  desktop-file-utils libappstream-glib
```

From the repository root, create the source tarball and build the RPM:

```sh
VERSION=$(tr -d '[:space:]' < VERSION)
rpmdev-setuptree
git archive --format=tar.gz --prefix="nuvashalauncher-${VERSION}/" HEAD \
  -o ~/rpmbuild/SOURCES/"nuvashalauncher-${VERSION}.tar.gz"
rpmbuild -ba --define "_version ${VERSION}" packaging/rpm/nuvashalauncher.spec
```
