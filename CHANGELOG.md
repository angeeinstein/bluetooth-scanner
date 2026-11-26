# Changelog

All notable changes to the BTScanner firmware. Format based on
[Keep a Changelog](https://keepachangelog.com/); versions follow semantic
versioning and match `FIRMWARE_VERSION` in [`src/version.h`](src/version.h).

When cutting a release: move the `[Unreleased]` notes into a new dated
`## [x.y.z]` section, bump `FIRMWARE_VERSION`, commit, and push — the build then
publishes a GitHub release using that section as the notes (see the README).

BTScanner started as a copy of the VibraShock recorder firmware 1.6.0. Changes
before that are in the
[VibraShock changelog](https://github.com/angeeinstein/vibrashock/blob/main/CHANGELOG.md).

## [Unreleased]

### Changed
- **New identity: BTScanner.** The board now shows up as Wi-Fi
  `BTScanner-XXXXXX` and `btscanner-xxxxxx.local` (the shared name is
  `btscanner.local`), the device ID is `BTS-XXXXXX`, and the firmware calls
  itself "Bluetooth Scanner", starting at version 0.1.0. Build images are
  named `btscanner-v<version>.bin`.
- **New OTA update token.** VibraShock and BTScanner images are now rejected by
  each other's OTA update, so neither firmware can be installed over the other
  by mistake. A USB flash still works either way.
- `managed_components/` is no longer committed; the build downloads it, pinned
  by `dependencies.lock`. The committed copy broke the build on Windows clones
  because git rewrote its line endings.
