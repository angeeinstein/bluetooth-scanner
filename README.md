# BTScanner — Bluetooth scanner firmware

PlatformIO + ESP-IDF firmware that turns the ESP32-S3 board from the
[VibraShock](https://github.com/angeeinstein/vibrashock) vibration recorder into a **Bluetooth Low
Energy scanner and logger**. It scans for nearby BLE devices, logs what it sees to the SD card, and
records a motion profile of the device itself from the on-board IIS3DWB accelerometer. The web
dashboard, multi-network Wi-Fi, OTA updates, battery/charger handling and status LED carry over
from VibraShock.

> This repo started as a copy of VibraShock 1.6.0 and has been reworked into a BLE scanner: live
> device list, every packet logged as a CSV row, target rules, 100 % scan mode, a motion profile of
> the board from its accelerometer, and a web interface built around all of that. Sections marked
> as inherited still describe VibraShock behaviour.
>
> The ESP32-S3 radio is **BLE only** — Classic Bluetooth (BR/EDR) devices can't be discovered.

## Set up a new board

A fresh board needs **one** USB flash; after that it updates over the air. Everything for that
first flash is in **[provisioning/](provisioning/)** — no VS Code or PlatformIO needed:

- **In the browser (easiest):** open <https://espressif.github.io/esptool-js/> in Chrome/Edge →
  Connect → flash `provisioning/btscanner-v<version>-full-flash.bin` at address **`0x0`**.
- **GUI flasher:** double-click [provisioning/flasher.pyw](provisioning/flasher.pyw) (needs
  Python 3), pick the port marked (Espressif), click **Flash**.

Then join the board's `BTScanner-XXXXXX` Wi-Fi (password `recorder123`) and open
<http://192.168.4.1>. Step-by-step guide: [provisioning/README.md](provisioning/README.md).

See [SELFTEST_SPEC.md](SELFTEST_SPEC.md) for the board's hardware/pin specification and
[datasheets/](datasheets/) for the underlying part datasheets.

Firmware identity lives in [src/version.h](src/version.h) — name, version (bump `FIRMWARE_VERSION`
per release), author, and the compiler build stamp. It's shown in the dashboard's **About** card
(Settings tab, last item) alongside a per-board **Device ID** derived from the chip's factory MAC
(`BTS-XXXXXX`, unique with no configuration), and printed to the boot serial log. By Angelo Popovic.

## Build & flash

```
pio run                # build
pio run -t upload      # flash (USB-C port, native USB — auto-reset works out of the box)
pio device monitor      # or: pio run -t monitor
```

First build downloads the full ESP-IDF toolchain/framework (~1-2 GB) and takes several minutes; it
also fetches the `mdns` managed component (declared in `src/idf_component.yml`, since ESP-IDF 5.x
no longer bundles it) from the component registry the first time — needs network access once, then
it's cached under `.pio/build/esp32-s3-selftest/managed_components/`.

### Flashing a new board without PlatformIO

See [Set up a new board](#set-up-a-new-board) and [provisioning/](provisioning/). The image there
is made by `python provisioning/make_image.py` after a `pio run` — a manual side tool; the build
and `pio run -t upload` don't use it. Refresh it only when you want a newer starting version.

### Firmware updates (OTA)

The board uses a **dual-OTA partition layout** (`partitions_ota.csv`: two 3 MB app slots +
`otadata`), so firmware can be updated **from the dashboard** — Settings → *Firmware update* → pick a
built `firmware.bin`; the device verifies it (SHA-256 + image structure), switches slots, and
reboots, with **automatic rollback** if the new image fails to boot. See
[ARCHITECTURE.md §14](ARCHITECTURE.md) for the full mechanism, verification, and security notes.

> **One-time step:** adopting the OTA layout from the old single-app table needs **one USB flash**
> (`pio run -t upload`) — you can't OTA *into* a new partition layout. After that, updates are
> wireless. `nvs` is kept at the same offset, so saved settings survive the reflash.

**Getting the update image.** The OTA image is just the built app binary. `pio run` produces it at
`.pio/build/esp32-s3-selftest/firmware.bin`, and a post-build step (`pkg_firmware.py`) also drops a
version-named copy at **`dist/btscanner-v<version>.bin`** (version from `FIRMWARE_VERSION` in
`src/version.h`) — that's the file to upload. This is purely additive; normal `pio run` /
`pio run -t upload` (USB) behavior is unchanged, and `dist/` is git-ignored.

**Update authentication.** Every build embeds a secret **build token** (`src/ota_token.h`); the
device only installs an uploaded image that contains it, so a random/unrelated firmware is rejected
with no manual password needed. This is casual/accidental protection (the token is plaintext in the
`.bin`), not hardened security — see [ARCHITECTURE.md §14](ARCHITECTURE.md). Rotating the token needs
a USB flash (a device only trusts the token in its currently-running firmware).

**Publishing a release.** Firmware is always built **locally** in PlatformIO (GitHub never builds it).
Day-to-day you rebuild and OTA-flash freely at the same `FIRMWARE_VERSION` — nothing is published. To
cut a release you bump the version and run the `release` task, which publishes it to **GitHub
Releases** with `dist/btscanner-v<version>.bin` attached and the matching `CHANGELOG.md` section as
the notes. Concretely:

1. Add your changes under `## [Unreleased]` in [`CHANGELOG.md`](CHANGELOG.md) as you work.
2. When ready, rename that section to `## [x.y.z] - <date>` and bump `FIRMWARE_VERSION` in
   [`src/version.h`](src/version.h) to the same `x.y.z`.
3. **Commit and push** (this is required — the release tag points at the pushed commit, and the
   attached `.bin` must match committed source).
4. Run **`pio run -t release`** (or the *Publish release* task in the PlatformIO sidebar). It runs
   `gh release create vx.y.z …`, or tells you exactly why it didn't.

The release only fires when **all** of these hold, so ordinary iteration never publishes by accident:
the version has no release yet, the git tree is clean, the commit is pushed, and `gh` is installed +
authenticated. (A release *also* fires automatically after any build that recompiles the firmware, but
a no-op build won't trigger it — so `run -t release` is the reliable path.) Published versions are
treated as **immutable** — for a fix, bump to the next patch version rather than swapping an asset.

> **One-time setup (do this when you want auto-releases turned on).** Install the GitHub CLI
> (<https://cli.github.com/> or `winget install GitHub.cli`) and run `gh auth login` once, choosing
> this repository's account. Until you do, the release step is simply skipped and builds are
> unaffected — everything else (the `dist/` copy, USB flash, dashboard OTA) works with no GitHub
> account at all. No token is stored in the repo; `gh` holds the credential in your OS keychain.
> **Full step-by-step manual (setup, publishing, troubleshooting): [RELEASING.md](RELEASING.md).**

(Note: a release `.bin` contains the build token in plaintext, so keep the repo/releases private, or
accept that the token is public — it's obscurity-level protection anyway. Release assets live outside
git history, so the source tree stays clean, and this is exactly the mechanism a future pull-from-URL
auto-update would fetch from.)

## Bluetooth scanning

The scanner starts at boot and never stops: it listens for Bluetooth Low Energy advertisements
(legacy and BLE 5 extended) on all three advertising channels with no duplicate filter, so every
packet is received and repeated packets from one device form a continuous RSSI series. If scanning
ever stops, it is restarted within a second. The ESP32-S3 has no Classic Bluetooth — BR/EDR-only
devices can't be seen.

**Address kinds:** `public` and `static` addresses stay the same; `rpa` (resolvable private) and
`nrpa` addresses rotate every few minutes, so phones and most wearables show up as a new entry
after each rotation.

**Wi-Fi and Bluetooth share one radio.** While Wi-Fi is on — even just the access point — the
scanner gets only part of the air time and misses some packets. For measurements where that
matters, turn on **100 % scan mode** (Settings → Bluetooth → Logging): Wi-Fi switches off 5 s after
logging starts and comes back when logging stops (POWER double-click). The dashboard is unreachable
in between; the log records `wifi_off` / `wifi_on` INFO rows.

## Using it

1. Flash and boot. Connect a phone/laptop to the board's Wi-Fi **`BTScanner-XXXXXX`** / password
   **`recorder123`** (always on unless 100 % scan mode has switched it off), or join it to your own
   network (Settings → Wi-Fi).
2. Open **http://192.168.4.1/** (or **http://btscanner.local/**).
3. **Dashboard** — *Nearby Bluetooth devices*: name (cached per address — many devices send it only
   in the scan response), address and kind, RSSI now / average, packets per second, total, last
   seen, maker (from the manufacturer data) and flags (`conn` connectable, `ext` BLE 5 extended,
   `coded` long range). Filter by text, limit to the last 10 s / 1 min / 5 min, sort by any column,
   click a row to open it in Analyze. Below: logging controls, the *Air activity* chart (reports/s
   and devices heard per second, last 10 min), the Status card and the *Target devices* card.
4. **Logging** — give the session a name (optional) and **Start logging**, or double-click POWER.
   A single POWER click (or *Add marker*) places a marker. Every received packet becomes one CSV
   row on the SD card (format below); the readout shows elapsed time, rows, size, write rate and
   how long the card lasts at that rate. **Settings → Bluetooth → Logging** has *Start logging at
   power-on* (field use, off by default), *100 % scan mode*, the POWER-click marker switch and *Copy
   every log row to USB serial* (rows prefixed `BTS,`, with or without an SD session).
5. **Target devices** (Settings → Bluetooth): one rule per line — `name:XXZ` (name starts with,
   case-sensitive; the default), `addr:AA:BB:CC:DD:EE:FF` (exact address), `prefix:AA:BB:CC`
   (address starts with). A matching device is flagged in every row (`target=1`), a `TARGET` row is
   logged on its first match in a session and again after it was unseen for 30 s, the LED flashes
   magenta three times, and the Dashboard lists it on top. Address rules only work reliably for
   devices with a fixed address.
6. **Motion** — the board's own movement, from the accelerometer alone (no gyroscope or compass):
   *still / moving / walking*, activity, steps and cadence, estimated walking speed and distance,
   the main horizontal axis it moves along (relative to the board, 180° ambiguous) and its tilt.
   Shown in the Dashboard's *Motion* card, as a 10-minute chart in Analyze, as a MOTION row in the
   log every second, and — if *Log 100 Hz acceleration* is on (Settings → Motion sensor, default
   on) — as every ~100 Hz sample in `<base>.motion.csv`. It answers "did it stand still or move
   around, and roughly how"; it does not track position. Speed is only estimated while walking
   (from step rhythm and step length); in a vehicle the state is just *moving*.
7. **Analyze** — one device in detail: decoded fields, the last advertisement and scan response as
   hex, its mean RSSI and packets per second over the last 2 minutes (kept on the board, so it's
   there even if the page was closed), and the air-activity chart.
8. **Recordings** — every logging session with its duration, rows, size and how it ended; download
   the CSV part(s), `.log` and `.mark`, rename or delete. Files are in `/ble` on the card.
9. **Settings → Scanning**: *Active scanning* (default on) asks every scannable device for its scan
   response; off = passive, the scanner never transmits and is invisible to the devices around it.
   *Coded PHY* adds BLE 5 long range, at the cost of fewer packets from ordinary devices.

The LED keeps VibraShock's vocabulary: blue breathing = logging, white blinks = start / stop /
marker, red = error, amber ramp = SD card nearly full, cyan = Wi-Fi connected, **magenta = target
device detected**.

### Scan log format

Each session is a set of files in `/ble` on the SD card, sharing a base name: the UTC start time
`YYYY-MM-DD_HHMMSS` (or `log_NNNNN` if the clock wasn't set) plus `_<name>` if one was given. The
card also carries a `FORMAT.md` that describes all of this — it's rewritten by the firmware
whenever it changes, so it always matches the board's firmware.

- `<base>.csv` — the log, rotating at 1 GB into `<base>_part2.csv`, ...; every part starts with
  the header row, rows are never split across parts.
- `<base>.log` — one line per event (`start`, `rotate`, `stop`, `full`, `error`) with board id,
  firmware, UTC, uptime, bytes, rows, duration and dropped rows. No end line = power loss.
- `<base>.mark` — the session's marker list (the markers are also rows in the CSV).
- `<base>.motion.csv` — optional: `uptime_ms, ax_g, ay_g, az_g` at ~100 Hz (gravity included,
  board frame), on the same clock as the log.

CSV columns: `kind, uptime_ms, utc, addr, addr_kind, rssi, event, props, phy, sid, tx_power,
data_status, name, name_src, company_id, mfg_data, ad_tx_power, flags, appearance, uuids, svc_data,
data, target, info`.

- `kind=ADV`: one received report — an advertisement or a scan response, legacy (`ADV_IND`,
  `ADV_DIRECT_IND`, `ADV_SCAN_IND`, `ADV_NONCONN_IND`, `SCAN_RSP`) or extended (`EXT_ADV`,
  `EXT_SCAN_RSP`). `data` is the complete raw payload as hex; the parsed columns are decoded from
  it. `name` comes from the per-address cache, `name_src` says whether this packet carried it.
  Extended payloads over 255 bytes arrive as several rows (`data_status` 1 = more follows).
- `MOTION` (once per second): `state`, `activity_g`, `peak_g`, `steps`, `cadence_spm`, `step_m`,
  `speed_mps`, `dist_m`, `axis_deg`, `axis_conf`, `tilt_deg`, `g` (gravity direction).
- `TARGET`, `MARK`, `STATUS` (once per second: reports/s, devices, drops, Wi-Fi, battery,
  temperatures), `INFO` (`clock_set`, `clock_adjusted`, `wifi_off`, `wifi_on`, `rules_changed`),
  `START` (scan settings, rules) and `STOP` rows carry key=value pairs in `info`.
- `uptime_ms` is always there and monotonic; `utc` (ISO 8601, ms) only once the clock is set — by
  NTP on a network with internet, or from the browser when the dashboard is opened. A
  `clock_set` INFO row marks the moment, so earlier rows can be re-timed from `uptime_ms`.
- Dropped data is counted, never silent: `drops` (reports lost before processing) and
  `log_drops` (rows lost before the card) in every STATUS row. Reports the radio controller itself
  discards under heavy load are not visible to the firmware.

## Live streaming (kept from VibraShock, not in the UI)

The firmware still has VibraShock's two full-rate (26.7 kHz) accelerometer streams — UDP (`POST
/api/udp/start`) and the binary WebSocket (`/wsstream`) — and `tools/udp_viewer.py`. They're kept
on purpose as a basis for a later lower-rate stream (motion data or live sightings), but the web
interface no longer controls them, and they still carry raw 26.7 kHz accelerometer data. The
formats below are unchanged from VibraShock.

#### UDP stream packet format

Sent to the target set from the dashboard's UDP card, `POST /api/udp/start` / `POST
/api/udp/stop`. Full raw rate (26.7kHz, no decimation) — UDP was chosen over TCP deliberately: this
is a live telemetry feed, so dropping a few milliseconds of old data during a network hiccup is
better than TCP's retransmission stalling or backlogging the sender. `seq` and `sample_index` both
reset to 0 on each `/api/udp/start` call, so a receiver can quantify exactly how many samples were
lost by diffing `sample_index` between packets (see the receiver snippet below) without needing
retransmission -- `sample_index` is the field to diff, not `seq`: it directly reports the sample
count, whereas `seq` would need multiplying by `sample_count` to mean the same thing.

The sender drains its ring strictly in order and never skips ahead: this project prioritizes
complete data over low latency for the UDP feed, so if the network can't quite keep up with the
full ~160KB/s raw rate for a while, delivery falls behind rather than samples being discarded to
catch up. The ring (256KB, ~1.6s at that rate) absorbs ordinary jitter; only if it fills up
completely does the write side (in `accel_capture.c`) start dropping, which shows up as `seq`/
`sample_index` gaps like any other loss.

16-byte header (little-endian), followed by `sample_count` raw `int16` X,Y,Z triplets:

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `uint8` | `magic` | fixed `0xAC` |
| 1 | `uint8` | `flags` | bits 0-1 = FS range code (0=2g, 1=16g, 2=4g, 3=8g), bit 2 = LPF2 enabled |
| 2 | `uint16` | `sample_count` | samples in this packet (242 by default) |
| 4 | `uint32` | `seq` | packet counter, resets on each `/api/udp/start` |
| 8 | `uint64` | `sample_index` | index of this packet's first sample, resets with `seq` |
| 16 | `int16[sample_count][3]` | samples | raw X,Y,Z, same units/scaling as the `.bin` format above |

Minimal Python receiver:

```python
import socket, struct

FS_TO_MG_PER_LSB = {0: 0.061, 1: 0.488, 2: 0.122, 3: 0.244}  # matches iis3dwb_sensitivity_mg_per_lsb()
HEADER = struct.Struct("<BBHIQ")  # magic, flags, sample_count, seq, sample_index

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 9000))  # match the port set on the dashboard

expected_sample_index = None
while True:
    data, _ = sock.recvfrom(2048)
    magic, flags, sample_count, seq, sample_index = HEADER.unpack_from(data)
    if expected_sample_index is not None and sample_index != expected_sample_index:
        print(f"lost {sample_index - expected_sample_index} sample(s)")
    expected_sample_index = sample_index + sample_count

    sens_g = FS_TO_MG_PER_LSB[flags & 0x03] / 1000.0
    samples = struct.unpack_from(f"<{sample_count * 3}h", data, HEADER.size)
    xyz_g = [v * sens_g for v in samples]  # feed this into your FFT
```

#### `tools/udp_viewer.py` — desktop viewer with live FFT

A ready-to-run PyQt/pyqtgraph GUI that implements the receiver above plus a proper live view:

```
pip install numpy pyqtgraph PyQt5
python tools/udp_viewer.py --port 9000
```

Then start streaming to this machine's IP from the dashboard's UDP card. Two tabs:

- **Time Domain** — scrolling X/Y/Z waveform (adjustable window length, per-axis show/hide,
  mouse drag/scroll to zoom and pan like any pyqtgraph plot). Uses pyqtgraph's `peak` downsampling
  so zoomed-out views still show transient spikes instead of averaging them away.
- **FFT** — live spectrum (selectable FFT size, window function, dB/linear and log/linear frequency
  axis, optional peak-hold overlay) with a draggable band on the plot (or numeric spinboxes) for
  the frequency range you care about, reporting the peak frequency and RMS within that band —
  either per-axis or as a combined `sqrt(x²+y²+z²)` magnitude spectrum.

A status bar shows live packet rate, drop count, and current FS range; "Pause" freezes the display
without stopping data collection, and "Save buffer to CSV" dumps whatever's currently buffered in
a `sample_index,time_s,x_g,y_g,z_g` column layout -- close to, but not identical to, the firmware's
own CSV export (below): the UDP/WS wire format carries no wall-clock field, so there's no
`unix_time_s` column here, only `time_s` relative to whenever streaming started.

#### Binary WebSocket live-stream format (`/wsstream`)

Same idea as the UDP stream, different transport: a browser can't open raw UDP sockets, so `/live`
gets the full-rate feed over a dedicated binary WebSocket at `/wsstream` instead, started/stopped
via `POST /api/wsstream/start` / `POST /api/wsstream/stop` (or the Start/Stop button on the `/live`
page itself). **Mutually exclusive with UDP streaming** — starting either one stops the other, since
sending full-rate data down both at once would double the load for no benefit. Unlike UDP, there's
no artificial single-client limit: every currently-connected `/live` tab gets broadcast the same
frames (bounded only by the server's overall 7-socket ceiling, shared with the low-rate `/ws`).

Same 16-byte header as the UDP format above (`magic`, `flags`, `sample_count`, `seq`,
`sample_index` — see the table above, byte-for-byte identical), followed by `sample_count` raw
`int16` X,Y,Z triplets. The only difference is the block size: **512 samples/frame** (3072-byte
payload, ~52 frames/sec) rather than UDP's 242-sample packets, since WebSocket runs over TCP and
isn't constrained by the UDP-MTU-fragmentation concern that sized the UDP packet. In JavaScript:

```javascript
ws.binaryType = "arraybuffer";
ws.onmessage = function(evt) {
  var dv = new DataView(evt.data);
  var sampleCount = dv.getUint16(2, true);
  var samples = new Int16Array(evt.data, 16, sampleCount * 3); // zero-copy view
  // ...
};
```

`src/live.html` implements this end-to-end (ring buffer, a self-contained radix-2 FFT, window
functions, dB/log-frequency toggles, peak-hold, an adjustable frequency band with live peak/RMS
stats) — deliberately close to `tools/udp_viewer.py`'s FFT tab feature set, so the Python tool is
optional rather than required for a quick live look.

## Architecture

> Inherited from VibraShock and only partly updated — the BLE scanner and the scan log are
> described in [ARCHITECTURE.md](ARCHITECTURE.md) §16–17.

- `src/accel_capture.c/.h` — the real-time path: a GPIO ISR on the sensor's FIFO-watermark
  interrupt wakes a dedicated drain task (core 1, highest priority) that DMA-bursts the FIFO over
  SPI and feeds five independent rings from the same samples, so none of them can ever contend
  with or block each other: a 4MB PSRAM ring for full-rate recording, a 1MB PSRAM ring
  boxcar-decimated to ~988.9Hz (26700Hz / 27) for a much smaller optional recording rate, a small
  ring decimated to ~26.7Hz (boxcar-averaged) for the low-rate live web graph, and two separate
  256KB full-rate (undecimated) rings — one for UDP streaming, one for the `/live` WebSocket
  stream. Separate rings rather than one shared one is deliberate: `ring_buffer_t` is
  single-producer/single-consumer, and each ring's dedicated consumer task always drains it
  (using it if active, discarding if not) so an inactive stream/rate never accumulates stale
  backlog — a shared ring gated by "whose turn is it" would leave everything undrained while
  nothing is active.
- `src/recorder.c/.h` — an always-running SD writer task (core 1, lower priority than capture)
  that drains *both* the full-rate and decimated recording rings every loop (discarding whichever
  one the current recording isn't using, per the always-drained rule above), writing to a file
  from whichever ring matches the active recording's chosen rate. Start/stop are asynchronous
  from the HTTP layer — a handler only flips a flag and returns. Durability: FATFS only commits a
  file's directory size + FAT chain on `f_sync`/`f_close`, so the writer `fsync`s every
  `FSYNC_INTERVAL_MS` (5s) during a recording (and right after the header) — an ungraceful stop
  (power loss, yanked card) then costs at most ~5s of data instead of the whole in-progress file,
  which would otherwise be orphaned as a 0-byte entry. The ~25s ring headroom absorbs the small
  metadata-write latency, so it costs no samples. Files still roll over at ~3.5GB (FAT32's 4GB/file
  limit), and a fatal write error (e.g. card full) stops the recording and reports it.
- `src/udp_stream.c/.h` — an always-running send task (core 0), same discard-when-idle idiom as
  the SD writer: drains its stream ring into fixed-size UDP packets (see the packet format above)
  and sends them when streaming is enabled, or just discards when it isn't, so the ring never backs
  up regardless of UI state.
- `src/ws_stream.c/.h` — structurally the same as `udp_stream.c` (always-running task, drains its
  own ring, discards when idle), but broadcasts binary WebSocket frames to every connected `/live`
  client instead of sending UDP datagrams to one fixed target — see the format above.
- `src/sd_benchmark.c/.h` — manually-triggered synthetic read/write throughput test, independent
  of recording.
- `src/power_ctrl.c/.h` — charger status polling and the POWER-button long-press → ship-mode
  sequence (stops/flushes any active recording first, then tells the BQ25628 to cut battery
  power via `BATFET_CTRL`). A single press while shipped wakes the charger and cold-boots the ESP
  — that's a hardware path (the button also drives the charger's QON pin directly), not firmware.
  Also owns the **Ignore TS** setting (Settings tab's Power card) — with no NTC thermistor
  populated, the TS pin floats and the BQ25628 reads that as a bias fault and blocks charging;
  this sets `NTC_CONTROL_0`'s `TS_IGNORE` bit to bypass that check. Persisted to NVS, re-applied
  at boot. `refresh_status()`'s ~1s poll loop also pets the charger's I2C watchdog
  (`CHARGER_CONTROL_0.WD_RST`, POR default 50s timeout) on every cycle -- per the datasheet,
  letting that watchdog expire silently reverts several registers to their power-on defaults,
  `TS_IGNORE` included, which is exactly what was undoing the ignore-TS setting ~50s after boot
  before this was found and fixed. Fast-charge current is set once at boot from
  `CHARGE_CURRENT_MA` (`power_ctrl.c`) via `bq25628_set_charge_current_ma()` -- a plain `#define`,
  not a dashboard setting, since the right value is a battery-safety decision (rule of thumb: at or
  below the pack's own 1C rating, numerically its mAh capacity) rather than something to casually
  flip from a web UI. Currently 2000mA (2A, the BQ25628's own hardware ceiling) for the ~3000mAh
  test battery in use -- update this constant first if the battery changes.

  Also runs a coulomb-counted battery-runtime estimate (`update_battery_estimate()`, called from
  `refresh_status()`'s existing ~1s poll -- reuses the `ichg_ma` reading already fetched there every
  cycle, no extra I2C traffic), shown in the dashboard's status bar as `NN% (~H.Hh left)`. Chosen
  over a voltage-to-charge lookup table because LiPo discharge curves are notoriously flat through
  the ~30-70% range -- no amount of data fixes that, it's the cell chemistry, not a measurement
  problem -- whereas the BQ25628's `IBAT_ADC` already reports signed current (positive charging,
  negative discharging), so integrating it directly sidesteps the flat-curve problem instead of
  fighting it. Two calibration anchors keep it self-correcting rather than just accumulating drift:
  a detected charge-completion snaps the running "mAh consumed" counter back to exactly 0, and
  crossing the fixed `BATT_GAUGE_EMPTY_MV` checkpoint (the hard cutoff, 3.4V -- so 0% lines up with
  where the device actually powers off) while discharging treats the counter's value at that
  repeatable point as a capacity measurement, EMA'd (`BATT_LEARN_EMA_ALPHA`) into the
  learned capacity so it adapts to the cell aging over many cycles. Nominal/learned capacity and
  progress persist to NVS (namespace `pwr_cfg`, same as TS-ignore/charge-current). Deliberately
  **not** exposed in the dashboard UI -- `power_ctrl_reset_battery_learning()` (called after
  physically changing the battery) is only reachable via `GET /api/battery/reset[?mah=<capacity>]`,
  an intentionally unlinked route (see `dashboard.c`); the reasoning being that this is a rare,
  technical, "you opened the case" action, not something to surface as a normal setting.

  "Charging" for this logic's own purposes is `ichg_ma > CHARGING_CURRENT_THRESHOLD_MA` (a small
  margin, not a literal `> 0`), deliberately **not** the charger's own `CHG_STAT` field the
  dashboard displays -- confirmed on real hardware that `CHG_STAT` can sit in taper/CV mode
  indefinitely under concurrent system load (recording + Wi-Fi + SD writes drawing from VSYS while
  charging), never reporting "terminated" even once the measured charge current has actually
  tapered to ~0mA overnight. The raw current is the more direct, reliable completion signal here.

  Also runs **low-battery protection** (`evaluate_low_battery()`), because the BQ25628's
  own battery cutoff (`VBAT_UVLO` ~2.2V per the datasheet) is far below where a single LiPo cell is
  safe -- a cell taken that low is damaged. It's voltage-based (robust even if the coulomb-counting
  gauge is uncalibrated) and debounced against transient sag from Wi-Fi TX / SD write current
  spikes. **Warning** at `BATT_LOW_THRESHOLD_MV` (3.6V), on battery:
  informational only -- the dashboard shows a battery-low banner with the (smoothed) estimated time
  left and the RGB LED pulses red, but the device keeps running and recording. (The gauge's 0%
  anchor is the 3.4V hard cutoff, not this threshold, so at 3.6V the estimate still reads a real
  nonzero time-left -- the 3.6V→3.4V runtime -- rather than pinning to 0%.) There's deliberately
  no timed pre-emptive shutdown: the real 3.6V→3.4V runtime is long and hard to predict, and the hard
  cutoff below already protects the cell, so a fixed timer just threw away usable runtime. The warning
  clears with hysteresis once VBAT recovers above `BATT_LOW_CLEAR_MV`. **Hard cutoff** at
  `BATT_HARD_MV` (3.4V): the single automatic power-off -- a non-overridable graceful shutdown to
  **shutdown mode**; per the datasheet's BATFET control table, shutdown mode (unlike ship mode) can
  *only* be exited by plugging in USB, so a nearly-empty cell can't be button-woken straight back
  into another low-battery loop; coming back always means connecting a charger. `enter_low_power_mode()`
  stops/flushes any active recording first (same safe path as the POWER-button long-press), and the
  LED gives a periodic low-battery blink while idle for headless use. Thresholds are all tunable `#define`s --
  kept deliberately conservative since runtime is effectively unlimited, so keeping a cell reserve
  costs nothing.
- `src/wifi_manager.c/.h` — always-on SoftAP + a smartphone-style multi-network STA manager + NTP +
  mDNS (`btscanner.local`, registered on both netifs). Remembers up to `WIFI_MAX_SAVED` (8)
  networks in an NVS blob (personal or enterprise; least-recently-used evicted when a 9th is added,
  by a `used_seq` stamp), and a dedicated **reconnect task** drives all connection attempts via a
  message queue (the event handler just forwards disconnect/got-IP events to it). Auto-connect
  tries saved networks **last-connected-first** with targeted connects (no RSSI pre-scan, so boot
  stays fast); the disconnect reason distinguishes *AP present but failed* (retry up to
  `WIFI_PER_NET_RETRIES` — real effort on an in-range network, then flag it "wrong password") from
  *not in range* (skip on immediately), and the pass is cyclic so no network is permanently
  abandoned. It **never gives up** — after a full failed pass it backs off and retries forever; the
  backoff is **use-aware**: escalating (20s→2min) while the device is active (a client polling the
  dashboard, a recording, or streaming — so the SoftAP stays smooth), but a fast ~5s when idle
  (nothing to disrupt, get back online quickly). A changed password is fixed by re-adding the
  network (updates the stored entry, clears the flag); Forget disconnects if it's the active one.
  Also owns the wall clock: NTP sync is authoritative when reachable; `wifi_manager_set_time_from_client()`
  is a `settimeofday()` fallback from the dashboard's browser clock, only applied while NTP hasn't
  synced (a real NTP sync always wins and keeps re-correcting on its own schedule after that). STA
  disconnects are translated from the raw `wifi_err_reason_t` into a short human-readable string for
  the dashboard's Wi-Fi card; `wifi_manager_scan()` does a blocking active scan for the add-network list.
  TX power defaults to full (this board may end up mounted somewhere with the client PC not
  nearby, e.g. a drone, so range shouldn't be sacrificed by default) -- `maybe_backoff_tx_power()`
  only drops it to a fallback level if `esp_reset_reason()` reports `ESP_RST_BROWNOUT` on
  `BROWNOUT_BACKOFF_THRESHOLD` (2) consecutive boots, tracked via a counter in NVS that any
  non-brownout boot resets to 0. That counter, not RAM, is what survives the reset, and reduced
  power is visible on the dashboard's Wi-Fi card when active.
- `src/web/dashboard.c/.h` + `src/dashboard.html` + `src/live.html` — `esp_http_server` based, two
  pages. The main dashboard's live graph is pushed over a WebSocket (`httpd_ws_send_frame_async` +
  `httpd_queue_work`, the pattern esp_http_server's own docs call out for async sends) rather than
  polled — this IDF build's `esp_http_server` runs a single accept/dispatch task by default, so a
  naive long-lived loop-inside-a-handler (e.g. SSE) would stall every other request while
  connected; a WS connection the *server* pushes to asynchronously doesn't have that problem.
  `/api/status` (charger, recording state, Wi-Fi, SD benchmark) stays plain request/response
  polling at ~1s, which is the right amount of complexity for slow-changing state. `/live` is a
  separate page (own WebSocket at `/wsstream`, see above) rather than folded into the main
  dashboard. Both `.html` files are embedded as plain C string constants (`tools/gen_html_c.py`
  regenerates `dashboard_html.c/.h` and `live_html.c/.h` in one run — real CMake `EMBED_TXTFILES`
  hits a path-resolution bug under PlatformIO's ESP-IDF integration).
- `src/drivers/` — `iis3dwb.c/.h` (register + FIFO driver),
  `bq25628.c/.h` (register driver, including `BATFET_CTRL` ship mode), `i2c_bus.c/.h` (shared I2C
  master bus) — the board's register-level drivers.
- `src/ring_buffer.c/.h` — generic PSRAM-backed single-producer/single-consumer byte ring buffer,
  used for all five of `accel_capture.c`'s rings (full-rate recording, decimated recording,
  preview, UDP stream, WS stream).

### Task/core layout

Capture and SD writing are pinned to core 1 (highest priorities) to keep them off the core the
Wi-Fi stack and httpd server run on (core 0) — the web dashboard should never be able to steal
cycles from the recording path. Runs at 240MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`, not the
default 160MHz) for headroom.

## Hardware notes

- **BQ25628 I2C address**: 0x6A on this board (not 0x6B as `SELFTEST_SPEC.md`'s schematic
  reference states) — confirmed via I2C scan on real hardware.
- **Charger CE (IO18)**: driven LOW at boot to enable charging (active-low).
- **POWER button (IO2)**: no internal pull — the external 100k network already provides the high
  level; enabling the internal pull-up would form a divider and prevent a valid LOW read.
- **Buttons**: POWER (IO2) double-click starts/stops logging, single click marks, 3 s hold powers
  off. BOOT (IO0) is unused.
- **IIS3DWB SPI**: 8MHz (hard limit is 10MHz), mode 0. ODR is fixed at 26.7kHz — not a tunable
  parameter, it's the only rate this part supports.

## Out of scope for now

An on-device neural network for predictive maintenance, reliable delivery/retransmission for the
UDP/WebSocket streams (sequence numbers are enough for a live telemetry link), and encrypting the
Wi-Fi credentials stored in NVS (plaintext — acceptable for a thesis bench setup, not production).
