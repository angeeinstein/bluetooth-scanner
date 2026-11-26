# Changelog

All notable changes to the VibraShock recorder firmware. Format based on
[Keep a Changelog](https://keepachangelog.com/); versions follow semantic
versioning and match `FIRMWARE_VERSION` in [`src/version.h`](src/version.h).

When cutting a release: move the `[Unreleased]` notes into a new dated
`## [x.y.z]` section, bump `FIRMWARE_VERSION`, commit, and push — the build then
publishes a GitHub release using that section as the notes (see the README).

## [Unreleased]

## [1.6.0] - 2026-11-01

Recordings now log **temperatures, power and signal quality once per second**
in a new `.aux.csv` next to each recording, and every SD card carries a short
**`FORMAT.md`** describing all recording files. Also fixes the Wi-Fi signal bars
(they always showed full strength) and two smaller bugs.

### Added
- **Once-per-second side channels for every recording.** Each part now gets an
  `.aux.csv` next to its `.bin` with one row per second: sensor, board (ESP32)
  and charger temperatures, USB/battery voltage and current, battery level,
  charging state, Wi-Fi signal strength, and data-loss / per-axis clipping
  counters. Rows are keyed by `sample_index`, so they line up exactly with the
  samples. Before, temperature was only logged at the start and end. The `.bin`
  format is unchanged; the file is listed under the recording's files in the
  dashboard and is downloaded, renamed and deleted together with the recording.
  Column reference: README → Recorded file format.
- **The SD card now describes its own file format.** The recordings folder
  gets a short `FORMAT.md`: header layout, time-sync table, sample block, and
  what the `.log`, `.aux.csv` and `.mark` files contain. A script or AI agent
  handed just the card can read recordings without guessing or looking up this
  repository. It's written after each card mount and kept in step with the
  running firmware (rewritten only when outdated).

### Fixed
- **The Wi-Fi signal bars always showed full strength.** The signal level was
  never updated after connecting, so it always read 0 dBm (= "excellent"). The
  bars now show the real signal.
- **Downloads of recordings with long names could fail.** The download
  endpoint cut file names off at 63 characters, so a long recording name (up to
  ~128 characters are allowed) returned "file not found".
- **A board whose charger isn't detected could reboot when opening the
  dashboard.** Reading the power status before the charger was set up hit an
  internal assertion; it now just reports "no data".

## [1.5.0] - 2026-09-25

Adds **full second-stage filter control** for the IIS3DWB accelerometer
(low-pass or high-pass with 8 selectable cutoffs, where before there was only an
on/off toggle) and **pinning a device to one specific access point**, the
groundwork for time-aligned multi-device recordings. Also restores **full-rate,
low-latency UDP streaming** and makes Wi-Fi auto-reconnect recover from a
network wrongly flagged "wrong password".

### Added
- **Full IIS3DWB second-stage filter control.** The old on/off LPF2 toggle only
  ever used one setting (low-pass at the widest ~6.3 kHz). Settings → Sensor &
  scale now exposes the whole filter: keep the on/off toggle, and when on choose
  a **Type** (Low-pass / High-pass) and a **Cutoff** (8 steps, ~6.3 kHz down to
  ~33 Hz — `HPCF_XL`). High-pass removes the DC / 1 g gravity offset, passing only
  vibration above the cutoff (useful for AC-only work at a smaller range). The
  choice applies immediately (blocked during a recording), is optionally
  remembered across reboots, is carried through auto-range range changes, and is
  stamped into each recording's `.log` (and shown per-recording under Filter) so
  the data stays self-describing for offline analysis.
- **Pin to a specific access point.** Wi-Fi normally joins the strongest AP of a
  network; you can now pin a device to one exact AP — Settings → Wi-Fi → **Access
  point** → "Pin to a specific AP…", which scans and lists each AP of your network
  by BSSID · channel · signal. This lets several VibraShock devices share one
  access point, which is what time-aligned (TSF) multi-device recordings need.
  It's a **soft, opt-in** pin: if the chosen AP is out of range the device falls
  back to a normal connect, so it can never strand itself, and un-pinned Wi-Fi
  behaves exactly as before. Persisted across reboots (stored separately from the
  saved networks, so existing Wi-Fi is untouched by the update).

### Fixed
- **UDP live streaming fell ~1.6 s behind and dropped ~9 % of samples.** The
  sender yielded a full tick (10 ms) after every packet, capping it at 100
  packets/sec — just under the ~110 packets/sec the 26.7 kHz stream produces. The
  backlog grew until the ring buffer was full (≈1.6 s of latency) and then
  samples were dropped at the write side. It now drains a short burst per wake, so
  the ring stays near-empty: full sample rate, latency down to ~10 ms. (WebSocket
  streaming was never affected — its larger frames need only ~52/sec.)
- **Auto-reconnect could get permanently stuck on a network flagged "wrong
  password".** A transient/AP-side refusal (e.g. a UniFi "Lock to AP" steering the
  client, a roam, or weak signal) is reported to the ESP much like a credential
  failure, so the reconnect logic would mark the network `auth_failed` and then
  only retry it once per pass — leaving it effectively stranded even after the AP
  issue cleared, while a manual "Connect" from the dashboard worked immediately.
  Auto-reconnect now clears those marks after a few fully-failed passes, so every
  saved network gets a fresh full retry and a mis-flag self-heals with no manual
  action (a genuinely wrong password just gets re-flagged next pass, so the badge
  still works when it's real).

## [1.4.7] - 2026-09-12

### Fixed
- **Console error "Data frame received after close" when leaving the Analyze
  tab.** Live-stream frames are queued a few deep for async delivery; when the
  tab closed the socket, one already-queued frame could land after the browser's
  close handshake, which the browser rejects and logs (harmless on the device —
  no serial error). The server now re-checks the socket is still a live
  WebSocket at send time (not just when queuing), and the page stops the stream
  before closing the socket, so in-flight frames arrive while it's still open.

## [1.4.6] - 2026-09-12

### Fixed
- **A dashboard endpoint stopped registering (`/api/wsstream/stop` returned 404).**
  The HTTP server's URI-handler limit (48) had fallen one short of the 49 handlers
  now registered, so the last one silently failed to register — the boot log
  warned "no slots left for registering handler" and the endpoint 404'd. Raised
  the limit to 60, with headroom for future endpoints.

## [1.4.5] - 2026-09-12

The decisive fix for the cloud-update download on this memory-tight board: the TLS
buffers now live in PSRAM instead of the exhausted internal heap.

### Fixed
- **Cloud firmware download TLS out-of-memory — root fix.** During a bulk transfer
  the update server sends full ~16 KB TLS records, so mbedTLS must allocate a
  ~16 KB record buffer — and the internal heap (shared with Wi-Fi + httpd) has no
  16 KB contiguous block, so the download failed with `alloc(...) failed` /
  `-0x7F00`. Freeing a few KB (1.4.4) couldn't create a 16 KB hole. mbedTLS memory
  now lives in the **8 MB PSRAM** (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`), which has
  ample room. This changes **only TLS** — the OTA download and the periodic
  device-IP report; the live WS/UDP stream is not TLS, so **simultaneous recording
  + live streaming is unaffected** (deliberately not relocating Wi-Fi/LWIP, which
  would have touched that path). The 1.4.4 during-OTA reclaim (pausing streams,
  freeing the reporter's stack) stays as extra headroom.

## [1.4.4] - 2026-09-12

Completes the cloud-update fix chain: 1.4.3 got the install task to *start*, but
on this memory-tight board the download's TLS then couldn't allocate its record
buffer mid-stream. The updater now frees internal RAM for the duration of a
download.

### Fixed
- **Cloud firmware download ran out of internal RAM mid-transfer.** After 1.4.3
  let the install task start, the download's TLS reads failed with
  `Dynamic Impl: alloc(...) failed` / `-0x7F00` — not enough free internal RAM
  remained for the TLS record buffer (the device-report task's own TLS POSTs
  succeed only because the install task's stack isn't present at that time).
  During an OTA the firmware now pauses live streaming and frees the device-report
  task's ~8 KB stack — both idle or irrelevant while updating (recording is
  already refused) — restoring the free heap to the level where TLS already
  works. Everything is restored when a failed update returns (or mooted by the
  reboot on success); nothing touches the recording path.

## [1.4.3] - 2026-09-12

Another OTA fix on top of 1.4.1: on this board the cloud update couldn't even
*start* — "could not start update task" (HTTP 409) — because the install task's
stack couldn't be allocated in the scarce internal RAM.

### Fixed
- **Cloud update "could not start update task" (HTTP 409).** The OTA install task
  needs its stack in *internal* RAM (FreeRTOS task stacks can't live in PSRAM),
  and Wi-Fi + LWIP + httpd had left too little contiguous internal heap for the
  old 12 KB stack, so `xTaskCreate` failed before the download even began. The
  install task now uses an **8 KB stack** — the size ESP-IDF's own
  `esp_https_ota` example uses for the same TLS + flash-write work — which fits
  reliably. (Deliberately *not* fixed by relocating Wi-Fi/LWIP buffers to PSRAM:
  that would free internal RAM but add PSRAM-bus contention to the recording ring,
  and simultaneous recording + live streaming must keep working as before.) The
  failure message now also reports the free internal RAM, so a genuine low-memory
  condition is diagnosable straight from the dashboard.

## [1.4.1] - 2026-09-12

A follow-up to 1.4.0 that fixes the cloud firmware **download**, which was failing
at the TLS handshake because the internal heap ran out of a contiguous block — an
out-of-memory error, not a network problem (so 1.4.0's download retry couldn't
help).

### Fixed
- **Cloud firmware downloads now actually complete.** 1.4.0's whole-download retry
  couldn't help because the failure was never a transient network drop — the TLS
  handshake for the ~1.5 MB HTTPS download was running the internal heap out of a
  contiguous 16 KB block and failing every time with `mbedtls_ssl_setup ->
  MBEDTLS_ERR_SSL_ALLOC_FAILED`. TLS now uses **dynamic buffers**
  (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`), which allocate the record buffer incrementally
  during the handshake instead of grabbing a fixed 16 KB up front, so it needs far
  less contiguous RAM. The periodic device-address report also holds off its own
  TLS session while a download is in flight, so the two never compete for memory.

## [1.4.0] - 2026-09-12

Adds **find-my-device address reporting**: each board can tell the update server
its current local IP so you can reach it on any network without mDNS. Alongside
it, two reliability fixes — cloud firmware downloads now survive a dropped
connection, and boards running without a battery (straight off USB) are handled
cleanly instead of showing a phantom full cell.

### Added
- **Find-my-device address reporting.** Each board now POSTs its current local IP
  (plus hostname, Wi-Fi SSID, firmware, battery, and recording status) to the
  configured update server on every new IP — first connect and any later DHCP/AP
  change — and on a ~5-minute heartbeat while online. That backs a public
  `…/find/<device_id>` lookup, so you can find a board's address on a network
  where mDNS doesn't reach (or after DHCP moves it) and open `http://<local-ip>/`
  directly. Reuses the OTA endpoint URL and Cloudflare Access token — nothing new
  to configure — and does nothing until an update server is set. Runs on a
  low-priority background task that only reads existing state and never touches
  the sampling/SD path; failed POSTs are logged and retried with backoff (and the
  request itself retries transient connect failures), never fatal. Can be turned
  off with a "Send status reports" toggle (Settings → Wi-Fi, default **on**).

### Fixed
- Cloud firmware **downloads are now resilient to a dropped connection**. A
  ~1.5 MB download that broke partway (the TLS stream failing mid-transfer, or
  the server closing early) previously failed the whole update with "download
  interrupted"; it now retries the entire download a few times with backoff, since
  an OTA write can't resume mid-stream. Genuine problems (a bad HTTP status, a
  flash-write error, or a complete-but-rejected image) still fail immediately so
  real errors aren't hidden behind retries.
- Running without a battery (this board can run straight off USB) is now handled
  cleanly. Previously a battery-less board showed a phantom 100% cell and a
  charging state; the firmware now detects the absent battery (debounced VBAT:
  an empty connector floats near 0 V, a real cell never reads below ~2.5 V) and
  shows "no battery" — no battery gauge, no charging indicator, and never a
  low-battery warning. Detection is deliberately conservative (a wide threshold
  gap) so a board that *does* have a cell is never misdetected as empty.

## [1.3.0] - 2026-09-12

Adds **multi-device time synchronization**: recordings from two or more boards on
the same Wi-Fi access point can now be time-aligned offline to ~microseconds,
using the 802.11 TSF clock every associated device shares. Each recording embeds
periodic TSF checkpoints (`.bin` format v3), and a **measured true ODR** replaces
the nominal 26.7 kHz label so frequency analysis lands on the right axis. Also a
grab-bag of reliability fixes: graceful stop when the SD card fills, much faster
SD benchmark and downloads, and a no-card boot-loop fix.

### Added
- **TSF time-sync checkpoints in recordings (`.bin` format v3).** Each recording
  now stores a table of `(sample_index, TSF, BSSID)` checkpoints — one at the start
  plus every ~60 s — so recordings from multiple devices on the same access point
  can be time-aligned offline to ~microseconds (far tighter than NTP's
  milliseconds). Each checkpoint carries the AP BSSID its TSF belongs to, so a
  mid-recording Wi-Fi drop or roam to a different AP is handled cleanly (align on
  checkpoints whose BSSID matches; a zero-TSF slot was taken while unassociated).
  The table is a fixed ~12 KB block between the 64-byte header and the sample data
  (rewritten on each fsync, so a crash keeps all but the last few seconds); the
  header bumps to version 3. Readers: sample data now starts at
  `64 + header.sync_table_bytes` and `samples = (size − 64 − sync_table_bytes) / 6`
  (v2 files have `sync_table_bytes == 0`, so the same math still works). The live
  TSF clock is also shown under Settings → Wi-Fi.
- **Measured accelerometer ODR.** The firmware now measures the true output data
  rate (the nominal 26.7 kHz is only a label) as a long-baseline average that keeps
  tightening the longer the device runs. Shown live under Settings → Device &
  firmware, and stored in each recording's header so offline frequency analysis
  uses the real rate — a wrong rate shifts every computed frequency by the same
  factor. The header's rate field is refreshed at file close with the most accurate
  (end-of-recording) estimate. Backed by the sensor's hardware timestamp counter as
  an independent clock cross-check.
- **Estimated sample loss.** The dashboard status card now shows roughly how many
  samples FIFO overruns actually cost (a magnitude next to the overrun event count,
  e.g. "9 · ≈350 samples (0.0002%)"), so it's clear at a glance whether loss is
  negligible. Also recorded in each recording's log.
- **External sync trigger (IO38).** A shared rising-edge signal fed to IO38 on
  multiple boards latches each board's Wi-Fi TSF at the same instant — a hardware
  reference for measuring (and later enforcing) cross-device time sync, free of
  Wi-Fi/HTTP timing jitter. Exposed as an API (`GET /api/sync/edges` for the
  per-edge `(n, TSF)` log, `POST /api/sync/reset` to zero the counter); align the
  logs from two boards by edge number `n`. No dashboard UI — it's a
  measurement/wired-sync hook, driven from a script. Drive IO38 with 0–3.3 V logic
  only.

### Changed
- **Per-device unique network identity** (so multiple boards don't collide). Each
  device now derives a unique mDNS/DHCP hostname and SoftAP SSID from its device id
  — e.g. `vibrashock-ccef4c.local` and `VibraShock-CCEF4C` — and always answers to
  that unique name. A per-device "Answer to vibrashock.local" toggle (Settings →
  Wi-Fi, default **on**) keeps the friendly shared name working: leave it on for a
  single device, and turn it off on all but your main one when you run several, so
  `vibrashock.local` reaches one board unambiguously. Each device also advertises a
  unique mDNS service instance, groundwork for a future single-page multi-device
  view. (Heads-up on upgrade: your SoftAP SSID and the `.local` name change.)
- Dashboard: the current full-scale range now sits next to the "preview" label on
  the Live signal card; dropped the redundant "open Analyze for full-rate" hint.
  The sensor name in the status bar stays desktop-only (hidden on mobile).

### Fixed
- Update server settings are now forgiving of two common paste mistakes: a
  server URL with a trailing slash (which doubled into `//version` and made the
  update check fail with HTTP 404) is trimmed, and a CF-Access Client Id/Secret
  pasted with its header label still attached (`CF-Access-Client-Id: …`) has the
  label stripped. Cleaned both on the device (so any client is covered) and live
  in the input fields.
- Recording detail: each file's size is now shown on its download button, so
  multi-part (rotation-split) recordings show every part's own size, not just the
  group total — without widening the file list.
- SD card full is now a graceful stop, not an error: filling the card ends the
  recording cleanly (everything written is kept) and shows a neutral "SD card full
  — recording stopped and saved" notice instead of a red "write failed (errno=28)"
  error. Only genuine write faults are still reported as errors.
- SD benchmark: when the card is full it now says so plainly ("SD card full — free
  up space to run the benchmark") instead of a cryptic "short write at 0 bytes",
  and cleans up its partial temp file.
- SD benchmark now measures the card's real throughput: it reads/writes via raw
  DMA-capable transfers instead of buffered stdio through a PSRAM buffer (which the
  SDMMC DMA can't reach and had to bounce), so the numbers reflect the actual
  40 MHz/4-bit bus rather than the bounce path. It takes the largest DMA-capable
  internal buffer available (down to 4 KB) so it doesn't run out of memory when
  internal RAM is tight. Also logs the negotiated SD bus speed and width on mount.
- Web file downloads are faster: the download now reads the card into DMA-capable
  internal RAM via raw reads instead of a PSRAM buffer the SDMMC DMA had to bounce.
  (Downloads are ultimately capped by Wi-Fi throughput, so the gain is bounded, but
  the SD read is no longer the bottleneck.) Also speeds up the "Convert in browser"
  flow, which downloads the .bin first. The fast path is only used when no recording
  is active (and capped small); while recording, downloads use the old PSRAM path so
  they can never take internal memory the recorder needs — recording stays priority.
- Fixed a boot loop when powering on with no SD card inserted: the LED status
  poll task read Wi-Fi status before the Wi-Fi manager had created its mutex (the
  SD mount timeout delayed init enough to expose the race), which asserted and
  reset the device repeatedly. Wi-Fi status now reports "not connected" until it's
  ready instead of crashing.

## [1.2.0] - 2026-09-07

Adds **clipping detection** and optional **auto-range on clipping** — the recorder
now sees when the signal saturates full scale and can step the measurement range
up mid-recording, so a long unattended run isn't ruined by clipping. Rotation-split
recordings also group and expose all their parts correctly now.

### Added
- **Live clipping detection.** The firmware measures, on the raw full-rate
  samples (where the dashboard preview's boxcar averaging would otherwise hide
  it), the per-axis percentage of samples hitting full-scale saturation, and
  reports it in `/api/status`. The dashboard shows a red "clipping" badge on the
  live graph naming the affected axes and worst percentage when it happens — a
  cue to raise the measurement range before signal peaks are lost (which matters
  most mid-recording, since a clipped recording can't be recovered). The check
  costs ~0.1 % of a core.
- **Auto-range on clipping** (opt-in, Settings → Capture, off by default). When a
  recording saturates full scale for a sustained period (~0.3 % of samples for
  ~5 s on any axis), the device automatically continues it in a **new rotation
  part at a larger range** — briefly probing the true peak at ±16 g (~1.5 s) and
  picking the smallest range that fits it, escalating up only, capped at ±16 g.
  It runs headless (no dashboard needed), so a long unattended recording isn't
  ruined by clipping; each part keeps its own single, self-describing range in
  its header, and the range step is recorded in the new part's log. The
  auto-bumped range is **runtime-only** — it never overwrites your remembered
  scale/filter preference, so a reboot restores what you set.
- **"Clipped" recording badge + range provenance.** A recording that clipped for
  a sustained period is flagged **⚠ clipped** in the list and detail (separate
  from the sample-integrity "clean" badge, since a recording can be sample-clean
  yet saturated), with the peak clip % shown. The detail's Range field shows the
  full range chain across auto-range steps (e.g. **±2 g → ±8 g → ±16 g**), and the
  Log panel now shows every part's log, not just the first.

### Changed
- **Measurement range & LPF2 apply immediately** — the Sensor &amp; scale card's
  *Apply* button is gone; changing the range or the LPF2 toggle now takes effect
  on the spot (consistent with every other setting), and the two controls are
  disabled while a recording is in progress.
- **Removed the recordings view's "Convert on device" button**, leaving
  "Convert in browser" as the CSV path — on-device conversion of a multi-GB
  recording is impractically slow, and the raw `.bin` is the working format
  anyway. The on-device converter and its `/api/export/csv` endpoint stay in the
  firmware for possible future use; only the button is gone.

### Fixed
- **Rotation-split recordings group into one entry again.** A recording past the
  ~3.5 GB rotation limit named its parts with fresh timestamps (`…_1430_x.bin`
  then `…_1445_x_part2.bin`), so they didn't share a base — the dashboard listed
  only part 1 (with only its size/duration) and orphaned the rest. All rotation
  parts now reuse part 1's filename base, so the recording shows as a single
  entry with size, samples, duration and end-temperature aggregated across every
  part, and each part is individually downloadable from the detail sheet.
- **The dashboard's "live" connection indicator now reflects real reachability.**
  It was tied to the `/ws` preview socket, which can stay "open" long after the
  device silently drops (no TCP FIN seen), so it kept showing green when the
  device was actually gone. It now follows the 1 s `/api/status` poll — turning
  red ("offline") after a few missed polls and back to green when it recovers.
- Corrected the live-graph label from "preview ~27 Hz" to "~107 Hz" (the preview
  rate was raised in 1.1.1).

## [1.1.2] - 2026-09-05
### Fixed
- **Cloud firmware updates now retry a failed download connection instead of
  giving up.** The download step could intermittently fail to connect
  (`ESP_ERR_HTTP_CONNECT`) — a socket momentarily unavailable while the dashboard
  held its own, the just-finished version check still in TCP `TIME_WAIT`, or a
  DNS hiccup on a locked-down network — and surfaced the error immediately, so
  you had to press *Update* again. It now retries the connect a few times with a
  short backoff before failing.

## [1.1.1] - 2026-09-05
### Changed
- **Smoother, faster live preview graph.** The dashboard's live acceleration
  graph now updates at ~107 Hz (up from ~27 Hz) with a 60 Hz redraw, so fast
  motion no longer looks stepped and the trace reads as continuous. It's still a
  lightweight decimated feed — the Analyze tab remains the full-rate view — and
  it adds no extra connections, only a couple of KB/s per client.
- **The Analyze FFT band now defaults to the full spectrum.** The *Band (Hz)*
  inputs default to `0`–`13350` Hz (Nyquist), matching the graph and the in-band
  stats on load, instead of the old `10`–`1000` that left the inputs and the
  plot disagreeing.
- **The Analyze FFT linear Y range defaults to 0–0.1 g** (was 0–1 g), so a
  typical spectrum is visible instead of hugging the axis when *dB scale* is off.
- The status-LED recording "breathe" now dims further at its low point, making
  the slow blue pulse easier to read.

### Fixed
- **Analyze time-domain "Autoscale Y" actually autoscales now.** It previously
  only switched between the sensor's full-scale range and a fixed ±2 g —
  identical at the ±2 g range, so it looked like it did nothing. It now fits the
  Y axis to the real signal in the window (nice-rounded, capped at full scale).
  It defaults to off.

## [1.1.0] - 2026-09-05

The biggest release since 1.0.0, headlined by a **complete redesign of the web
interface**: the dashboard and the separate live / FFT / spectrogram page are
merged into one cohesive single-page app (Dashboard / Analyze / Recordings /
Settings), rebuilt on a new design system and made to work properly on phones.
It also reworks the low-battery behaviour into a warning-plus-hard-cutoff model,
adds a full RGB status-LED system for the new WS2812 boards, and fixes several
stability issues in live streaming and status polling.

### Changed
- **The web interface is now a redesigned single-page app.** `dashboard.html`
  and the former `live.html` (full-rate stream, FFT, spectrogram) are merged
  into one page with **Dashboard / Analyze / Recordings / Settings** tabs — a
  bottom tab bar on phones, and a top status bar showing Wi-Fi signal bars, the
  UTC clock, charger/battery and SD free space. The whole console was restyled
  and restructured into a single deep-navy design system: a Dashboard "cockpit"
  (live graph + a single record start/stop control with marker chips + a
  status/live-statistics column), a Settings side-nav grouping
  Capture · Wi-Fi · Power &amp; battery · Device (measurement range and the LPF2
  filter now live under Capture, and every toggle is a switch), and an Analyze
  tab carrying the Time / FFT / Spectrogram views with an in-browser / UDP
  source switch and a fullscreen (landscape on mobile) mode. The old `/live` URL
  redirects to the new page, and the Analyze tab opens the full-rate `/wsstream`
  only while it is visible. The redesign keeps the same `/api/*` endpoints and
  WebSocket protocol.
- **Low battery is now a warning, not a countdown.** The old fixed 15-minute
  soft "nag" countdown that shipped the device pre-emptively at 3.6 V is gone
  (along with its *Extend runtime* button). The real 3.6 V→3.4 V runtime is far
  longer than 15 min and hard to predict, and the **3.4 V hard cutoff already
  protects the cell** (it stops and flushes any recording, then shuts down),
  so the pre-emptive timer only threw away usable runtime. Now 3.6 V just
  raises a **warning** — the status bar shows "LOW" with the estimated time
  left, the dashboard shows a battery-low banner, and the RGB LED gives a
  steady red pulse — while the device keeps running and recording all the way
  to the hard cutoff, the single automatic power-off. The displayed runtime
  estimate is also **smoothed more** (~10 s vs ~3 s), so it no longer jumps
  around with per-cycle current spikes (SD writes, Wi-Fi bursts). And the fuel
  gauge's **0% point moved from 3.6 V to the 3.4 V cutoff**, so the shown
  percentage and time-left now count the full usable range down to where the
  device actually powers off — previously the estimate hit 0% at 3.6 V, exactly
  where the warning starts, so the warning's "time left" would have read ~0.
- **Recording marker sidecars (`.mark`) are now CSV** — a single header row
  (`id,time_s,unix_time_s,note`) then one line per marker — matching the
  comma-separated recording data export, so they can be imported straight into
  external analysis tools as events. The note is quoted (so a comma in it can't
  break the row); `unix_time_s` is blank when the device clock wasn't synced at
  record time.

### Added
- **RGB status LED (optional WS2812 boards).** A board revision can swap the
  plain status LED for a WS2812C-2020 addressable RGB LED (DI on the same
  `PIN_LED`, through the existing series resistor); set `STATUS_LED_WS2812` to
  `1` in [src/pins.h](src/pins.h) to drive it. Left at `0`, the original plain
  active-low LED behaves exactly as before, so the existing boards are
  unaffected. On RGB boards a new [src/led_indicator.c](src/led_indicator.c)
  owns the LED and shows what you can't otherwise see on a headless device:
  **blue breathe** = recording · **white flashes** = button feedback (marker 1 /
  start 2 / stop 3) · **cyan flash** = Wi-Fi (re)connected · **green/red flash**
  = accelerometer self-test pass/fail · **red** = error · an **amber
  accelerating ramp** as the SD card fills while recording (starts 45 min out,
  tightening to solid) · a **steady red pulse** = low-battery warning · a
  **white ship-hold ramp**; idle is off (the separate power LED already shows
  "on"). Colours are a named palette kept separate from their meaning, states
  re-evaluate every tick, and the board does a red/green/blue check at power-on.
  A **party button** (Settings → Device) also fires a few seconds of colourful
  animation — a smooth rainbow on the RGB boards, a fast strobe on the plain
  ones. Driver: [src/led_rgb.c](src/led_rgb.c) (single-pixel WS2812 over the RMT
  peripheral).

### Removed
- **`POST /api/battery/extend`** and the dashboard's *Extend runtime* button —
  the low-battery soft countdown they drove no longer exists (see Changed).
- **`battery.countdown_s`** from the `/api/status` payload, for the same reason.
- **Reading of the old space-separated `mark …` marker-sidecar format** — `.mark`
  files are CSV now, so markers from recordings made before this release won't
  display in the recordings view.
- **Migration of Wi-Fi credentials from the pre-list single-network firmware** —
  a device updated from that early build will need its Wi-Fi networks re-entered.

### Fixed
- **The "SD card not mounted" error clears once the card is back.** It was set
  on a start attempt with no card and only cleared on the next successful start,
  so re-inserting/remounting the card left the message on the dashboard even
  though the card was working again. Any stale storage error is now dropped as
  soon as the card (re)mounts.
- **A recording's markers now show immediately in its detail view.** The detail
  sheet fetches two small sidecars (`.log` and `.mark`) at once, but both went
  through the single big-file download worker, so the second got a 503 and the
  markers silently didn't render (or only appeared seconds later on the next
  list refresh). These small text sidecars are now streamed inline, skipping the
  one-at-a-time worker gate.
- **Live streaming no longer slowly exhausts the heap and stalls.** The
  full-rate `/wsstream` (and the dashboard `/ws` preview) queue each outgoing
  frame as an async send holding a malloc'd copy of the payload; a browser on
  Wi-Fi can't keep up with 26.7 kHz, so the httpd task drained those sends
  slower than they were produced and the queued copies piled up — heap fell
  from ~2.4 MB to ~220 KB over about an hour on the Analyze tab, then the stream
  stopped (with `httpd_sock_err … 104/128` in the log). Both channels now cap
  the number of **outstanding async sends** and drop frames while over the cap,
  bounding the streaming path's heap use (the live view was already dropping
  most frames under that load anyway).
- **`/api/status` no longer returns 500 (and blanks the dashboard) under heap
  pressure.** During a long recording the heap can fragment enough that the
  4 KB `malloc` the status handler used per poll fails; it now builds the JSON
  in a static buffer instead (safe — the HTTP server dispatches handlers
  serially), so the dashboard keeps working. The status now also reports
  `chip.free_heap` (shown in Settings → Device → *Free heap*) and the connected
  Wi-Fi `rssi`, so heap pressure and signal strength are visible.

## [1.0.3] - 2026-09-01
### Fixed
- **Internet update check/download now connects** (previously failed with
  `connect failed: ESP_ERR_HTTP_CONNECT` and `Failed to create socket … sock <
  0`). The LWIP socket pool (10) was fully claimable by the dashboard web server
  alone — 7 client connections plus 3 reserved internally — leaving no socket
  for the outbound HTTPS client that fetches updates once mDNS/NTP and a couple
  of browser connections were live. Raised the pool to the LWIP maximum (16) so
  the update client always has a free socket. The manual `firmware.bin` upload
  was unaffected (it reuses the browser's existing connection).

## [1.0.2] - 2026-09-01
### Added
- **Internet firmware updates.** A *Check for updates* button on the dashboard
  pulls the latest release from a configurable HTTPS endpoint (a Cloudflare
  Worker fronting the GitHub releases) and installs it over the air, with a
  numeric version comparison so it only offers strictly newer builds. The server
  URL and Cloudflare Access service token are stored on the device (Settings →
  *Update server*), not compiled in. The manual `firmware.bin` upload stays, and
  now also accepts a **drag-and-dropped** file.
- The About card now shows the reason for the device's last restart (power-on,
  brownout, panic, watchdog, …) — highlighted when it's a brownout/crash — so
  unexpected resets can be diagnosed in the field without a USB serial monitor.

### Fixed
- **Recordings no longer stop when the device changes Wi-Fi networks or
  reconnects.** The NTP client was re-initialised on every reconnect, which
  asserted deep in lwip (`operating mode must not be set while SNTP client is
  running`) and panicked/rebooted the device mid-recording. SNTP is now set up
  once and only re-checked afterwards.
- Recording `.log` sidecars are `fsync`'d right after the start line, so a
  recording cut short by a reset/brownout keeps its log (settings + provenance)
  instead of leaving a 0-byte file (FATFS only commits a file's size on
  sync/close).

## [1.0.1] - 2026-08-31
### Changed
- Firmware-update card: the file picker uses the standard dashboard button style,
  and its on-screen text no longer describes the build-token check.
- Recordings now report the FIFO overruns and ring drops that occurred *during
  that recording* (end minus start of the cumulative counter) instead of the
  raw since-boot counter, so a clean recording shows "✓ clean".
- The Wi-Fi card shows the device clock (UTC) — including when it was set from
  the browser rather than NTP — alongside the NTP-synced state.

### Fixed
- The one or two benign FIFO overruns that happen at boot (while the sensor FIFO
  first fills) are no longer counted in the live "Sensor FIFO overruns" figure.

## [1.0.0] - 2026-08-31

First tagged release — the vibration-recorder firmware (bachelor's thesis
project) for the ESP32-S3 board with the ST IIS3DWB accelerometer.

### Added
- **Full-rate acquisition** — IIS3DWB captured at its fixed 26.7 kHz rate via
  FIFO watermark interrupt + DMA burst reads, fanned out through lock-free
  single-producer/single-consumer ring buffers, with sensor-overrun and
  ring-drop integrity counters.
- **Recording** to raw `.bin` (64-byte header + packed int16 X/Y/Z), with an
  optional ~988.9 Hz boxcar-decimated mode, automatic ~3.5 GB rotation, periodic
  `fsync` durability, and a `.log` sidecar carrying provenance (`dev`/`fw`),
  capture settings (range, rate, LPF2, temperature) and integrity counters.
- **Web dashboard** (SoftAP + STA, embedded in the firmware): live acceleration
  graph, recording control, charger/battery status, SD benchmark, Wi-Fi setup,
  and a separate live FFT / spectrogram page with in-browser PNG capture.
- **Recordings as objects** — a music-app-style list (waveform "cover", integrity
  badge, inline metadata and CSV status), a detail sheet decoding the header +
  log, per-file downloads, rename, and whole-recording delete.
- **Recording markers** — timestamped annotations placed during a recording
  (web button or a single POWER-button press, toggle in Settings), with notes,
  discard/delete, a live list, persistence to a `.mark` sidecar, and display in
  the recordings view.
- **CSV export** — on-device (background, with up-front free-space check, atomic
  temp-then-rename, crash-resume via checkpoint, and cancel) and in-browser (Web
  Worker); the on-device converter reports live progress + ETA and auto-extends a
  low-battery countdown to finish a nearly-done conversion.
- **Multi-network Wi-Fi** — remembers up to 8 networks (WPA2/WPA3 personal and
  enterprise), auto-reconnects last-connected-first via a queue-driven task with
  activity-aware backoff; always-on SoftAP; mDNS.
- **Power management** — BQ25628 charger control, a coulomb-counting fuel-gauge
  estimate, and two-stage low-battery protection (soft nag countdown → ship mode,
  hard cutoff → shutdown mode) with boot-grace hysteresis; LED status vocabulary
  and POWER-button gestures (double-click record, 3 s hold ship).
- **SD free-space** shown live in the status bar and the microSD card (throttled
  `esp_vfs_fat_info`, no scan).
- **Device identity** — a per-board `VS-XXXXXX` id (from the factory MAC), the
  firmware version, and a build stamp, in an About card and the boot log.
- **OTA firmware updates** — dual-OTA partition layout, dashboard firmware upload
  streamed into the inactive slot, image verification (SHA-256 + structure),
  automatic rollback on a failed boot, and a build-token check that rejects
  firmware not built from this source.
- **Large-download handling** — file downloads run on a dedicated worker so a big
  transfer never blocks the dashboard, with an adaptive throttle that backs off
  only while an active recording is actually dropping samples.

### Fixed
- Recordings-list file sizes over 2 GB no longer overflow (unsigned 32-bit read,
  64-bit totals).
