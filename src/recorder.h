#pragma once

#include <stdbool.h>
#include <stdint.h>

// Mounts the SD card persistently (unlike the old self-test's mount-per-call
// pattern), ensures /sdcard/rec exists, and starts a background task that
// watches the card-detect pin for hot-swap (see recorder_remount()). Call
// once at boot. Returns false if no card is present yet -- that's fine, the
// CD monitor (or a manual recorder_remount()) will pick it up once one is
// inserted, no reboot needed.
bool recorder_init(void);

bool recorder_is_sd_mounted(void);

// Fills the mounted card's total and free capacity in bytes. Returns false if
// no card is mounted (or the query fails). Cheap -- a FAT metadata read, not a
// scan -- and internally throttled so repeated calls (e.g. the ~1Hz status
// poll) never hit the card more than every few seconds.
bool recorder_get_sd_space(uint64_t *total_bytes, uint64_t *free_bytes);

// Safely unmounts (stopping/flushing any active recording first) and
// re-attempts a mount. Called automatically on a debounced card-detect
// change; also exposed for a manual "Remount SD" action, since CD polarity
// isn't assumed/verified for this socket. Safe to call with no card
// present (just leaves the SD unmounted).
bool recorder_remount(void);

// Starts the always-running SD writer task. It continuously drains the
// accelerometer recording ring buffer -- to a file while recording, or by
// discarding while idle -- so the ring never backs up and stalls capture
// even when nothing is being saved. Call once, after recorder_init() and
// accel_capture_start().
bool recorder_start_writer_task(void);

// Requests a new recording, with an optional user-supplied name tag
// (sanitized into the filename; pass NULL or "" for none). `decimated`
// selects the ~988.9Hz boxcar-decimated recording ring instead of the
// full 26.7kHz one -- see accel_capture_get_recording_decim_rate_hz().
// Returns false if a recording is already in progress. Asynchronous -- the
// writer task opens the file on its next loop iteration.
bool recorder_request_start(const char *name_tag, bool decimated);

// Requests the current recording to stop. Safe to call even if none is
// active (no-op). Asynchronous -- the writer task flushes and closes the
// file on its own.
void recorder_request_stop(void);

// Blocks up to timeout_ms until any in-progress stop has been fully
// serviced (file flushed and closed). Returns true immediately if nothing
// was recording. Used by the power-button long-press handler to guarantee
// the file is safely closed before ship mode cuts power.
bool recorder_wait_for_stop(uint32_t timeout_ms);

bool recorder_is_recording(void);
const char *recorder_get_current_filename(void); // "" if not recording
uint64_t recorder_get_bytes_written(void);        // for the current/most recent file
float recorder_get_current_sample_rate_hz(void);  // for the current/most recent file

// --- Markers within a recording ---
// A time-stamped annotation placed during an active recording (e.g. "motor
// started"). The position is captured when the mark is added and is never moved
// by editing the note afterwards. Persisted to a <name>.mark sidecar so it
// survives a power loss and shows up for the finished recording.
#define RECORDER_MARKER_NOTE_LEN 48

typedef struct {
    uint32_t id;
    double elapsed_s;    // seconds since the recording started (its position in the data)
    int64_t wall_unix_s; // absolute time if the clock was synced, else 0
    char note[RECORDER_MARKER_NOTE_LEN];
} recorder_marker_t;

// Adds a marker at `at_us` (an esp_timer_get_time() value -- e.g. captured at a
// button press for accuracy) or "now" when `at_us` is 0. Returns the new
// marker's id, or -1 if not recording / the per-recording limit is reached.
int recorder_marker_add(int64_t at_us);

// Sets (replaces) a marker's note; the note is sanitized and length-capped.
// Returns false if no marker with that id exists.
bool recorder_marker_set_note(uint32_t id, const char *note);

// Removes a marker by id (serves both "discard" and "delete"). Returns false
// if no marker with that id exists.
bool recorder_marker_delete(uint32_t id);

// Copies up to `max` of the current recording's markers into `out` (oldest
// first). Returns the number written. `recorder_marker_count()` is a cheap
// size query for status polling.
int recorder_marker_list(recorder_marker_t *out, int max);
int recorder_marker_count(void);

// "" if the last start/stop cycle had no error. Set when: a start is
// rejected (already recording / SD not mounted), a start's file-open fails
// asynchronously (SD present but the actual open still failed), or an
// active recording hits a fatal write error (e.g. the card fills up
// mid-recording) and gets stopped automatically as a result. Cleared the
// moment a new start is accepted.
const char *recorder_get_last_error(void);

// esp_timer timestamp (us) of when the last error was set; advances on every
// (re)set, so a repeat of the same error still counts as a fresh occurrence.
int64_t recorder_last_error_at_us(void);

// A benign, informational stop message ("" if none) -- e.g. the SD card filled
// up, which ends the recording gracefully rather than as an error. Cleared when
// a new recording starts. Shown neutrally on the dashboard, not as an error.
const char *recorder_get_last_notice(void);

// Auto-range on clipping: when enabled, a recording that saturates full scale
// for a sustained period is automatically continued in a new rotation part at a
// larger measurement range (headless, no dashboard needed) -- so a long
// unattended recording isn't ruined by clipping. Escalates up only, caps at
// +-16g. Off by default; persisted to NVS.
bool recorder_get_auto_range(void);
void recorder_set_auto_range(bool enable);
