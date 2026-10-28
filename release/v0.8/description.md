# Nuvasha Launcher 0.8

Nuvasha Launcher 0.8 replaces the account JSON/keyring split with a complete
Secret Service-backed account store and strengthens headless and sanitizer test
coverage.

## Account storage

- Store every account, including offline accounts, in the desktop Secret
  Service.
- Keep complete account records together: identity, access and refresh tokens,
  expiry, avatar, skin model, and cape.
- Store the active account ID in a separate Secret Service item.
- Persist account updates without overwriting other accounts, including skin
  and cape changes.
- Use new account and active-account schemas without migrating legacy JSON or
  keyring entries.

## Packaging and documentation

- Add libsecret to native build dependencies and CI installation.
- Grant Flatpak access to `org.freedesktop.secrets`.
- Document the Secret Service account architecture and runtime requirement.
- Update the CLI branding and Flatpak version smoke test for the spaced
  “Nuvasha Launcher” name.

## Testing and reliability

- Add persistence coverage for complete account records across restart.
- Cover account replacement, active-account transitions, authentication
  persistence, and cosmetic updates.
- Make account integration tests skip cleanly when Secret Service is absent.
- Fix sanitizer cleanup for skipped authentication tests and asynchronous
  launcher cleanup.
- Keep strict CI builds working with `-Werror`.

## Verification

- 121 tests pass in the normal test suite.
- Headless sanitizer runs pass with account/display-dependent tests skipped
  where their external services are unavailable.
- Static analysis passes without project warnings.
