# Security Policy

## Supported Versions

Security updates are provided for the latest stable release and the `main` branch. Older versions do not receive backported fixes; please upgrade to the latest release before reporting an issue.

| Version | Supported |
| ------- | --------- |
| `1.7.x` (latest) | Yes |
| `<= 1.6` | No |

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub Issues, Pull Requests, or Discussions.**

If you discover a security vulnerability in Nuvasha Launcher, report it privately using GitHub's Private Vulnerability Reporting:

1. Go to the repository's **Security** tab (`https://github.com/NuvashaLauncher/NuvashaLauncher/security/advisories/new`).
2. Click **Report a vulnerability**.
3. Provide as much detail as possible so the issue can be reproduced and resolved quickly:
   - Affected version, commit hash, and package format (Flatpak, DEB, RPM, Arch, or source build).
   - Clear steps to reproduce or a minimal proof of concept.
   - Potential impact and affected components.
   - Relevant logs or crash output (with any personal tokens or credentials removed).

### Disclosure Process

- Reports will be reviewed and acknowledged as soon as possible.
- Once confirmed, a fix will be developed privately and released in a new version before public disclosure.
- Reporters may be credited in the security advisory and release notes unless anonymity is requested.

## Scope

Areas of particular security interest in Nuvasha Launcher include:

- **Authentication & Credentials:** Handling of Microsoft / Xbox Live / Minecraft OAuth tokens, desktop Secret Service (`libsecret`) storage, and token redaction in logs and launch arguments.
- **Modpacks, Archives & Downloads:** Path traversal, symlink/hardlink handling, decompression size/entry limits, host allowlists, HTTPS/TLS enforcement, and SHA-1/SHA-512 checksum verification when importing or installing `.mrpack` modpacks, mods, worlds, or loader installers.
- **Process Execution & Sandboxing:** Command-line construction, Java runtime discovery/execution, and Flatpak sandbox boundaries.

### Out of Scope

- Vulnerabilities inside Minecraft itself, Java runtimes, or third-party mod loaders (Fabric, Quilt, Forge, NeoForge).
- Bugs or outages on upstream services (Mojang, Microsoft/Xbox Live, Modrinth).
- Malicious behavior inside third-party mods, shader packs, resource packs, or modpack contents themselves, unless Nuvasha Launcher fails to enforce its own archive extraction, path validation, or download restrictions.
