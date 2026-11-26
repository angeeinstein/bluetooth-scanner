// On-disk format for recorded .bin files, shared between recorder.c
// (writer) and csv_export.c (reader).
//
// Layout:
//   [ 64-byte header ][ sync_table_bytes of sync checkpoints ][ raw int16 XYZ triplets ]
// The sample data is raw little-endian int16 X,Y,Z triplets, one per sample, no
// per-sample timestamps (time is index / sample_rate_hz). READERS: sample data
// starts at offset (64 + header.sync_table_bytes), and
//   sample_count = (file_size - 64 - header.sync_table_bytes) / 6.
//
// v2: start time is stored two ways, always both -- boot_time_us (uptime,
// always valid, monotonic) and wall_time_unix_s (real calendar time, valid
// only if wall_time_valid). v1 only stored one or the other depending on a
// flag; a v1 reader interpreting a v2 file's boot_time_us as v1's 8-byte
// start_time would get nonsense, hence the version bump.
//
// v3: adds a fixed-size table of sync checkpoints between the header and the
// sample data (sync_table_bytes > 0). Each checkpoint pairs a sample_index with
// the 802.11 TSF (a microsecond clock shared by all devices on the same AP) at
// that instant AND the BSSID that TSF belongs to, so recordings from multiple
// boards can be time-aligned. Because every checkpoint carries its own BSSID, a
// mid-recording roam to another AP is handled cleanly: align two devices using
// checkpoints whose BSSID matches; a checkpoint with tsf_us == 0 / zero bssid was
// taken while unassociated. The header's `bssid` is just the start AP (summary).
// A v2 file has sync_table_bytes == 0 and no table (data at offset 64).

#pragma once

#include <stdint.h>

#define RECORDING_MAGIC "VREC"

// Sync-checkpoint table: fixed number of slots reserved right after the header.
// 512 * 60 s covers an ~8.5 h span, comfortably past the ~6 h rotation limit.
#define SYNC_TABLE_ENTRIES 512
#define SYNC_CHECKPOINT_INTERVAL_S 60

#pragma pack(push, 1)
typedef struct {
    char magic[4]; // "VREC"
    uint16_t version;
    uint16_t reserved0;
    float sample_rate_hz;
    float fs_range_g;
    float sensitivity_mg_per_lsb;
    uint8_t wall_time_valid; // 1 if wall_time_unix_s is a real synced timestamp (NTP or browser)
    uint8_t reserved1[3];
    int64_t boot_time_us;     // esp_timer_get_time() at recording start -- always valid
    int64_t wall_time_unix_s; // Unix epoch seconds at start; meaningful only if wall_time_valid
    // --- v3 additions (previously reserved2) ---
    uint32_t sync_table_bytes;  // bytes of sync-checkpoint table between header and data (0 = v2)
    uint32_t sync_count;        // number of valid checkpoints in that table
    uint8_t bssid[6];           // AP BSSID at start (all-zero if unknown); shared-clock scope
    uint16_t sync_interval_s;   // nominal checkpoint spacing (informational)
    uint8_t reserved2[8];
} recording_header_t; // 64 bytes

// One sync checkpoint: the shared TSF at the moment `sample_index` samples had
// been written, and the AP that TSF belongs to. sample_index counts data samples
// only (not header/table bytes).
typedef struct {
    uint64_t sample_index;
    int64_t tsf_us;     // 802.11 TSF microseconds; 0 if the STA wasn't associated
    uint8_t bssid[6];   // AP the TSF is from (all-zero if unassociated); shared-clock scope
    uint8_t reserved[2];
} sync_checkpoint_t; // 24 bytes
#pragma pack(pop)

_Static_assert(sizeof(recording_header_t) == 64, "recording header must stay 64 bytes");
_Static_assert(sizeof(sync_checkpoint_t) == 24, "sync checkpoint must stay 24 bytes");
