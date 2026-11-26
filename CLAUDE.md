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

**Rework in progress**, in this order: (1) rename/identity — done; (2) BLE
scanner (NimBLE) + live device list; (3) SD scan log — per-device summary by
default, switchable full capture of every advertisement; (4) accelerometer
reduced to a ~100 Hz motion log + still/moving, steps, estimated speed;
(5) remove vibration analysis UI. Keep `udp_stream`/`ws_stream` (detached
from full-rate accel) for a later middle-ground stream. Code not yet reworked
still does vibration-recorder things, and docs say so where they're inherited.

- The ESP32-S3 is **BLE only** — no Classic Bluetooth. Wi-Fi and BLE share
  one radio (coexistence), so heavy Wi-Fi traffic cuts scan time.

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
   `config.max_uri_handlers = 60` (~49 registered). Adding endpoints past the cap
   silently fails to register (boot logs "no slots left for registering handler",
   and the endpoint 404s) — raise it in step if you add handlers.

5. **Keep `nvs` at its partition offset.** Any change to
   [partitions_ota.csv](partitions_ota.csv) must keep `nvs` at the same offset,
   or devices lose all saved settings (Wi-Fi, toggles) on update.

6. **Changed anything a recording writes? Update the on-card format doc.** The
   firmware writes `s_format_doc` (in [src/recorder.c](src/recorder.c)) to
   `/rec/FORMAT.md` on every SD card, and scripts/AI agents reading a card rely on
   it. Any change to the `.bin` header or layout ([src/recording_format.h](src/recording_format.h)),
   the `.log` lines, the `.aux.csv` columns, the `.mark` format, the CSV export, or
   file naming/rotation must update that text in the same change — plus the
   README's "Recorded file format" section. Nothing checks this; a stale doc
   silently misleads every reader of the card.

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

`main.c` (boot/init order + OTA validate) · `accel_capture.*` (IIS3DWB FIFO+DMA,
ring buffers) · `ring_buffer.*` · `recorder.*` (recording, markers, SD space) ·
`csv_export.*` (on-device CSV) · `ota.*` + `ota_token.h` (OTA install + token
gate) · `ota_cloud.*` (pull-from-URL update server + authed POST helper) ·
`device_report.*` (periodic local-IP reporting to the update server) ·
`wifi_manager.*` · `power_ctrl.*` (charger, fuel gauge, low-battery,
status LED) · `led_rgb.*` (WS2812 RMT driver) + `led_indicator.*` (RGB status
vocabulary — used only when `STATUS_LED_WS2812` in pins.h is 1; plain boards
keep power_ctrl's on/off patterns) ·
`device_id.*` · `sd_benchmark.*` · `udp_stream.* / ws_stream.*` (live streaming) ·
`web/dashboard.*` (HTTP API + page serving) · `*.html` + generated `*_html.*`.
