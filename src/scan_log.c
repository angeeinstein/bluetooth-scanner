#include "scan_log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ble_scanner.h"
#include "device_id.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "power_ctrl.h"
#include "version.h"
#include "wifi_manager.h"

#define NVS_SETTINGS "bts_log"
#define NVS_RULES "bts_targets"
#define MAX_RULES 32
// 100 % scan mode: Wi-Fi goes off this long after a session starts, so the
// dashboard that started it can still show the confirmation.
#define WIFI_OFF_GRACE_S 5

// --- CSV schema ----------------------------------------------------------------
// Keep in sync with s_format_doc below (and the README's "Scan log format").

static const char s_csv_header[] =
    "kind,uptime_ms,utc,addr,addr_kind,rssi,event,props,phy,sid,tx_power,data_status,name,"
    "name_src,company_id,mfg_data,ad_tx_power,flags,appearance,uuids,svc_data,data,target,info\n";

static const char s_motion_header[] = "uptime_ms,ax_g,ay_g,az_g\n";

static const char s_format_doc[] =
    "# BTScanner scan logs\n"
    "\n"
    "Written by BTScanner firmware " FIRMWARE_VERSION "; rewritten automatically when outdated, do not edit.\n"
    "\n"
    "## Files (same base name; `<base>` = start time `YYYY-MM-DD_HHMMSS` in UTC, or `log_NNNNN` if the\n"
    "clock was not set, plus `_<name>` if one was given)\n"
    "- `<base>.csv`: the log. Rotates at 1 GB into `<base>_part2.csv`, `_part3`, ...; every part starts\n"
    "  with the header row, and rows are never split between parts.\n"
    "- `<base>.log`: one line per event: `start` / `rotate` / `stop` / `full` (card full) / `error`, with\n"
    "  part, dev (board id), fw, utc, uptime_ms, bytes, rows, dur_s, log_drops. No end line = power loss;\n"
    "  data up to the last ~5 s sync is valid.\n"
    "- `<base>.mark`: current marker list, CSV `id,time_s,uptime_ms,unix_time_s,note` (the markers are\n"
    "  also rows in the log itself).\n"
    "- `<base>.motion.csv` (if enabled): the board's own acceleration at ~100 Hz,\n"
    "  `uptime_ms,ax_g,ay_g,az_g` -- boxcar average of the 26.7 kHz sensor, gravity included, device\n"
    "  frame. Same uptime clock as the log, so it lines up with every BLE row. Rotates like the log.\n"
    "\n"
    "## CSV columns\n"
    "One row per received BLE report (`kind=ADV`) plus event rows. Empty = not present / not applicable.\n"
    "Text fields that may contain commas (name, info) are double-quoted, `\"\"` = a literal quote.\n"
    "\n"
    "| column | meaning |\n"
    "|---|---|\n"
    "| kind | ADV, TARGET, MARK, MOTION, STATUS, INFO, START, STOP |\n"
    "| uptime_ms | ms since boot (always present; monotonic, use it to order and time rows) |\n"
    "| utc | ISO 8601 UTC with ms, if the clock was set (NTP or browser); see INFO clock_set |\n"
    "| addr | advertiser address AA:BB:CC:DD:EE:FF (MSB first) |\n"
    "| addr_kind | public, random-static, rpa (resolvable private, rotates), nrpa (non-resolvable) |\n"
    "| rssi | dBm |\n"
    "| event | legacy PDU: ADV_IND, ADV_DIRECT_IND, ADV_SCAN_IND, ADV_NONCONN_IND, SCAN_RSP; BLE 5:\n"
    "  EXT_ADV, EXT_SCAN_RSP |\n"
    "| props | report property bits (hex): 01 connectable, 02 scannable, 04 directed, 08 scan response,\n"
    "  10 legacy PDU |\n"
    "| phy | primary[/secondary] PHY: 1M, 2M, CODED (legacy reports: 1M) |\n"
    "| sid | advertising set id (extended only) |\n"
    "| tx_power | dBm from the extended advertising header |\n"
    "| data_status | 0 complete, 1 incomplete (more fragments follow in later rows), 2 truncated |\n"
    "| name | device name; cached per address, so packets without a name still carry it. Always\n"
    "  valid UTF-8: bytes that aren't (and control characters) become `?` -- the original bytes are in\n"
    "  `data` |\n"
    "| name_src | adv / rsp = this packet carried the name (in the advertisement / scan response);\n"
    "  cache = taken from an earlier packet |\n"
    "| company_id | first manufacturer-specific entry: Bluetooth SIG company id, hex (004C = Apple) |\n"
    "| mfg_data | that entry's data after the company id, hex |\n"
    "| ad_tx_power | TX power level AD field, dBm |\n"
    "| flags | flags AD field, hex |\n"
    "| appearance | appearance AD field, hex |\n"
    "| uuids | service UUIDs (16/32/128-bit), space-separated |\n"
    "| svc_data | first service-data entry: `<uuid>:<hex data>` |\n"
    "| data | the complete raw payload of this report, hex (advertisement or scan response) |\n"
    "| target | 1 if the device matches a target rule |\n"
    "| info | key=value pairs for event rows (and `direct=<addr>` for directed ADV rows) |\n"
    "\n"
    "## Event rows\n"
    "- START: session settings (scan mode, PHYs, Wi-Fi mode, target rules). STOP: reason (stop, full,\n"
    "  error), rows.\n"
    "- TARGET: a device matched a rule (`rule=`); emitted on the first match in a session and again\n"
    "  after the device was unseen for 30 s. addr, rssi, name are filled.\n"
    "- MARK: marker add / note / delete (`id=`, `action=`, `elapsed_s=`, `note=`), from the POWER\n"
    "  button or the dashboard.\n"
    "- MOTION: once per second, the board's own movement (accelerometer only, best effort): state\n"
    "  (still / moving / walking / unknown), activity_g (RMS of dynamic acceleration), peak_g, steps\n"
    "  (since boot), cadence_spm, step_m (estimated step length), speed_mps (walking speed; 0 still;\n"
    "  empty = moving but unknown), dist_m (walked since boot), rhythm (0-1: how strongly the vertical\n"
    "  motion repeated at a step/stride period over the last 4 s; walking ~0.6-0.95, vehicle or handling\n"
    "  ~0.2), axis_deg (main horizontal movement\n"
    "  axis, 0-180 from the device X axis projected horizontally; 180 deg ambiguous; empty = none),\n"
    "  axis_conf (0-1), tilt_deg (device +Z vs up), g (gravity direction x/y/z). No gyroscope or\n"
    "  compass: there is no absolute heading, and positions are not tracked. walking = a steady step\n"
    "  rhythm (rhythm >= 0.6 for 3 s, with activity); steps and dist_m then accrue from the cadence.\n"
    "  moving = activity without that rhythm (handling, vehicle, ...).\n"
    "- STATUS: once per second: rps (reports/s), devices (in table), recent (seen in 60 s), drops\n"
    "  (reports lost before processing), log_drops (rows lost before the card), wifi, battery, temperatures.\n"
    "- INFO: clock_set / clock_adjusted (the clock was set or jumped; rows before it have no or old utc),\n"
    "  wifi_off / wifi_on (100 % scan mode), rules_changed, scan_changed.\n"
    "\n"
    "Reports the radio controller discarded under heavy load are not counted anywhere.\n";

const char *scan_log_csv_header(void) {
    return s_csv_header;
}

const char *scan_log_motion_csv_header(void) {
    return s_motion_header;
}

const char *scan_log_format_doc(void) {
    return s_format_doc;
}

// --- State -----------------------------------------------------------------------

typedef enum { RULE_NAME, RULE_ADDR, RULE_PREFIX } rule_kind_t;
typedef struct {
    rule_kind_t kind;
    char val[40]; // name prefix, or upper-case "AA:BB:..." address (prefix)
} rule_t;

static rule_t s_rules[MAX_RULES];
static int s_nrules = 0;
static char s_rules_text[SCAN_LOG_RULES_MAX + 1] = SCAN_LOG_DEFAULT_RULES;
static SemaphoreHandle_t s_rules_mux = NULL;

static scan_log_settings_t s_set = {.autostart = false, .wifi_off = false, .serial = false};
static volatile int64_t s_session_start_us = 0;
static volatile bool s_wifi_forced_off = false;

// --- Small formatting helpers ---------------------------------------------------

static size_t hex(const uint8_t *b, size_t n, char *out, size_t cap) {
    static const char d[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < n && o + 2 < cap; i++) {
        out[o++] = d[b[i] >> 4];
        out[o++] = d[b[i] & 15];
    }
    out[o] = '\0';
    return o;
}

// "2026-11-26T10:12:13.456Z" from UTC microseconds; "" if 0.
static void utc_str(int64_t wall_us, char *out, size_t cap) {
    if (wall_us <= 0) {
        out[0] = '\0';
        return;
    }
    time_t sec = (time_t)(wall_us / 1000000);
    struct tm tm;
    gmtime_r(&sec, &tm);
    size_t n = strftime(out, cap, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + n, cap - n, ".%03dZ", (int)((wall_us / 1000) % 1000));
}

static int64_t wall_now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1600000000 ? (int64_t)tv.tv_sec * 1000000 + tv.tv_usec : 0;
}

// Wall time of an esp_timer instant (now minus how long ago it was); 0 if unset.
static int64_t wall_at(int64_t t_us) {
    int64_t w = wall_now_us();
    return w ? w - (esp_timer_get_time() - t_us) : 0;
}

// Appends `s` as a quoted CSV field (quotes doubled, control chars dropped).
static size_t csv_quoted(char *out, size_t cap, size_t o, const char *s) {
    if (o + 3 >= cap) return o;
    out[o++] = '"';
    for (size_t i = 0; s[i] != '\0' && o + 3 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) continue;
        if (c == '"') out[o++] = '"';
        out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = '\0';
    return o;
}

static size_t appendf(char *out, size_t cap, size_t o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t appendf(char *out, size_t cap, size_t o, const char *fmt, ...) {
    if (o >= cap) return o;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
    if (n < 0) return o;
    return (o + (size_t)n < cap) ? o + (size_t)n : cap - 1;
}

// Sends one finished line (ending in '\n') to the session and, if enabled, the
// serial port (prefixed "BTS," so it can be told apart from the boot log).
static void emit(const char *line, size_t len) {
    recorder_log_line(line, len);
    if (s_set.serial && usb_serial_jtag_is_connected()) {
        fwrite("BTS,", 1, 4, stdout);
        fwrite(line, 1, len, stdout);
    }
}

static bool session_open(void) {
    return recorder_is_recording();
}

// An event row: kind, times, optional device columns, info (unquoted text).
// Runs on several tasks (BLE processing, writer, httpd, the POWER-button
// poll task with a 4 KB stack), so the line buffer stays small on the stack;
// only long info texts (START with many rules) go through the heap.
static void emit_event(const char *kind, int64_t t_us, const char *addr, const char *addr_kind,
                       int rssi, const char *name, const char *info) {
    if (!session_open() && !s_set.serial) {
        return;
    }
    char small[512];
    size_t info_len = info ? strlen(info) : 0;
    size_t cap = info_len + 220;
    char *line = small;
    if (cap > sizeof(small)) {
        line = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (line == NULL) {
            return;
        }
    } else {
        cap = sizeof(small);
    }
    char utc[32];
    utc_str(wall_at(t_us), utc, sizeof(utc));
    size_t o = appendf(line, cap, 0, "%s,%lld,%s,%s,%s,", kind, (long long)(t_us / 1000), utc,
                       addr ? addr : "", addr_kind ? addr_kind : "");
    if (rssi != 127) {
        o = appendf(line, cap, o, "%d", rssi);
    }
    o = appendf(line, cap, o, ",,,,,,,"); // event .. data_status
    if (name && name[0]) {
        o = csv_quoted(line, cap, o, name);
    }
    o = appendf(line, cap, o, ",,,,,,,,,,,"); // name_src .. target
    if (info_len) {
        o = csv_quoted(line, cap, o, info);
    }
    o = appendf(line, cap, o, "\n");
    emit(line, o);
    if (line != small) {
        free(line);
    }
}

// --- Target rules -------------------------------------------------------------------

static bool parse_addr_text(const char *in, char *out, size_t cap, bool allow_prefix) {
    // Accepts AA:BB:CC / AA-BB-CC / AABBCC (case-insensitive); writes "AA:BB:CC".
    size_t o = 0;
    int digits = 0;
    for (const char *p = in; *p; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        if (!isxdigit((unsigned char)*p)) return false;
        if (o + 1 >= cap) return false;
        if (digits > 0 && digits % 2 == 0) out[o++] = ':';
        out[o++] = (char)toupper((unsigned char)*p);
        digits++;
    }
    out[o] = '\0';
    if (digits == 0 || digits % 2 != 0 || digits > 12) return false;
    return allow_prefix || digits == 12;
}

// Parses `text` into `rules`; returns count, or -1 with a message.
static int parse_rules(const char *text, rule_t *rules, char *err, size_t err_len) {
    int n = 0;
    const char *p = text;
    int lineno = 0;
    while (*p) {
        lineno++;
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[80];
        size_t l = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, l);
        line[l] = '\0';
        // trim
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        char *end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
        if (*s != '\0' && *s != '#') {
            if (n >= MAX_RULES) {
                snprintf(err, err_len, "too many rules (max %d)", MAX_RULES);
                return -1;
            }
            rule_t *r = &rules[n];
            if (strncmp(s, "name:", 5) == 0 && s[5] != '\0') {
                r->kind = RULE_NAME;
                snprintf(r->val, sizeof(r->val), "%s", s + 5);
            } else if (strncmp(s, "addr:", 5) == 0 && parse_addr_text(s + 5, r->val, sizeof(r->val), false)) {
                r->kind = RULE_ADDR;
            } else if (strncmp(s, "prefix:", 7) == 0 && parse_addr_text(s + 7, r->val, sizeof(r->val), true)) {
                r->kind = RULE_PREFIX;
            } else {
                snprintf(err, err_len, "line %d: expected name:<prefix>, addr:<address> or prefix:<address start>",
                         lineno);
                return -1;
            }
            n++;
        }
        p = e ? e + 1 : p + len;
    }
    return n;
}

// Index of the first rule `d` matches, or -1. Caller holds s_rules_mux.
static int match_rule(const ble_device_t *d) {
    char addr[18];
    ble_addr_str(d->addr, addr);
    for (int i = 0; i < s_nrules; i++) {
        const rule_t *r = &s_rules[i];
        switch (r->kind) {
        case RULE_NAME:
            if (d->name[0] && strncmp(d->name, r->val, strlen(r->val)) == 0) return i;
            break;
        case RULE_ADDR:
            if (strcmp(addr, r->val) == 0) return i;
            break;
        case RULE_PREFIX:
            if (strncmp(addr, r->val, strlen(r->val)) == 0) return i;
            break;
        }
    }
    return -1;
}

static bool target_fn(const ble_device_t *d) {
    xSemaphoreTake(s_rules_mux, portMAX_DELAY);
    bool m = match_rule(d) >= 0;
    xSemaphoreGive(s_rules_mux);
    return m;
}

static void rule_str(int i, char *out, size_t cap) {
    static const char *k[] = {"name", "addr", "prefix"};
    snprintf(out, cap, "%s:%s", k[s_rules[i].kind], s_rules[i].val);
}

static void rules_summary(char *out, size_t cap) {
    out[0] = '\0';
    size_t o = 0;
    for (int i = 0; i < s_nrules; i++) {
        char r[56];
        rule_str(i, r, sizeof(r));
        o = appendf(out, cap, o, "%s%s", i ? ";" : "", r);
    }
}

size_t scan_log_get_rules(char *out, size_t out_len) {
    xSemaphoreTake(s_rules_mux, portMAX_DELAY);
    snprintf(out, out_len, "%s", s_rules_text);
    xSemaphoreGive(s_rules_mux);
    return strlen(out);
}

bool scan_log_set_rules(const char *text, char *err, size_t err_len) {
    if (strlen(text) > SCAN_LOG_RULES_MAX) {
        snprintf(err, err_len, "rules too long (max %d bytes)", SCAN_LOG_RULES_MAX);
        return false;
    }
    static rule_t parsed[MAX_RULES]; // only the httpd task calls this
    int n = parse_rules(text, parsed, err, err_len);
    if (n < 0) {
        return false;
    }
    xSemaphoreTake(s_rules_mux, portMAX_DELAY);
    memcpy(s_rules, parsed, sizeof(rule_t) * (size_t)n);
    s_nrules = n;
    snprintf(s_rules_text, sizeof(s_rules_text), "%s", text);
    char summary[600];
    rules_summary(summary, sizeof(summary));
    xSemaphoreGive(s_rules_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_RULES, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "rules", text);
        nvs_commit(h);
        nvs_close(h);
    }
    ble_scanner_reeval_targets();
    char info[640];
    snprintf(info, sizeof(info), "rules_changed rules=%s", summary);
    emit_event("INFO", esp_timer_get_time(), NULL, NULL, 127, NULL, info);
    return true;
}

// --- ADV rows (BLE processing task) ------------------------------------------------

static const char *event_name(const ble_report_t *r) {
    if (r->props & BLE_PROP_LEGACY) {
        static const char *legacy[] = {"ADV_IND", "ADV_DIRECT_IND", "ADV_SCAN_IND", "ADV_NONCONN_IND", "SCAN_RSP"};
        return r->legacy_evt < 5 ? legacy[r->legacy_evt] : "LEGACY";
    }
    return (r->props & BLE_PROP_SCAN_RSP) ? "EXT_SCAN_RSP" : "EXT_ADV";
}

static const char *phy_name(uint8_t phy) {
    switch (phy) {
    case 1: return "1M";
    case 2: return "2M";
    case 3: return "CODED";
    default: return "";
    }
}

static size_t uuid_str(const uint8_t *p, uint8_t size, char *out, size_t cap) {
    if (size == 2) return (size_t)snprintf(out, cap, "%02X%02X", p[1], p[0]);
    if (size == 4) return (size_t)snprintf(out, cap, "%02X%02X%02X%02X", p[3], p[2], p[1], p[0]);
    // 128-bit, little-endian on air -> canonical 8-4-4-4-12
    char h[33];
    for (int i = 0; i < 16; i++) {
        snprintf(h + i * 2, 3, "%02X", p[15 - i]);
    }
    return (size_t)snprintf(out, cap, "%.8s-%.4s-%.4s-%.4s-%.12s", h, h + 8, h + 12, h + 16, h + 20);
}

static void report_hook(const ble_report_t *r, const ble_adv_parsed_t *p, const ble_device_t *dev) {
    bool to_sd = session_open();
    bool to_serial = s_set.serial && usb_serial_jtag_is_connected();
    char addr[18];
    ble_addr_str(r->addr, addr);
    const char *kind = ble_addr_kind(r->addr_type, r->addr);

    if (dev->target_event) {
        char info[96], rs[56] = "?";
        xSemaphoreTake(s_rules_mux, portMAX_DELAY);
        int i = match_rule(dev);
        if (i >= 0) rule_str(i, rs, sizeof(rs));
        xSemaphoreGive(s_rules_mux);
        snprintf(info, sizeof(info), "rule=%s", rs);
        emit_event("TARGET", r->t_us, addr, kind, r->rssi, dev->name, info);
    }
    if (!to_sd && !to_serial) {
        return;
    }

    // Worst case: 255-byte payload (510 hex) + mfg (506) + svc data + uuids.
    static char line[1800];
    const size_t cap = sizeof(line);
    char utc[32];
    utc_str(r->wall_us, utc, sizeof(utc));
    size_t o = appendf(line, cap, 0, "ADV,%lld,%s,%s,%s,", (long long)(r->t_us / 1000), utc, addr, kind);
    if (r->rssi != 127) o = appendf(line, cap, o, "%d", r->rssi);
    o = appendf(line, cap, o, ",%s,%02X,%s", event_name(r), r->props, phy_name(r->prim_phy));
    if (r->sec_phy) o = appendf(line, cap, o, "/%s", phy_name(r->sec_phy));
    o = appendf(line, cap, o, ",");
    if (!(r->props & BLE_PROP_LEGACY) && r->sid != 0xFF) o = appendf(line, cap, o, "%u", r->sid);
    o = appendf(line, cap, o, ",");
    if (r->tx_power != 127) o = appendf(line, cap, o, "%d", r->tx_power);
    o = appendf(line, cap, o, ",%u,", r->data_status);
    if (dev->name[0]) o = csv_quoted(line, cap, o, dev->name);
    const char *src = p->name_kind ? ((r->props & BLE_PROP_SCAN_RSP) ? "rsp" : "adv") : (dev->name[0] ? "cache" : "");
    o = appendf(line, cap, o, ",%s,", src);
    if (p->has_mfg) {
        o = appendf(line, cap, o, "%04X,", p->company_id);
        o += hex(p->mfg_data, p->mfg_len, line + o, cap - o);
    } else {
        o = appendf(line, cap, o, ",");
    }
    o = appendf(line, cap, o, ",");
    if (p->has_tx_power) o = appendf(line, cap, o, "%d", p->tx_power);
    o = appendf(line, cap, o, ",");
    if (p->has_flags) o = appendf(line, cap, o, "%02X", p->flags);
    o = appendf(line, cap, o, ",");
    if (p->has_appearance) o = appendf(line, cap, o, "%04X", p->appearance);
    o = appendf(line, cap, o, ",");
    for (int i = 0; i < p->n_uuids && o + 40 < cap; i++) {
        if (i) line[o++] = ' ';
        o += uuid_str(p->uuids[i].p, p->uuids[i].size, line + o, cap - o);
    }
    o = appendf(line, cap, o, ",");
    if (p->has_svc_data && o + 40 < cap) {
        o += uuid_str(p->svc_data, p->svc_uuid_size, line + o, cap - o);
        line[o++] = ':';
        o += hex(p->svc_data + p->svc_uuid_size, p->svc_data_len - p->svc_uuid_size, line + o, cap - o);
    }
    o = appendf(line, cap, o, ",");
    o += hex(r->data, r->data_len, line + o, cap - o);
    o = appendf(line, cap, o, ",%s,", dev->target ? "1" : "");
    if (r->props & BLE_PROP_DIRECTED) {
        char da[18];
        ble_addr_str(r->direct_addr, da);
        o = appendf(line, cap, o, "direct=%s", da);
    }
    o = appendf(line, cap, o, "\n");
    emit(line, o);
}

// --- Session / marker rows (called by recorder.c) -----------------------------------------

void scan_log_session_begin(const char *base) {
    s_session_start_us = esp_timer_get_time();
    ble_scanner_rearm_targets(); // every log lists the targets present at its start
    char devid[16];
    device_id_str(devid, sizeof(devid));
    char rules[600];
    xSemaphoreTake(s_rules_mux, portMAX_DELAY);
    rules_summary(rules, sizeof(rules));
    xSemaphoreGive(s_rules_mux);
    char info[900];
    snprintf(info, sizeof(info),
             "base=%s dev=%s fw=%s scan=%s phy=%s duplicates=all wifi_mode=%s motion_file=%s accel_range_g=%d "
             "rules=%s", base, devid,
             FIRMWARE_VERSION, ble_scanner_get_active() ? "active" : "passive",
             ble_scanner_get_coded() ? "1M+CODED" : "1M",
             s_set.wifi_off ? "off_while_logging" : "on", motion_log_enabled() ? "on" : "off",
             motion_get_range_g(), rules[0] ? rules : "-");
    emit_event("START", s_session_start_us, NULL, NULL, 127, NULL, info);
}

void scan_log_session_end(const char *reason) {
    char info[96];
    snprintf(info, sizeof(info), "reason=%s rows=%llu log_drops=%u", reason,
             (unsigned long long)recorder_get_rows_written(), (unsigned)recorder_get_log_drops());
    emit_event("STOP", esp_timer_get_time(), NULL, NULL, 127, NULL, info);
    s_session_start_us = 0;
    if (s_wifi_forced_off) { // give the dashboard back right away
        s_wifi_forced_off = false;
        wifi_manager_set_radio_enabled(true);
    }
}

void scan_log_marker_event(const recorder_marker_t *m, const char *action) {
    char info[128];
    snprintf(info, sizeof(info), "id=%u action=%s elapsed_s=%.3f note=%s", (unsigned)m->id, action,
             m->elapsed_s, m->note);
    emit_event("MARK", m->at_us, NULL, NULL, 127, NULL, info);
}

void scan_log_motion_row(const motion_status_t *m, int64_t t_us) {
    char info[400];
    size_t o = (size_t)snprintf(info, sizeof(info), "state=%s activity_g=%.3f peak_g=%.3f steps=%u cadence_spm=%.0f",
                                motion_state_name(m->state), (double)m->activity_g, (double)m->peak_g,
                                (unsigned)m->steps, (double)m->cadence_spm);
    o = appendf(info, sizeof(info), o, " step_m=%.2f speed_mps=", (double)m->step_m);
    if (m->speed_mps >= 0) o = appendf(info, sizeof(info), o, "%.2f", (double)m->speed_mps);
    o = appendf(info, sizeof(info), o, " dist_m=%.1f rhythm=%.2f axis_deg=", (double)m->dist_m, (double)m->rhythm);
    if (m->axis_deg >= 0) o = appendf(info, sizeof(info), o, "%.0f", (double)m->axis_deg);
    o = appendf(info, sizeof(info), o, " axis_conf=%.2f tilt_deg=%.0f g=%.2f/%.2f/%.2f", (double)m->axis_conf,
                (double)m->tilt_deg, (double)m->gx, (double)m->gy, (double)m->gz);
    emit_event("MOTION", t_us, NULL, NULL, 127, NULL, info);
}

// --- 1 Hz task: STATUS rows, clock watch, Wi-Fi switching -------------------------------------

static void status_row(void) {
    ble_scanner_status_t b;
    ble_scanner_get_status(&b);
    power_status_t pw;
    power_ctrl_get_status(&pw);
    wifi_status_t w;
    wifi_manager_get_status(&w);
    const char *wifi = !wifi_manager_get_radio_enabled() ? "off"
                       : (w.sta_state == WIFI_STA_CONNECTED) ? "sta" : "ap";
    char info[384];
    snprintf(info, sizeof(info),
             "rps=%u devices=%u recent=%u total=%llu drops=%u log_drops=%u restarts=%u scanning=%d wifi=%s "
             "batt_pct=%.0f vbat_mv=%d usb=%d mcu_c=%.1f chg_c=%.1f free_heap=%u free_psram=%u "
             "motion_rows=%llu motion_drops=%u",
             (unsigned)b.reports_per_s, (unsigned)b.devices_known, (unsigned)b.devices_recent,
             (unsigned long long)b.reports_total, (unsigned)b.queue_drops, (unsigned)recorder_get_log_drops(),
             (unsigned)b.restarts, b.scanning ? 1 : 0, wifi, (double)pw.batt_percent, pw.vbat_mv,
             pw.on_battery ? 0 : 1, (double)pw.mcu_temp_c, (double)pw.tdie_c,
             (unsigned)esp_get_free_internal_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long long)recorder_get_motion_rows(), (unsigned)recorder_get_motion_drops());
    emit_event("STATUS", esp_timer_get_time(), NULL, NULL, 127, NULL, info);
}

static void tick_task(void *arg) {
    (void)arg;
    int64_t last_wall = 0, last_up = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t up = esp_timer_get_time();
        int64_t wall = wall_now_us();

        // Clock watch: first set, or a jump of more than 2 s against uptime.
        if (wall != 0 && last_wall == 0) {
            wifi_status_t w;
            wifi_manager_get_status(&w);
            char utc[32], info[96];
            utc_str(wall, utc, sizeof(utc));
            snprintf(info, sizeof(info), "clock_set utc=%s source=%s", utc, w.ntp_synced ? "ntp" : "browser");
            emit_event("INFO", up, NULL, NULL, 127, NULL, info);
        } else if (wall != 0 && last_wall != 0) {
            int64_t drift = (wall - last_wall) - (up - last_up);
            if (drift > 2000000 || drift < -2000000) {
                char info[64];
                snprintf(info, sizeof(info), "clock_adjusted delta_ms=%lld", (long long)(drift / 1000));
                emit_event("INFO", up, NULL, NULL, 127, NULL, info);
            }
        }
        last_wall = wall;
        last_up = up;

        bool rec = recorder_is_recording();
        if (rec) {
            status_row();
        }

        // 100 % scan mode.
        bool want_off = rec && s_set.wifi_off && s_session_start_us != 0 &&
                        up - s_session_start_us >= (int64_t)WIFI_OFF_GRACE_S * 1000000;
        if (want_off && !s_wifi_forced_off) {
            s_wifi_forced_off = true;
            emit_event("INFO", up, NULL, NULL, 127, NULL, "wifi_off reason=100pct_scan_mode");
            wifi_manager_set_radio_enabled(false);
        } else if (!want_off && s_wifi_forced_off && !(rec && s_set.wifi_off)) {
            s_wifi_forced_off = false;
            emit_event("INFO", up, NULL, NULL, 127, NULL, "wifi_on");
            wifi_manager_set_radio_enabled(true);
        }
    }
}

int scan_log_wifi_off_in_s(void) {
    if (!s_set.wifi_off || !recorder_is_recording() || s_session_start_us == 0 || s_wifi_forced_off) {
        return -1;
    }
    int64_t left = (int64_t)WIFI_OFF_GRACE_S * 1000000 - (esp_timer_get_time() - s_session_start_us);
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

// --- Settings ---------------------------------------------------------------------------

static void save_u8(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void scan_log_get_settings(scan_log_settings_t *out) {
    *out = s_set;
}

void scan_log_set_autostart(bool on) {
    s_set.autostart = on;
    save_u8("autostart", on);
}

void scan_log_set_wifi_off(bool on) {
    s_set.wifi_off = on;
    save_u8("wifi_off", on);
}

void scan_log_set_serial(bool on) {
    s_set.serial = on;
    save_u8("serial", on);
}

void scan_log_init(void) {
    s_rules_mux = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "autostart", &v) == ESP_OK) s_set.autostart = v != 0;
        if (nvs_get_u8(h, "wifi_off", &v) == ESP_OK) s_set.wifi_off = v != 0;
        if (nvs_get_u8(h, "serial", &v) == ESP_OK) s_set.serial = v != 0;
        nvs_close(h);
    }
    if (nvs_open(NVS_RULES, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_rules_text);
        if (nvs_get_str(h, "rules", s_rules_text, &n) != ESP_OK) {
            snprintf(s_rules_text, sizeof(s_rules_text), "%s", SCAN_LOG_DEFAULT_RULES);
        }
        nvs_close(h);
    }
    char err[96];
    int n = parse_rules(s_rules_text, s_rules, err, sizeof(err));
    if (n < 0) { // stored rules unreadable -- fall back to the default
        printf("scan_log: stored target rules invalid (%s) -- using default\n", err);
        snprintf(s_rules_text, sizeof(s_rules_text), "%s", SCAN_LOG_DEFAULT_RULES);
        n = parse_rules(s_rules_text, s_rules, err, sizeof(err));
    }
    s_nrules = n < 0 ? 0 : n;

    ble_scanner_set_target_fn(target_fn);
    ble_scanner_set_report_hook(report_hook);
    // Stack in PSRAM (internal RAM is scarce with Wi-Fi + BLE). Allowed: this
    // task never writes flash -- settings are saved from the httpd task.
    if (xTaskCreatePinnedToCoreWithCaps(tick_task, "scan_log", 6144, NULL, 4, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        printf("scan_log: could not start its 1 Hz task\n");
    }
}

void scan_log_maybe_autostart(void) {
    if (!s_set.autostart) {
        return;
    }
    if (recorder_request_start("auto")) {
        printf("scan_log: auto-start -- logging\n");
    } else {
        printf("scan_log: auto-start skipped (%s)\n", recorder_get_last_error());
    }
}
