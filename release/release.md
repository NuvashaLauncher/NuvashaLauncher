# Nuvasha Launcher 0.9

## Highlights

- Localized user-facing error handling across accounts, instances, Java,
  loaders, launching, content browsing, modpacks, skins, cache, and export
  flows.
- Replaced raw GUI filesystem, network, HTTP, service, and backend errors with
  translated summaries.
- Revealed previously hidden offline-account and bulk-operation failures.
- Improved Microsoft authentication state handling without invalidating saved
  credentials during temporary outages.
- Prevented invalid or oversized downloads from being retried unnecessarily.
- Added regression coverage for localized errors, authentication, downloads,
  skins, and launch failures.

## Build verification

- Tests: **127 passed, 0 skipped**
- Version: `0.9`
- Tag: `v0.9`
- Commit: `ae7ad339eb495af4c4710274b0124fed048a3fe6`

## Source archive

- File: `nuvasha-launcher-0.9.tar.gz`
- SHA-256: `6dbcd75013feb0006bfdf36ed129e7051abca3e73b7ef3a7933081c27c653e8e`

Verify it with:

```sh
sha256sum -c nuvasha-launcher-0.9.tar.gz.sha256
```
