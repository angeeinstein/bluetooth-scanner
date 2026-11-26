#pragma once

#include <stddef.h>

// Fills `out` (needs >= 16 bytes) with this board's unique identifier, derived
// from the chip's factory-programmed MAC address ("VS-XXXXXX", the last three
// MAC bytes in hex). Stable across reboots, unique per board, needs no storage
// or configuration. Single source of truth for the ID format -- used by the
// dashboard's About card, the boot log, and the recording .log stamp.
void device_id_str(char *out, size_t len);
