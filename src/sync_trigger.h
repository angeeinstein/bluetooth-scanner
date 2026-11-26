#pragma once

#include <stdint.h>

// External hardware sync trigger on PIN_SYNC_TRIG (IO38). A shared rising edge fed
// to this pin on multiple boards latches each board's Wi-Fi TSF at (very nearly)
// the same instant, so the difference between boards' latched TSF is their true
// clock-sync error -- a hardware reference free of the Wi-Fi/HTTP timing jitter
// that limits software-only measurements. Currently a measurement/validation tool
// for the multi-device time-sync work; the same latch is reusable for real wired
// sync later.

typedef struct {
    uint32_t n;      // edge sequence number since the last reset (1-based)
    int64_t tsf_us;  // Wi-Fi TSF (us) at the edge instant, back-computed via esp_timer
} sync_edge_t;

// Configures IO38 as a rising-edge interrupt and starts the latch task. Safe to
// call once at boot after the GPIO ISR service exists (accel_capture installs it).
void sync_trigger_start(void);

// Zeroes the edge counter and clears the log. Do this on every board (with the
// signal idle) right before starting the shared square wave, so edge N is the
// same physical edge on every board.
void sync_trigger_reset(void);

// Total edges seen since the last reset.
uint32_t sync_trigger_get_count(void);

// Copies up to `max` recent logged edges into `out` (unordered; the caller keys
// by `n`). Returns the number written.
int sync_trigger_get_edges(sync_edge_t *out, int max);
