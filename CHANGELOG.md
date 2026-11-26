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

### Added
- **BLE scanner.** Scans continuously for Bluetooth Low Energy devices
  (legacy and BLE 5 extended advertising) with no duplicate filter, so every
  advertisement and scan response is received; restarts itself if scanning
  ever stops. Active scanning by default (gets scan responses, where names
  often are); passive and Coded-PHY (long range) scanning can be switched in
  **Settings → Bluetooth**.
- **Nearby devices list** on the Dashboard: name (cached per address),
  address and its kind (public / static / rotating), RSSI now / average,
  packets per second, total packets, last seen, maker and flags. Filter by
  text, limit to the last 10 s / 1 min / 5 min, sort by any column.
- Bluetooth state, reports per second and dropped reports in the Status card;
  a **Scanner status** card in Settings → Bluetooth.

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
