#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Source-agnostic OTA write core. Today it's driven by the dashboard's
// firmware-upload handler (bytes streamed from an HTTP POST); a future
// pull-from-URL path would feed the exact same begin/write/end sequence, so the
// download source can change without touching this core.

typedef enum {
    OTA_IDLE,
    OTA_WRITING,
    OTA_SUCCESS,
    OTA_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    uint32_t bytes_written;
    uint32_t total_bytes; // 0 if the size wasn't known up front
    bool available;       // running on a dual-OTA partition layout (updates possible)
    char running_partition[16];
    char error[64];
} ota_status_t;

// Starts writing a new image into the inactive OTA slot. `total_bytes` may be 0
// if unknown. Returns false (with a reason in the status) if an update can't
// start: a recording is active, there's no OTA partition (old layout), or an
// update is already in progress.
bool ota_begin(uint32_t total_bytes);

// Appends a chunk to the update image. Returns false on error (the session is
// aborted and left in OTA_FAILED with a reason).
bool ota_write(const void *data, size_t len);

// Validates the completed image (magic/size/SHA-256) and sets it as the boot
// partition. Returns true on success -- the caller should then reboot.
bool ota_end(void);

// Aborts an in-progress session and frees its handle.
void ota_abort(void);

void ota_get_status(ota_status_t *out);

// Marks the currently-running image valid, cancelling the pending rollback.
// Call once at boot after the core subsystems are up (a bad update that crashes
// before this reverts to the previous firmware).
void ota_mark_valid(void);

// Reboots after a short delay -- long enough for the HTTP success response to
// flush to the client first.
void ota_schedule_reboot(void);
