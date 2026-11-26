#include "ble_scanner.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

#define BLE_NVS_NS "ble"

// Scan interval == window (100 % duty on each enabled PHY). 0.625 ms units:
// 80 = 50 ms, so the controller hops 37/38/39 every 50 ms.
#define SCAN_ITVL 80

#define REPORT_QUEUE_LEN 512   // ~145 KB in PSRAM: absorbs bursts while the logger writes
#define DEV_TABLE_SIZE 4096    // open-addressing hash table (power of two), in PSRAM
#define DEV_TABLE_HIGH 3072    // purge old entries when this full (keeps probing short)
#define DEV_KEEP_MAX 2048      // ... down to at most this many, newest first
#define DEV_RECENT_US (60LL * 1000000) // "recent" for the status counter

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_tab_mux = NULL;  // device table
static SemaphoreHandle_t s_scan_mux = NULL; // start/stop scanning
static ble_device_t *s_tab = NULL;          // DEV_TABLE_SIZE entries
static ble_device_t *s_tab_tmp = NULL;      // purge scratch (same size)
static uint32_t s_tab_count = 0;

static volatile bool s_synced = false;
static volatile bool s_scanning = false;
static uint8_t s_own_addr_type = 0;
static bool s_active = true;
static bool s_coded = false;

static ble_report_hook_t s_hook = NULL;
static ble_target_fn_t s_target_fn = NULL;

// Counters (written by one task each; read unlocked -- word-sized, fine for status).
static volatile uint32_t s_queue_drops = 0;
static volatile uint32_t s_restarts = 0;
static volatile int s_last_err = 0;
static uint64_t s_reports_total = 0;
static uint32_t s_reports_this_s = 0;
static uint32_t s_reports_per_s = 0;
static uint32_t s_devices_recent = 0;
static uint32_t s_table_purges = 0;

// --- Address helpers ----------------------------------------------------

void ble_addr_str(const uint8_t addr[BLE_ADDR_LEN], char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1], addr[2], addr[3],
             addr[4], addr[5]);
}

const char *ble_addr_kind(uint8_t addr_type, const uint8_t addr[BLE_ADDR_LEN]) {
    if (addr_type == BLE_ADDRTYPE_PUBLIC || addr_type == BLE_ADDRTYPE_PUBLIC_ID) {
        return "public";
    }
    switch (addr[0] >> 6) { // the two most significant bits of a random address
    case 3:
        return "random-static";
    case 1:
        return "rpa";
    case 0:
        return "nrpa";
    default:
        return "random";
    }
}

// --- AD structure decoding ----------------------------------------------

void ble_adv_parse(const uint8_t *data, uint8_t len, ble_adv_parsed_t *out) {
    memset(out, 0, sizeof(*out));
    uint8_t i = 0;
    while (i < len) {
        uint8_t flen = data[i];
        if (flen == 0 || i + 1 + flen > len) {
            break; // zero padding or a malformed length -- stop, keep what we have
        }
        uint8_t type = data[i + 1];
        const uint8_t *v = &data[i + 2];
        uint8_t vlen = flen - 1;
        switch (type) {
        case 0x01: // flags
            if (vlen >= 1) {
                out->has_flags = true;
                out->flags = v[0];
            }
            break;
        case 0x02: case 0x03: // 16-bit service UUIDs (incomplete / complete)
        case 0x04: case 0x05: // 32-bit
        case 0x06: case 0x07: { // 128-bit
            uint8_t sz = (type <= 0x03) ? 2 : (type <= 0x05) ? 4 : 16;
            for (uint8_t k = 0; k + sz <= vlen && out->n_uuids < BLE_MAX_UUIDS; k += sz) {
                out->uuids[out->n_uuids].size = sz;
                out->uuids[out->n_uuids].p = v + k;
                out->n_uuids++;
            }
            break;
        }
        case 0x08: // shortened local name
        case 0x09: // complete local name
            if (type == 0x09 || out->name_kind == 0) {
                uint8_t n = vlen < BLE_NAME_MAX - 1 ? vlen : BLE_NAME_MAX - 1;
                memcpy(out->name, v, n);
                out->name[n] = '\0';
                out->name_kind = (type == 0x09) ? 2 : 1;
            }
            break;
        case 0x0A: // TX power level
            if (vlen >= 1) {
                out->has_tx_power = true;
                out->tx_power = (int8_t)v[0];
            }
            break;
        case 0x16: // service data, 16-bit UUID
        case 0x20: // 32-bit
        case 0x21: { // 128-bit
            uint8_t sz = (type == 0x16) ? 2 : (type == 0x20) ? 4 : 16;
            if (!out->has_svc_data && vlen >= sz) {
                out->has_svc_data = true;
                out->svc_uuid_size = sz;
                out->svc_data = v;
                out->svc_data_len = vlen;
            }
            break;
        }
        case 0x19: // appearance
            if (vlen >= 2) {
                out->has_appearance = true;
                out->appearance = (uint16_t)(v[0] | (v[1] << 8));
            }
            break;
        case 0xFF: // manufacturer specific
            if (!out->has_mfg && vlen >= 2) {
                out->has_mfg = true;
                out->company_id = (uint16_t)(v[0] | (v[1] << 8));
                out->mfg_data = v + 2;
                out->mfg_len = vlen - 2;
            }
            break;
        default:
            break;
        }
        i += 1 + flen;
    }
}

// --- Device table ---------------------------------------------------------

static uint32_t addr_hash(const uint8_t *a, uint8_t type) {
    uint32_t h = 2166136261u; // FNV-1a
    for (int i = 0; i < BLE_ADDR_LEN; i++) {
        h = (h ^ a[i]) * 16777619u;
    }
    return (h ^ type) * 16777619u;
}

static ble_device_t *tab_slot(ble_device_t *tab, const uint8_t *a, uint8_t type) {
    uint32_t i = addr_hash(a, type) & (DEV_TABLE_SIZE - 1);
    for (;;) { // the table is never full (purged at DEV_TABLE_HIGH), so this terminates
        ble_device_t *d = &tab[i];
        if (!d->used || (d->addr_type == type && memcmp(d->addr, a, BLE_ADDR_LEN) == 0)) {
            return d;
        }
        i = (i + 1) & (DEV_TABLE_SIZE - 1);
    }
}

// Drops the oldest entries so the table stays fast. Random addresses rotate
// every few minutes, so in a busy place the number of distinct addresses grows
// without bound -- only the newest DEV_KEEP_MAX survive a purge.
static void tab_purge(void) {
    // Find the last_us cut-off that keeps at most DEV_KEEP_MAX entries, by
    // bisecting on time (cheap: ~40 passes over 4096 entries, rare).
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (int i = 0; i < DEV_TABLE_SIZE; i++) {
        if (s_tab[i].used) {
            if (s_tab[i].last_us < lo) lo = s_tab[i].last_us;
            if (s_tab[i].last_us > hi) hi = s_tab[i].last_us;
        }
    }
    int64_t cut = lo;
    for (int iter = 0; iter < 40 && lo < hi; iter++) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t n = 0;
        for (int i = 0; i < DEV_TABLE_SIZE; i++) {
            n += (s_tab[i].used && s_tab[i].last_us > mid);
        }
        if (n > DEV_KEEP_MAX) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
        cut = hi;
    }
    memset(s_tab_tmp, 0, sizeof(ble_device_t) * DEV_TABLE_SIZE);
    uint32_t kept = 0;
    for (int i = 0; i < DEV_TABLE_SIZE; i++) {
        if (s_tab[i].used && s_tab[i].last_us > cut) {
            *tab_slot(s_tab_tmp, s_tab[i].addr, s_tab[i].addr_type) = s_tab[i];
            kept++;
        }
    }
    ble_device_t *t = s_tab;
    s_tab = s_tab_tmp;
    s_tab_tmp = t;
    s_tab_count = kept;
    s_table_purges++;
}

// Updates (or creates) the entry for report `r`; copies the result to `copy`.
static void tab_update(const ble_report_t *r, const ble_adv_parsed_t *p, ble_device_t *copy) {
    xSemaphoreTake(s_tab_mux, portMAX_DELAY);
    if (s_tab_count >= DEV_TABLE_HIGH) {
        tab_purge();
    }
    ble_device_t *d = tab_slot(s_tab, r->addr, r->addr_type);
    if (!d->used) {
        memset(d, 0, sizeof(*d));
        d->used = 1;
        memcpy(d->addr, r->addr, BLE_ADDR_LEN);
        d->addr_type = r->addr_type;
        d->first_us = r->t_us;
        d->tx_power = 127;
        d->company_id = 0xFFFF;
        d->rssi_min = 127;
        d->rssi_max = -128;
        s_tab_count++;
    }
    d->last_us = r->t_us;
    d->count++;
    d->count_this_s++;
    if (r->rssi != 127) {
        d->rssi_last = r->rssi;
        if (r->rssi < d->rssi_min) d->rssi_min = r->rssi;
        if (r->rssi > d->rssi_max) d->rssi_max = r->rssi;
        d->rssi_avg = (d->count == 1 || d->rssi_avg == 0.0f) ? (float)r->rssi
                                                             : d->rssi_avg * 0.8f + (float)r->rssi * 0.2f;
    }
    // A complete name always wins; a shortened one only fills an empty cache.
    if (p->name_kind == 2 || (p->name_kind == 1 && d->name_kind == 0)) {
        strncpy(d->name, p->name, BLE_NAME_MAX - 1);
        d->name[BLE_NAME_MAX - 1] = '\0';
        d->name_kind = p->name_kind;
    }
    if (r->tx_power != 127) {
        d->tx_power = r->tx_power;
    } else if (p->has_tx_power) {
        d->tx_power = p->tx_power;
    }
    if (p->has_mfg) d->company_id = p->company_id;
    if (p->has_appearance) d->appearance = p->appearance;
    if (d->uuid16 == 0) {
        for (int i = 0; i < p->n_uuids; i++) {
            if (p->uuids[i].size == 2) {
                d->uuid16 = (uint16_t)(p->uuids[i].p[0] | (p->uuids[i].p[1] << 8));
                break;
            }
        }
    }
    d->props_seen |= r->props;
    if (r->prim_phy == 1 || r->sec_phy == 1) d->phys_seen |= 1;
    if (r->sec_phy == 2) d->phys_seen |= 2;
    if (r->prim_phy == 3 || r->sec_phy == 3) d->phys_seen |= 4;
    if (s_target_fn != NULL) {
        d->target = s_target_fn(d);
    }
    *copy = *d;
    xSemaphoreGive(s_tab_mux);
}

// Once per second: per-device rates and the "recent" counter.
static void tab_tick(int64_t now_us) {
    xSemaphoreTake(s_tab_mux, portMAX_DELAY);
    uint32_t recent = 0;
    for (int i = 0; i < DEV_TABLE_SIZE; i++) {
        ble_device_t *d = &s_tab[i];
        if (!d->used) continue;
        d->rate = d->count_this_s;
        d->count_this_s = 0;
        recent += (now_us - d->last_us) <= DEV_RECENT_US;
    }
    s_devices_recent = recent;
    xSemaphoreGive(s_tab_mux);
}

int ble_scanner_snapshot(ble_device_t *out, int max, uint32_t max_age_ms, int64_t *now_us) {
    if (s_tab == NULL) {
        return 0;
    }
    int64_t now = esp_timer_get_time();
    if (now_us) *now_us = now;
    int n = 0;
    xSemaphoreTake(s_tab_mux, portMAX_DELAY);
    for (int i = 0; i < DEV_TABLE_SIZE && n < max; i++) {
        const ble_device_t *d = &s_tab[i];
        if (d->used && (max_age_ms == 0 || now - d->last_us <= (int64_t)max_age_ms * 1000)) {
            out[n++] = *d;
        }
    }
    xSemaphoreGive(s_tab_mux);
    return n;
}

// --- Scanning --------------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg);

// Starts (or restarts) the discovery procedure with the current settings.
static void scan_start(void) {
    if (!s_synced) {
        return;
    }
    xSemaphoreTake(s_scan_mux, portMAX_DELAY);
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    struct ble_gap_ext_disc_params uncoded = {.itvl = SCAN_ITVL, .window = SCAN_ITVL, .passive = !s_active};
    struct ble_gap_ext_disc_params coded = uncoded;
    // duration 0 = forever, period 0 = continuous, no duplicate filter, accept all.
    int rc = ble_gap_ext_disc(s_own_addr_type, 0, 0, 0, BLE_HCI_SCAN_FILT_NO_WL, 0, &uncoded,
                              s_coded ? &coded : NULL, gap_event, NULL);
    s_last_err = rc;
    s_scanning = (rc == 0);
    if (rc != 0) {
        printf("ble_scanner: starting scan failed (rc=%d)\n", rc);
    }
    xSemaphoreGive(s_scan_mux);
}

static void addr_be(const ble_addr_t *a, uint8_t *out) {
    for (int i = 0; i < BLE_ADDR_LEN; i++) {
        out[i] = a->val[BLE_ADDR_LEN - 1 - i]; // NimBLE stores LSB first
    }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_EXT_DISC: {
        const struct ble_gap_ext_disc_desc *e = &event->ext_disc;
        // Built on the stack and copied into the queue: the host task must never
        // wait here, or the controller starts discarding reports.
        ble_report_t r;
        r.t_us = esp_timer_get_time();
        struct timeval tv;
        gettimeofday(&tv, NULL);
        r.wall_us = tv.tv_sec > 1600000000 ? (int64_t)tv.tv_sec * 1000000 + tv.tv_usec : 0;
        addr_be(&e->addr, r.addr);
        r.addr_type = e->addr.type;
        r.rssi = e->rssi;
        r.tx_power = e->tx_power;
        r.props = e->props;
        r.legacy_evt = e->legacy_event_type;
        r.data_status = e->data_status;
        r.sid = e->sid;
        r.prim_phy = e->prim_phy;
        r.sec_phy = e->sec_phy;
        r.periodic_itvl = e->periodic_adv_itvl;
        addr_be(&e->direct_addr, r.direct_addr);
        r.direct_addr_type = e->direct_addr.type;
        r.data_len = e->length_data;
        if (e->length_data > 0 && e->data != NULL) {
            memcpy(r.data, e->data, e->length_data);
        }
        if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
            s_queue_drops++;
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        // Shouldn't happen with duration 0, but the stack may end discovery
        // (e.g. after a controller reset) -- the processing task restarts it.
        s_scanning = false;
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        printf("ble_scanner: no usable own address (rc=%d)\n", rc);
        s_last_err = rc;
        return;
    }
    s_synced = true;
    scan_start();
    printf("ble_scanner: scanning (%s%s)\n", s_active ? "active" : "passive",
           s_coded ? ", 1M + Coded PHY" : ", 1M PHY");
}

static void on_reset(int reason) {
    printf("ble_scanner: host reset (reason %d)\n", reason);
    s_synced = false;
    s_scanning = false;
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run(); // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// Drains the report queue: decode, device table, hook. Also the 1 Hz
// housekeeping and the scan watchdog.
static void proc_task(void *param) {
    (void)param;
    static ble_report_t r; // static: ~290 B, keep it off the task stack
    ble_adv_parsed_t p;
    ble_device_t dev;
    int64_t next_tick = esp_timer_get_time() + 1000000;
    for (;;) {
        if (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
            ble_adv_parse(r.data, r.data_len, &p);
            tab_update(&r, &p, &dev);
            s_reports_total++;
            s_reports_this_s++;
            if (s_hook != NULL) {
                s_hook(&r, &p, &dev);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_tick) {
            next_tick += 1000000;
            if (now >= next_tick) next_tick = now + 1000000; // fell behind -- resync
            s_reports_per_s = s_reports_this_s;
            s_reports_this_s = 0;
            tab_tick(now);
            // Watchdog: synced but no discovery running -> restart it. NimBLE's
            // own state is the truth; s_scanning just mirrors it for status.
            bool running = s_synced && ble_gap_disc_active();
            s_scanning = running;
            if (s_synced && !running) {
                s_restarts++;
                printf("ble_scanner: scan not running -- restarting\n");
                scan_start();
            }
        }
    }
}

// --- Settings ------------------------------------------------------------

static void load_settings(void) {
    nvs_handle_t h;
    if (nvs_open(BLE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "active", &v) == ESP_OK) s_active = v != 0;
        if (nvs_get_u8(h, "coded", &v) == ESP_OK) s_coded = v != 0;
        nvs_close(h);
    }
}

static void save_u8(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(BLE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool ble_scanner_get_active(void) { return s_active; }
bool ble_scanner_get_coded(void) { return s_coded; }

void ble_scanner_set_active(bool active) {
    if (active == s_active) return;
    s_active = active;
    save_u8("active", active);
    scan_start();
}

void ble_scanner_set_coded(bool coded) {
    if (coded == s_coded) return;
    s_coded = coded;
    save_u8("coded", coded);
    scan_start();
}

void ble_scanner_set_report_hook(ble_report_hook_t hook) { s_hook = hook; }
void ble_scanner_set_target_fn(ble_target_fn_t fn) { s_target_fn = fn; }

void ble_scanner_get_status(ble_scanner_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->ok = s_synced;
    out->scanning = s_scanning;
    out->active = s_active;
    out->coded = s_coded;
    out->reports_per_s = s_reports_per_s;
    out->reports_total = s_reports_total;
    out->queue_drops = s_queue_drops;
    out->restarts = s_restarts;
    out->devices_known = s_tab_count;
    out->devices_recent = s_devices_recent;
    out->table_purges = s_table_purges;
    out->last_err = s_last_err;
}

// --- Start -----------------------------------------------------------------

bool ble_scanner_start(void) {
    load_settings();
    s_queue = xQueueCreateWithCaps(REPORT_QUEUE_LEN, sizeof(ble_report_t), MALLOC_CAP_SPIRAM);
    s_tab = heap_caps_calloc(DEV_TABLE_SIZE, sizeof(ble_device_t), MALLOC_CAP_SPIRAM);
    s_tab_tmp = heap_caps_calloc(DEV_TABLE_SIZE, sizeof(ble_device_t), MALLOC_CAP_SPIRAM);
    s_tab_mux = xSemaphoreCreateMutex();
    s_scan_mux = xSemaphoreCreateMutex();
    if (s_queue == NULL || s_tab == NULL || s_tab_tmp == NULL || s_tab_mux == NULL || s_scan_mux == NULL) {
        printf("ble_scanner: out of memory -- BLE scanning disabled\n");
        return false;
    }

    esp_err_t err = nimble_port_init(); // also brings up the BT controller
    if (err != ESP_OK) {
        printf("ble_scanner: nimble_port_init failed (%s) -- BLE scanning disabled\n", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Core 1, below the accelerometer drain (20) and the SD writer (12): it
    // only has to keep up on average -- the PSRAM queue absorbs bursts.
    if (xTaskCreatePinnedToCore(proc_task, "ble_proc", 4096, NULL, 8, NULL, 1) != pdPASS) {
        printf("ble_scanner: could not start processing task\n");
        return false;
    }
    nimble_port_freertos_init(host_task);
    return true;
}
