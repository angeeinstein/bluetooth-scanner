#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CSV_EXPORT_IDLE,
    CSV_EXPORT_RUNNING,
    CSV_EXPORT_DONE,
    CSV_EXPORT_FAILED,
    CSV_EXPORT_CANCELLED,
} csv_export_state_t;

typedef struct {
    csv_export_state_t state;
    char filename[64]; // source .bin name, no path
    uint64_t total_samples;
    uint64_t samples_done;
    int32_t eta_s; // estimated seconds remaining while running, else -1
    char error[64];
} csv_export_result_t;

// Starts converting /sdcard/rec/<filename> to a sibling .csv file (same
// name, .csv extension) as a low-priority background task -- runs after
// the fact, no real-time constraint, but takes a while for large files
// since CSV is ~10-11x the raw size. Writes to a <name>.csv.tmp staging
// file (with a .csv.prog checkpoint alongside) and renames it to .csv on
// success, so an interruption never leaves a partial file that looks
// finished. Returns false if one is already running, a recording is
// currently active, or the file doesn't exist.
bool csv_export_start(const char *filename);

void csv_export_get_result(csv_export_result_t *out);

// Scans the recording directory once at boot for an interrupted conversion
// (a leftover .csv.tmp from a power loss / low-battery cutoff) and decides
// what to do with it:
//   - source .bin gone, no/invalid checkpoint, or the finished CSV would
//     exceed the ~2GB resume ceiling -> the partial is deleted (a >2GB job
//     must be re-started deliberately).
//   - otherwise, if `battery_low` is false it auto-resumes from the last
//     checkpoint; if true it is left in place and offered for a manual
//     Resume / Discard (see csv_export_get_pending()).
// Call once, after the SD card is mounted AND a valid battery reading exists
// (so `battery_low` is meaningful). No-op if a conversion is already running.
void csv_export_resume_on_boot(bool battery_low);

// If an interrupted conversion is sitting un-resumed (skipped at boot because
// the battery was low), fills `out_name` with its source .bin filename and
// returns true; else false. Thread-safe snapshot for the dashboard.
bool csv_export_get_pending(char *out_name, size_t len);

// Manually resume the pending interrupted conversion for `filename` (the
// source .bin name). Returns false if none is pending for it, or one is
// already running.
bool csv_export_resume(const char *filename);

// Discard the pending interrupted conversion for `filename`: deletes its
// .csv.tmp + .csv.prog without converting ("start fresh"). Returns false if
// nothing is pending for it.
bool csv_export_discard(const char *filename);

// Requests cancellation of the currently-running conversion. The worker
// stops at its next batch (sub-second), deletes its .csv.tmp + .csv.prog, and
// the state becomes CSV_EXPORT_CANCELLED. The source .bin is untouched.
// Returns false if no conversion is running.
bool csv_export_cancel(void);
