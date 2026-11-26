# Working on this repo (agent guide)

Conventions and non-obvious rules for any AI agent (or human) working on the
BTScanner firmware. Claude Code loads this file automatically; point other
agents at it. Keep it short and current — when a rule here changes, update it.

**Deeper docs:** [README.md](README.md) (usage, OTA), [ARCHITECTURE.md](ARCHITECTURE.md)
(how each subsystem works), [RELEASING.md](RELEASING.md) (cutting a release),
[SELFTEST_SPEC.md](SELFTEST_SPEC.md) (original hardware spec).

## What this is

**BTScanner**: a Bluetooth Low Energy scanner/logger on the ESP32-S3 + ST IIS3DWB
board, forked from the VibraShock vibration recorder (same hardware, imported
without history; no link to that repo). PlatformIO + **ESP-IDF 5.2.1**, single
env `esp32-s3-selftest`. Windows host, PowerShell primary. C firmware in `src/`;
the web dashboard is HTML embedded into the binary.

**Rework status:** done — rename/identity, BLE scanner (NimBLE) + live device
list, SD scan log (every report as a CSV row), target rules, 100 % scan mode,
web UI rebuilt around BLE (vibration graphs/FFT/spectrogram/CSV conversion
removed). **Next:** accelerometer reduced to a ~100 Hz motion log +
still/moving, steps, estimated speed, relative direction. Keep
`udp_stream`/`ws_stream` (still raw accel, no UI) for a later middle-ground
stream. Docs mark what's still inherited from VibraShock.

- The ESP32-S3 is **BLE only** — no Classic Bluetooth. Wi-Fi and BLE share
  one radio (coexistence), so heavy Wi-Fi traffic cuts scan time.

**Scanner requirements (agreed with the user; a minimum — record more where cheap):**
- Continuous active scan, window = interval, no duplicate filter, no time
  limit, auto-restart if the stack stops it. Legacy + extended advertising
  on 1M; Coded PHY as a switch.
- One CSV per logging session, one row per received report (adv *and* scan
  response) plus a `kind` column for MARK / TARGET / INFO rows. Per report:
  ms since boot + UTC when known, address + type, RSSI, event type, PHY,
  flags, name (cached per address), raw payload hex, parsed manufacturer
  data, TX power, service UUIDs/data, appearance. Log a row when the clock
  gets set. A once-per-second status row (packets/s, dropped reports, Wi-Fi
  on/off, battery, temperatures). Optional mirror of rows to USB serial.
- Target matching: configurable name-prefix / address / address-prefix rules
  in NVS, default name prefix "XXZ"; a match logs a TARGET row, flashes the
  LED and highlights the device on the dashboard.
- Wi-Fi stays on by default. A "100 % scan mode" setting turns Wi-Fi off
  while logging; explain the radio-sharing trade-off next to that toggle and
  in the README.
- Markers: POWER single click only (as in VibraShock). Auto-start logging at
  power-on: setting, default off.

## Build & flash

`pio` is **not on PATH**. Invoke it by full path:

```powershell
# PowerShell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
```
```bash
# Bash tool
"$USERPROFILE/.platformio/penv/Scripts/pio.exe" run
```

- Build: `pio run` · USB flash: `pio run -t upload` · Serial monitor: `pio run -t monitor`
- **Always finish a code change by building** to confirm it compiles before reporting done.

## Hard rules — these silently break things if ignored

1. **Edited a `.html` file? Regenerate the embedded C first.** `src/dashboard.html`
   and `src/live.html` are embedded via generated `*_html.c/.h`. After any HTML
   edit, run this **before building**, or your change won't be in the binary:
   ```
   python tools/gen_html_c.py src
   ```
   (Adding a new embedded page: add a tuple to `PAGES` in
   [tools/gen_html_c.py](tools/gen_html_c.py).) The whole UI is now a
   **single-page app** in `src/dashboard.html` (Dashboard / Analyze / Recordings
   / Settings tabs); `src/live.html` is just a redirect stub to `/` — edit
   `dashboard.html`.

2. **Do not unpin the platform.** [platformio.ini](platformio.ini) pins
   `platform = espressif32@6.7.0` (IDF 5.2.1) on purpose. Letting it auto-update
   (to 6.13.0 / IDF 5.5.3) changes the toolchain and has arrived half-installed
   and broken the build. Bump only intentionally.

3. **OTA rollback safety — never abort boot on absent hardware.** `app_main` in
   [src/main.c](src/main.c) ends with `ota_mark_valid()`; *reaching that line* is
   the "firmware is good" signal that cancels the OTA rollback. Every init above
   it must **warn-and-continue** on missing hardware (no SD card, detached
   accelerometer), never `abort`/`ESP_ERROR_CHECK` on those. Otherwise a missing
   peripheral rolls back a perfectly good update. Only `nvs_flash_init` is
   allowed to hard-fail. See the comment above `ota_mark_valid()`.

4. **Dashboard URI handler cap.** [src/web/dashboard.c](src/web/dashboard.c) sets
   `config.max_uri_handlers = 60` (~54 registered). Adding endpoints past the cap
   silently fails to register (boot logs "no slots left for registering handler",
   and the endpoint 404s) — raise it in step if you add handlers.

5. **Changed `sdkconfig.defaults`? Delete `sdkconfig.esp32-s3-selftest`.**
   That generated (git-ignored) file wins over the defaults for every key it
   already has, so edits to the defaults are silently ignored until it's
   deleted and regenerated by the next build. Also: IDF 5.2.1's NimBLE does
   **not compile** with the peripheral/broadcaster roles or the security
   manager disabled — leave those at their defaults.

6. **Keep `nvs` at its partition offset.** Any change to
   [partitions_ota.csv](partitions_ota.csv) must keep `nvs` at the same offset,
   or devices lose all saved settings (Wi-Fi, toggles) on update.

7. **NVS is shared with VibraShock — keep switching between them safe.** A
   board can be USB-flashed back and forth (a USB upload never touches the
   `nvs` partition). `wifi_cfg`, `boot_cfg`, `pwr_cfg`, `accel_cfg` and
   `recorder` are read by both firmwares: never change the meaning or format
   of an existing key there. New BTScanner settings go into new keys or a new
   namespace (`ble`, ...). The update-server settings use their own
   `bts_ota_cloud` / `bts_dev_report` namespaces on purpose, so a converted
   board never talks to VibraShock's server. Keep BTScanner's SD files out of
   VibraShock's `/rec` directory.

8. **Changed anything the scan log writes? Update the on-card format doc.** The
   firmware writes `s_format_doc` (in [src/scan_log.c](src/scan_log.c), next to
   the CSV header and row formatting) to `/ble/FORMAT.md` on every SD card, and
   scripts/AI agents reading a card rely on it. Any change to the CSV columns or
   row kinds, the `.log` lines, the `.mark` format, or file naming/rotation
   ([src/recorder.c](src/recorder.c)) must update that text in the same change —
   plus the README's "Scan log format" section. Nothing checks this.

## Documentation discipline (do this as part of the change, not later)

- **CHANGELOG** — every user-facing change gets a bullet under `## [Unreleased]`
  in [CHANGELOG.md](CHANGELOG.md) (Added / Changed / Fixed). This is how release
  notes are generated; an un-logged change ships with no notes.
- **ARCHITECTURE.md** — adding or materially changing a subsystem means updating
  the matching section (it's the design reference, kept in sync with the code).
- **README.md** — update it when user-facing behavior or setup changes.

## Versioning & releases

- `FIRMWARE_VERSION` in [src/version.h](src/version.h) is a **manual** semantic
  version. Do **not** bump it casually — a bump + commit + push + `pio run -t
  release` publishes a GitHub release (see [RELEASING.md](RELEASING.md)). Bump
  only when the user is deliberately publishing.
- Every build also copies the image to `dist/btscanner-v<version>.bin` via the
  post-build script [pkg_firmware.py](pkg_firmware.py). This is additive and
  never changes build/USB behavior; the release upload only fires on a new
  version with a clean, pushed tree and `gh` authenticated.

## Repo facts

- GitHub remote: `angeeinstein/bluetooth-scanner`.
- Firmware updates are done by **uploading the .bin in the dashboard**. The
  pull-from-update-server path (`ota_cloud.*`, `device_report.*`) is kept and
  must keep compiling/working, but no server is planned — don't build on it.
- `OTA_UPDATE_TOKEN` in [src/ota_token.h](src/ota_token.h) differs from
  VibraShock's on purpose, so neither firmware installs over the other via OTA.
  Don't copy VibraShock's token back.
- `managed_components/` is git-ignored (downloaded, pinned by
  `dependencies.lock`). Committing it breaks Windows clones: autocrlf rewrites
  line endings and the component manager rejects the "modified" files.
- `dist/` and `.pio/` are git-ignored — never commit build artifacts or `.bin`s.
  **One deliberate exception:** the single `provisioning/btscanner-v<version>-full-flash.bin`
  is committed so a fresh clone can set up new boards (see
  [provisioning/README.md](provisioning/README.md)). It's regenerated only manually via
  `python provisioning/make_image.py` (which replaces the old one), not per release.
- Match the surrounding code's style, comment density, and naming; prefer editing
  existing files over adding new ones.

## Source map (`src/`)

`main.c` (boot/init order + OTA validate) · `ble_scanner.*` (NimBLE continuous
extended scan → PSRAM report queue → processing task: AD decode, per-address
device table with 2 min history + last payloads, report hook, targets) ·
`scan_log.*` (CSV schema + FORMAT.md, ADV/TARGET/MARK/STATUS/INFO rows, target
rules, logging settings, 100 % scan mode, serial mirror) · `accel_capture.*` (IIS3DWB FIFO+DMA,
ring buffers) · `ring_buffer.*` · `recorder.*` (SD card, logging sessions, line buffer → SD writer, markers) · `ota.*` + `ota_token.h` (OTA install + token
gate) · `ota_cloud.*` (pull-from-URL update server + authed POST helper) ·
`device_report.*` (periodic local-IP reporting to the update server) ·
`wifi_manager.*` · `power_ctrl.*` (charger, fuel gauge, low-battery,
status LED) · `led_rgb.*` (WS2812 RMT driver) + `led_indicator.*` (RGB status
vocabulary — used only when `STATUS_LED_WS2812` in pins.h is 1; plain boards
keep power_ctrl's on/off patterns) ·
`device_id.*` · `sd_benchmark.*` · `udp_stream.* / ws_stream.*` (live streaming) ·
`web/dashboard.*` (HTTP API + page serving) · `*.html` + generated `*_html.*`.
