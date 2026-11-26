#pragma once

// BLE scanner: continuous NimBLE extended scan (legacy + BLE 5 extended
// advertising), every report delivered (no duplicate filter, scan window ==
// interval, no time limit, restarted automatically if it ever stops).
//
// Data path: the NimBLE GAP callback only copies each report into a PSRAM
// queue (never blocks the host task -- a full queue counts a drop). A
// processing task drains it, decodes the AD structures, keeps a per-address
// device table (name cache, RSSI stats, manufacturer, ...) and hands every
// report + its decode + the device entry to an optional hook (the scan logger).

#include <stdbool.h>
#include <stdint.h>

#define BLE_ADDR_LEN 6
#define BLE_NAME_MAX 32       // cached device name incl. NUL (longer names are cut)
#define BLE_REPORT_DATA_MAX 255 // one HCI report fragment carries at most 255 data bytes

// NimBLE ble_addr_t.type values.
#define BLE_ADDRTYPE_PUBLIC 0
#define BLE_ADDRTYPE_RANDOM 1
#define BLE_ADDRTYPE_PUBLIC_ID 2 // resolved identity (needs bonding; won't occur here)
#define BLE_ADDRTYPE_RANDOM_ID 3

// Report `props` bits (same values as NimBLE's BLE_HCI_ADV_*_MASK).
#define BLE_PROP_CONNECTABLE 0x01
#define BLE_PROP_SCANNABLE 0x02
#define BLE_PROP_DIRECTED 0x04
#define BLE_PROP_SCAN_RSP 0x08
#define BLE_PROP_LEGACY 0x10

// One received advertising report (or scan response), exactly as the
// controller delivered it. Extended-advertising payloads > 255 bytes arrive
// as several reports; `data_status` says which fragment this is.
typedef struct {
    int64_t t_us;       // esp_timer_get_time() when the host received it
    int64_t wall_us;    // UTC microseconds since 1970, or 0 if the clock isn't set
    uint8_t addr[BLE_ADDR_LEN]; // big-endian (as printed: addr[0] is the MSB)
    uint8_t addr_type;  // BLE_ADDRTYPE_*
    int8_t rssi;        // dBm (127 = unavailable)
    int8_t tx_power;    // dBm from the extended header (127 = unavailable)
    uint8_t props;      // BLE_PROP_* bits
    uint8_t legacy_evt; // legacy PDU type (0 ADV_IND .. 4 SCAN_RSP); valid if BLE_PROP_LEGACY
    uint8_t data_status; // 0 complete, 1 incomplete (more follows), 2 truncated
    uint8_t sid;        // advertising set id (extended only; 0xFF = none)
    uint8_t prim_phy;   // 1 = 1M, 3 = Coded
    uint8_t sec_phy;    // 0 = none (legacy), 1 = 1M, 2 = 2M, 3 = Coded
    uint16_t periodic_itvl; // periodic advertising interval (1.25 ms units), 0 = none
    uint8_t direct_addr[BLE_ADDR_LEN]; // target address of directed advertising
    uint8_t direct_addr_type;
    uint8_t data_len;
    uint8_t data[BLE_REPORT_DATA_MAX];
} ble_report_t;

// Decoded AD structures of one report. Pointers point into the report's data.
#define BLE_MAX_UUIDS 8
typedef struct {
    char name[BLE_NAME_MAX];
    uint8_t name_kind; // 0 none, 1 shortened local name, 2 complete local name
    bool has_flags;
    uint8_t flags;
    bool has_tx_power;
    int8_t tx_power; // AD type 0x0A (distinct from the extended-header TX power)
    bool has_appearance;
    uint16_t appearance;
    bool has_mfg;
    uint16_t company_id; // first manufacturer-specific entry
    const uint8_t *mfg_data; // bytes after the company id
    uint8_t mfg_len;
    uint8_t n_uuids; // service UUIDs (16/32/128-bit, complete + incomplete lists)
    struct {
        uint8_t size; // 2, 4 or 16 bytes
        const uint8_t *p; // little-endian, as on air
    } uuids[BLE_MAX_UUIDS];
    bool has_svc_data; // first service-data entry (any UUID size)
    uint8_t svc_uuid_size;
    const uint8_t *svc_data; // starts with the UUID (little-endian), then the data
    uint8_t svc_data_len; // incl. the UUID bytes
} ble_adv_parsed_t;

#define BLE_HIST_SECONDS 120 // per-device RSSI history (1 s slots)
#define BLE_GHIST_SECONDS 600 // global packets/s + active-device history
#define BLE_PAYLOAD_KEEP 64   // last adv / scan-response payload kept per device (full data is in the log)

// One entry of the per-address device table.
typedef struct {
    uint8_t addr[BLE_ADDR_LEN];
    uint8_t addr_type;
    char name[BLE_NAME_MAX]; // cached: kept when later packets carry no name
    uint8_t name_kind;
    int64_t first_us, last_us; // esp_timer time
    uint32_t count;            // reports received (adv + scan responses)
    uint32_t rate;             // reports in the last full second
    int8_t rssi_last, rssi_min, rssi_max;
    float rssi_avg; // exponential moving average (alpha 0.2)
    int8_t tx_power; // last known (127 = unknown)
    uint16_t company_id; // 0xFFFF = none seen
    uint16_t appearance; // 0 = none seen
    uint16_t uuid16;     // first 16-bit service UUID seen, 0 = none
    uint8_t props_seen;  // OR of BLE_PROP_* over all reports
    uint8_t phys_seen;   // bit0 1M, bit1 2M, bit2 Coded (primary + secondary)
    bool target;         // matched a target rule (see ble_scanner_set_target_fn)
    bool target_event;   // (report-hook copy only) this report should emit a TARGET event:
                         // first match, or matched again after >30 s unseen, or re-armed
    uint8_t adv_len, rsp_len;
    uint8_t adv_data[BLE_PAYLOAD_KEEP]; // last advertising payload (truncated to 64 B)
    uint8_t rsp_data[BLE_PAYLOAD_KEEP]; // last scan-response payload
    // RSSI history: slot (sec % BLE_HIST_SECONDS) holds that second's mean RSSI
    // (127 = not heard) and packet count; hist_sec is the last finished second.
    int8_t hist_rssi[BLE_HIST_SECONDS];
    uint8_t hist_cnt[BLE_HIST_SECONDS];
    // internal
    uint8_t used;
    uint32_t count_this_s;
    int32_t rssi_sum_this_s;
    uint32_t rssi_n_this_s;
} ble_device_t;

typedef struct {
    bool ok;        // stack initialised and synced with the controller
    bool scanning;  // a discovery procedure is running right now
    bool active;    // active scanning (sends scan requests)
    bool coded;     // also scanning the Coded PHY (long range)
    uint32_t reports_per_s;  // last full second
    uint64_t reports_total;
    uint32_t queue_drops;    // reports lost because the processing queue was full
    uint32_t restarts;       // times scanning had to be restarted
    uint32_t devices_known;  // entries in the device table
    uint32_t devices_recent; // seen in the last 60 s
    uint32_t table_purges;   // times old entries were dropped to make room
    uint32_t target_events;  // TARGET events since boot
    int last_err;            // last NimBLE error code from starting a scan (0 = none)
} ble_scanner_status_t;

// Initialises NimBLE + controller and starts scanning once synced. Returns
// false (and leaves BLE off) if the stack can't start -- never aborts.
bool ble_scanner_start(void);

void ble_scanner_get_status(ble_scanner_status_t *out);

// Scan settings, persisted in NVS; applied immediately (scan restarts).
bool ble_scanner_get_active(void);
void ble_scanner_set_active(bool active);
bool ble_scanner_get_coded(void);
void ble_scanner_set_coded(bool coded);

// Copies devices seen within the last `max_age_ms` (0 = all) into `out`, up to
// `max` entries; returns how many were copied. `now_us` (optional) receives
// the esp_timer time of the snapshot, to turn last_us into an age. Only the
// summary fields (addr .. target_event) are filled -- not the payloads or the
// history, which would make a full list ~1 MB; use ble_scanner_get_device().
int ble_scanner_snapshot(ble_device_t *out, int max, uint32_t max_age_ms, int64_t *now_us);

// Called on the processing task for every report, after the device table is
// updated. Must be quick (it paces the whole pipeline). `dev` is a copy.
typedef void (*ble_report_hook_t)(const ble_report_t *r, const ble_adv_parsed_t *p,
                                  const ble_device_t *dev);
void ble_scanner_set_report_hook(ble_report_hook_t hook);

// Copies one device (by address + type) into `out`; false if unknown. `sec`
// (optional) receives the current history second, so out->hist_* can be
// unrolled oldest-to-newest: slot (sec + 1 + i) % BLE_HIST_SECONDS, i = 0..N-1.
bool ble_scanner_get_device(const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type, ble_device_t *out,
                            uint32_t *sec);

// Global history, oldest first: reports per second and devices heard per
// second, for the last BLE_GHIST_SECONDS seconds (fewer right after boot).
// Returns the number of seconds filled in.
int ble_scanner_get_history(uint16_t *rps, uint16_t *devices, int max);

// Makes every currently matched device emit a fresh TARGET event on its next
// report (called when a logging session starts, so each log lists them).
void ble_scanner_rearm_targets(void);

// Re-evaluates the target predicate for every known device (after the rules change).
void ble_scanner_reeval_targets(void);

// Optional target predicate, evaluated when a report updates a device.
typedef bool (*ble_target_fn_t)(const ble_device_t *dev);
void ble_scanner_set_target_fn(ble_target_fn_t fn);

// Decodes the AD structures of `data` (exposed for the logger / tests).
void ble_adv_parse(const uint8_t *data, uint8_t len, ble_adv_parsed_t *out);

// "AA:BB:CC:DD:EE:FF" (needs 18 bytes).
void ble_addr_str(const uint8_t addr[BLE_ADDR_LEN], char *out);

// Short address-kind label: "public", "random-static", "rpa" (resolvable
// private), "nrpa" (non-resolvable private), "random" (reserved bits).
const char *ble_addr_kind(uint8_t addr_type, const uint8_t addr[BLE_ADDR_LEN]);
