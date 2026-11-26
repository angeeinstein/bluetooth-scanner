#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iis3dwb.h"
#include "ring_buffer.h"

typedef struct {
    float x, y, z; // g
} accel_preview_point_t;

typedef enum {
    ACCEL_SELFTEST_IDLE,
    ACCEL_SELFTEST_RUNNING,
    ACCEL_SELFTEST_PASS,
    ACCEL_SELFTEST_FAIL,
} accel_selftest_state_t;

typedef struct {
    accel_selftest_state_t state;
    // Measured per-axis self-test deflection (|output ST-on - output ST-off|)
    // in mg. Datasheet pass range at +-4g is 800..3200 mg per axis.
    float change_mg_x, change_mg_y, change_mg_z;
} accel_selftest_result_t;

// Initializes the SPI bus + IIS3DWB + FIFO + GPIO ISR + drain task. Call
// once at boot. Returns false if the sensor isn't found/doesn't respond
// (WHO_AM_I mismatch or SPI failure).
bool accel_capture_start(void);

bool accel_capture_is_ready(void);

// Suspends/resumes the drain task entirely (the sensor's own FIFO keeps
// running and will overflow/lose samples while paused -- that's expected
// and fine, this is only meant for the standalone SD benchmark, which wants
// an uncontended read of the card's own throughput rather than realistic
// recording-time contention). Never call this while a real recording could
// be in progress.
void accel_capture_pause(void);
void accel_capture_resume(void);

// Changes full-scale range and the second-stage filter (mode + cutoff). The
// caller (recorder.c) is responsible for rejecting this while a recording is in
// progress -- changing sensitivity or filtering mid-file would desync the
// file's header/log.
bool accel_capture_set_config(iis3dwb_fs_t fs, iis3dwb_filter_mode_t filter,
                              iis3dwb_hpcf_t cutoff);
// Like accel_capture_set_config() but a runtime-only change that never persists,
// even when "remember" is on -- for automatic (auto-range) changes that must not
// overwrite the user's manually chosen, remembered scale/filter preference.
bool accel_capture_set_config_transient(iis3dwb_fs_t fs, iis3dwb_filter_mode_t filter,
                                        iis3dwb_hpcf_t cutoff);
iis3dwb_fs_t accel_capture_get_fs(void);
iis3dwb_filter_mode_t accel_capture_get_filter_mode(void);
iis3dwb_hpcf_t accel_capture_get_hpcf(void);
// Convenience: whether the second-stage filter is engaged at all (mode != OFF).
bool accel_capture_get_lpf2(void);

// Whether the full-scale range + LPF2 setting are persisted to NVS and
// restored on the next boot. Off by default (boots at +-2g, regardless of the
// last-used scale). Enabling it snapshots the current config immediately, and
// every later accel_capture_set_config() is then saved too.
bool accel_capture_get_remember_config(void);
void accel_capture_set_remember_config(bool remember);
float accel_capture_get_sensitivity_mg_per_lsb(void);

// Reads the sensor's on-chip die temperature in degrees C. Thread-safe
// (takes the SPI mutex internally). Returns false on SPI failure.
bool accel_capture_read_temp_c(float *out_c);

// Runs the IIS3DWB built-in electrostatic self-test as a short (~0.5s)
// background task: pauses capture, forces +-4g, measures the deflection with
// self-test off vs on, and compares each axis against the datasheet's
// 800..3200 mg pass window, then restores the prior config and resumes.
// The device MUST be stationary (external vibration corrupts the result) and
// the caller must ensure no recording is active. Returns false if the sensor
// isn't ready or a self-test is already running; poll
// accel_capture_self_test_get_result() for progress/outcome.
bool accel_capture_self_test_start(void);
void accel_capture_self_test_get_result(accel_selftest_result_t *out);

// The recording ring buffer (raw little-endian int16 X,Y,Z triplets, 6
// bytes/sample) -- the SD writer task in recorder.c reads from this
// directly via ring_buffer_read().
ring_buffer_t *accel_capture_get_recording_ring(void);

// A second, independent recording-rate ring in the exact same raw int16
// X,Y,Z triplet format, boxcar-decimated to accel_capture_get_recording_decim_rate_hz()
// -- lets recorder.c write a much smaller file when full 26.7kHz resolution
// isn't needed. Fed unconditionally, same as every other ring here (see
// accel_capture_get_ws_stream_ring()'s rationale); recorder.c always drains
// both this and the full-rate ring, writing from whichever one the current
// recording is actually using and discarding the other.
ring_buffer_t *accel_capture_get_recording_decim_ring(void);

// The exact (non-round) sample rate accel_capture_get_recording_decim_ring()
// runs at -- an integer decimation of the fixed 26.7kHz raw rate, so it
// doesn't land on a round number. Store this in the recording header
// rather than a rounded label.
float accel_capture_get_recording_decim_rate_hz(void);

// The full-rate (undecimated) streaming ring buffer -- same raw int16
// X,Y,Z triplet format as the recording ring, but much smaller and fed
// unconditionally (like the recording ring). udp_stream.c reads from this
// directly via ring_buffer_read(), sending or discarding depending on
// whether streaming is enabled -- dropping is expected/fine here, unlike
// the recording ring.
ring_buffer_t *accel_capture_get_stream_ring(void);

// Same idea as accel_capture_get_stream_ring(), but a second, independent
// ring dedicated to ws_stream.c's binary WebSocket live stream. A separate
// ring (not a shared one gated by "whose turn is it") is deliberate: each
// ring has exactly one dedicated consumer task that always drains it,
// preserving the single-producer/single-consumer contract and keeping
// whichever stream is inactive perpetually fresh (rather than accumulating
// stale backlog that would burst out when it's next enabled).
ring_buffer_t *accel_capture_get_ws_stream_ring(void);

// Pops up to `max_points` decimated (~26.7Hz, boxcar-averaged) preview
// points accumulated since the last call into `out`. Returns the number
// actually written. Used by the web dashboard's live graph.
size_t accel_capture_read_preview(accel_preview_point_t *out, size_t max_points);

// Diagnostics: sensor-side FIFO overruns (data lost before we ever read it)
// and our own recording-ring drops (data read from the sensor but the ring
// buffer was full, e.g. because the SD card fell behind).
uint32_t accel_capture_get_sensor_overruns(void);
uint32_t accel_capture_get_ring_drops(void);
uint32_t accel_capture_get_stream_ring_drops(void);
uint32_t accel_capture_get_ws_stream_ring_drops(void);
uint32_t accel_capture_get_recording_decim_ring_drops(void);

// Fills `out` with the per-axis (X,Y,Z) percentage of samples at/near full-scale
// saturation (clipping) over the interval since the previous call. Best-effort
// live metric measured on the raw full-rate samples (the preview's averaging
// hides clipping); call it about once a second -- e.g. the status poll -- for a
// ~1s rolling window.
void accel_capture_get_clip_pct(float out[3]);

// The measured true accelerometer ODR in Hz, derived by the drain task from the
// sample count over the reliable esp_timer clock across overrun-free windows (an
// EMA). 0 until the first clean window is measured (~1s after capture starts).
// This is the real sampling rate -- the nominal "26700" is only a label.
float accel_capture_get_measured_odr_hz(void);

// Cumulative count of samples pulled from the FIFO since boot (the denominator
// for a loss percentage). 64-bit -- won't wrap in any realistic runtime.
uint64_t accel_capture_get_samples_total(void);

// Estimated number of samples lost to FIFO overruns since boot (reset with the
// startup-overrun clear, like accel_capture_get_sensor_overruns()). Derived from
// the measured ODR: the shortfall between expected and captured samples across
// overrun windows. Approximate -- a magnitude to judge whether loss is negligible,
// not an exact count. FIFO overruns only; ring-buffer drops are exact and counted
// separately by accel_capture_get_ring_drops().
uint32_t accel_capture_get_overrun_lost_samples(void);

// Publishes the drain task's latest time reference: the raw sensor timestamp
// counter (ts_ticks), the cumulative sample count that timestamp corresponds to
// (samples), and the measured sensor tick period in microseconds (tick_us, 0
// until measured). Any out-pointer may be NULL. Lets the recorder tag the file
// with a hardware timeline; multiply raw ticks by tick_us for real elapsed time.
void accel_capture_get_time_snapshot(uint32_t *ts_ticks_out, uint64_t *samples_out,
                                     float *tick_us_out);

// Raw cumulative clipping counters: total samples seen, and per-axis samples at
// full-scale saturation. For a caller that needs its own rolling window
// independent of accel_capture_get_clip_pct()'s single-consumer delta -- e.g.
// the recorder's auto-range monitor. Free-running and monotonic (unsigned
// wraparound is fine for deltas); aligned 32-bit reads are atomic, so no lock.
void accel_capture_get_clip_counts(uint32_t *total_out, uint32_t out[3]);
