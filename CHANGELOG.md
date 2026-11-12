# Changelog

All notable changes to Way Vibrance are documented here. Format based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versioning follows
[SemVer](https://semver.org/).

## [0.6.6] - 2026-11-12

### Added
- `KConfigWatcher` on `kwinrc` so external edits are picked up without a
  KWin restart.
- D-Bus setter rate limiting (token bucket) in the effect.
- Backup-and-restore around `kwinrc` persistence.
- `docs/adr/0001-tray-compositor-owner.md` documenting the compositor-owner
  trust invariant.
- `config_roundtrip_test` and `dbus_security_test` plus a CI job that builds
  the effect module with KWin dev packages.

### Changed
- Repaints are coalesced through a 16 ms timer instead of one full repaint
  per D-Bus/config call.
- `effects->screens()` is cached in `windowInScope()` and invalidated on
  `screenAdded`/`screenRemoved`.
- Shader cache keys use 9 significant digits instead of a 0.001 quantization.
- Per-profile gain serialization now keeps 7 significant digits.
- `resolveWindow()` defers reading `caption()` to its fast paths.
- Matching budget loop breaks early once exhausted.
- Exponential backoff for shader compile retries.
- Tray removes a duplicate `setStyleSheet` on the profile group.
- `friendlyAppName()` falls back to the prefixed pattern when the derived
  name is empty.

### Fixed
- `kwinrc` `sync()` result is now checked; backup is restored on failure.
- `setPrimaryOutputName` rejects control characters.
- Version metadata and docs aligned to 0.6.6.

### Security
- `kwinrc` persistence is guarded against truncation/corruption.
- D-Bus setters are rate-limited against local DoS.
- Primary output name validated on the setter path.
