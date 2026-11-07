# Gentoo packaging

The ebuild in `packaging/gentoo/nuvashalauncher-9999.ebuild` supports both live (`9999`) builds and versioned release builds.

## Local overlay installation

Copy the ebuild into your local repository overlay (for example `/var/db/repos/local`) and emerge it:

```sh
sudo mkdir -p /var/db/repos/local/games-util/nuvashalauncher /var/db/repos/local/metadata /var/db/repos/local/profiles
echo "masters = gentoo" | sudo tee /var/db/repos/local/metadata/layout.conf >/dev/null
echo "local" | sudo tee /var/db/repos/local/profiles/repo_name >/dev/null
sudo curl -fL "https://raw.githubusercontent.com/NuvashaLauncher/NuvashaLauncher/main/packaging/gentoo/nuvashalauncher-9999.ebuild" \
  -o /var/db/repos/local/games-util/nuvashalauncher/nuvashalauncher-9999.ebuild
sudo ebuild /var/db/repos/local/games-util/nuvashalauncher/nuvashalauncher-9999.ebuild manifest
sudo emerge --accept-keywords='**' games-util/nuvashalauncher
```
