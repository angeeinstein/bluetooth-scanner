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

### Fixed
- **Boot loop on real hardware** ("task create failed", then the web server
  couldn't start and the board rebooted). Wi-Fi + Bluetooth left too little
  internal RAM for task stacks. Wi-Fi/network buffers and the BLE, scan-log
  and motion task stacks now use PSRAM; the boot log reports free internal
  RAM at each stage.

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
- **Scan log on the SD card.** *Start logging* (or POWER double-click) writes
  every received packet as one CSV row to `/ble` on the card: time since boot
  and UTC, address and kind, RSSI, packet type, PHY, the raw payload in hex,
  and the decoded name, manufacturer data, TX power, flags, appearance,
  service UUIDs and service data. Device names are cached, so packets without
  a name still carry it. TARGET, MARK, once-per-second STATUS and INFO rows
  (clock set, Wi-Fi off/on, rule changes) go into the same file. Large logs
  rotate at 1 GB; a `.log` and `.mark` file sit next to each session, and the
  card carries a `FORMAT.md` describing all columns.
- **Target devices.** Rules for name prefixes, exact addresses and address
  prefixes (Settings → Bluetooth; default `name:XXZ`). A match is flagged in
  every row, logged as a TARGET event, flashed magenta on the LED and listed
  on the Dashboard.
- **100 % scan mode.** Optionally switches Wi-Fi off while logging, so the
  scanner gets the radio to itself; Wi-Fi returns when logging stops.
- **Start logging at power-on** (off by default) and **copy every row to USB
  serial** (rows prefixed `BTS,`).
- **Analyze tab** for one device: decoded fields, last advertisement and scan
  response in hex, and its RSSI and packet rate over the last 2 minutes
  (kept on the board). **Air activity** chart (reports/s and devices per
  second, last 10 minutes) on the Dashboard and in Analyze.
- **Motion profile of the board** from the accelerometer: still / moving /
  walking, activity, steps and cadence, estimated walking speed and distance,
  the main horizontal movement axis (relative to the board) and tilt. A
  Motion card on the Dashboard, a 10-minute motion chart in Analyze, a MOTION
  row in the log every second, and optionally every ~100 Hz sample in
  `<base>.motion.csv`. Settings → Motion sensor: range (default ±4 g) and the
  100 Hz file; the sensor's filter stays off so gravity is kept.
- **Recordings tab** lists the logging sessions with duration, rows, size and
  how they ended; download each part, rename or delete.

### Removed
- Accelerometer recording (`.bin` files), the on-device and in-browser CSV
  conversion, auto-range, the live acceleration graph and statistics, and the
  Analyze tab's time-domain / FFT / spectrogram views. The UDP and WebSocket
  accelerometer streams stay in the firmware (no UI) for later reuse.

### Changed
- **New identity: BTScanner.** The board now shows up as Wi-Fi
  `BTScanner-XXXXXX` and `btscanner-xxxxxx.local` (the shared name is
  `btscanner.local`), the device ID is `BTS-XXXXXX`, and the firmware calls
  itself "Bluetooth Scanner", starting at version 0.1.0. Build images are
  named `btscanner-v<version>.bin`.
- **New OTA update token.** VibraShock and BTScanner images are now rejected by
  each other's OTA update, so neither firmware can be installed over the other
  by mistake. A USB flash still works either way.
- **Update-server settings are separate from VibraShock's.** The server
  address, its secret and the IP-reporting switch are stored under new names,
  so a board converted from VibraShock starts with no server configured and
  doesn't report to VibraShock's server. VibraShock's saved values stay
  untouched for when the board is flashed back. Saved Wi-Fi networks, battery
  learning and the other settings carry over in both directions.
- `managed_components/` is no longer committed; the build downloads it, pinned
  by `dependencies.lock`. The committed copy broke the build on Windows clones
  because git rewrote its line endings.
