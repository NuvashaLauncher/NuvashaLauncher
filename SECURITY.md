# Security Policy

## Supported Versions

Security fixes are released for the latest stable version and the `main` branch. Older releases are not backported.

| Version | Supported |
| ------- | --------- |
| `1.8.x` (latest) | Yes |
| `<= 1.7` | No |

## Reporting a Vulnerability

**Do not report security vulnerabilities in public GitHub Issues, Pull Requests, or Discussions.**

Report them privately through GitHub's Private Vulnerability Reporting:

1. Open [Report a vulnerability](https://github.com/NuvashaLauncher/NuvashaLauncher/security/advisories/new).
2. Include enough detail to reproduce the issue:
   - Affected version or commit hash and package format (Flatpak, DEB, RPM, Arch, or source build).
   - Steps to reproduce or a minimal proof of concept.
   - Expected impact and affected components.
   - Relevant logs or crash output (with personal tokens or credentials removed).

Confirmed issues are fixed privately and released before public disclosure. Credit is given in the advisory unless you prefer to stay anonymous.

## Scope

In scope:

- **Authentication & credentials:** Microsoft / Xbox Live / Minecraft OAuth token handling, `libsecret` storage (via the XDG Secret Portal in Flatpak or desktop Secret Service natively), and token redaction in logs and launch output.
- **Archives & downloads:** Path traversal, symlink/hardlink handling, decompression size/entry limits, Modrinth/modpack host allowlists, HTTPS/TLS enforcement, and SHA-1/SHA-512 checksum verification.
- **Process execution & sandboxing:** Launch argument construction, Java runtime discovery/execution, and Flatpak sandbox boundaries.

Out of scope:

- Vulnerabilities in Minecraft itself, Java runtimes, or third-party mod loaders (Fabric, Quilt, Forge, NeoForge).
- Outages or bugs in upstream services (Mojang, Microsoft/Xbox Live, Modrinth).
- Malicious third-party mods, resource packs, shader packs, or modpack contents, unless Nuvasha Launcher fails to enforce its own archive extraction, path validation, or download restrictions.
