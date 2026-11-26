#pragma once

// SD card + logging sessions. Owns the card (mount, card-detect hot-swap,
// remount, free space) and one logging session at a time: a CSV file in
// /sdcard/ble (never VibraShock's /sdcard/rec, so a card can move between the
// two firmwares), plus a <base>.log (start/stop events) and a <base>.mark
// (current marker list) sidecar. What goes into the CSV is scan_log.c's job:
// producers append complete text lines with recorder_log_line(), a PSRAM line
// buffer absorbs SD stalls, and the SD writer task streams it to the card.
//
// "Recording" in function names = a logging session (the name is kept from
// VibraShock so the button, LED and OTA code didn't have to change).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RECORDER_DIR "/sdcard/ble"

// Mounts the SD card persistently, ensures RECORDER_DIR exists (and its
// FORMAT.md is current), and starts a background task that watches the
// card-detect pin for hot-swap. Call once at boot. Returns false if no card is
// present yet -- fine, the CD monitor or recorder_remount() picks it up later.
bool recorder_init(void);

bool recorder_is_sd_mounted(void);

// Total and free capacity in bytes. Throttled internally (FAT metadata read at
// most every few seconds). Returns false if no card is mounted.
bool recorder_get_sd_space(uint64_t *total_bytes, uint64_t *free_bytes);

// Stops any session (flushing it), unmounts and re-attempts a mount. Called on
// a debounced card-detect change and by the dashboard's "Remount SD".
bool recorder_remount(void);

// Starts the SD writer task. Call once, after recorder_init().
bool recorder_start_writer_task(void);

// Requests a new session with an optional name tag (sanitized into the file
// name; NULL/"" = none). Returns false if one is already running or no card is
// mounted. Asynchronous -- the writer task opens the file.
bool recorder_request_start(const char *name_tag);

// Requests the current session to stop (no-op if none). Asynchronous: the
// writer writes the final rows, flushes and closes.
void recorder_request_stop(void);

// Blocks up to timeout_ms until a pending stop has completed. True right away
// if nothing is running. Used before ship mode cuts power.
bool recorder_wait_for_stop(uint32_t timeout_ms);

bool recorder_is_recording(void);
const char *recorder_get_current_filename(void); // full path of the current part, "" if idle
uint64_t recorder_get_bytes_written(void);       // whole session (all parts), current or last
uint64_t recorder_get_rows_written(void);        // lines written, current or last session
double recorder_get_elapsed_s(void);             // session duration so far (or of the last one)
float recorder_get_bytes_per_s(void);            // write rate over the last ~10 s
uint32_t recorder_get_log_drops(void);           // lines lost because the line buffer was full

// Appends one complete line (must end with '\n') to the session. Thread-safe,
// never blocks for long. Returns false (and counts a drop if a session is
// running) when no session accepts lines or the buffer is full.
bool recorder_log_line(const char *line, size_t len);

// --- Markers within a session ---
// Time-stamped annotations (POWER single click or the dashboard). Each add,
// note change and delete is also written into the CSV (see scan_log.c), and
// the current list is mirrored to <base>.mark.
#define RECORDER_MARKER_NOTE_LEN 48

typedef struct {
    uint32_t id;
    double elapsed_s;    // seconds since the session started
    int64_t at_us;       // esp_timer time of the mark
    int64_t wall_unix_s; // absolute time if the clock was set, else 0
    char note[RECORDER_MARKER_NOTE_LEN];
} recorder_marker_t;

// Adds a marker at `at_us` (esp_timer time, e.g. captured at a button press)
// or now when 0. Returns the new id, or -1 if no session / limit reached.
int recorder_marker_add(int64_t at_us);
bool recorder_marker_set_note(uint32_t id, const char *note);
bool recorder_marker_delete(uint32_t id);
int recorder_marker_list(recorder_marker_t *out, int max);
int recorder_marker_count(void);

// "" if the last start/stop cycle had no error (start rejected, file open
// failed, or a write error ended the session).
const char *recorder_get_last_error(void);
int64_t recorder_last_error_at_us(void); // advances on every (re)set
// Benign stop message ("" if none), e.g. the card filled up.
const char *recorder_get_last_notice(void);
