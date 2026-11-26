#include "dashboard.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>

#include "accel_capture.h"
#include "ble_scanner.h"
#include "motion.h"
#include "scan_log.h"
#include "dashboard_html.h"
#include "sync_trigger.h"
#include "device_id.h"
#include "device_report.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_private/esp_clk.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "live_html.h"
#include "ota.h"
#include "ota_cloud.h"
#include "power_ctrl.h"
#include "recorder.h"
#include "sd_benchmark.h"
#include "udp_stream.h"
#include "version.h"
#include "wifi_manager.h"
#include "ws_stream.h"

#define REC_DIR RECORDER_DIR // BTScanner sessions live in /sdcard/ble, not VibraShock's /rec

static httpd_handle_t s_server = NULL;
static volatile int s_active_downloads = 0;          // in-flight async file downloads
static volatile bool s_download_throttling = false;  // download slowed to protect an active recording

// --- Tiny hand-rolled JSON helpers (no cJSON dependency -- both ends of
// this API are our own code, so a full parser would be overkill) ---------

static size_t append(char *buf, size_t buf_len, size_t off, const char *fmt, ...) {
    if (off >= buf_len) {
        return off;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + off, buf_len - off, fmt, args);
    va_end(args);
    if (n < 0) {
        return off;
    }
    return off + (size_t)n;
}

static void json_escape(char *dst, size_t dst_len, const char *src) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 2 < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c == '\n') {
            dst[di++] = '\\';
            dst[di++] = 'n';
        } else if (c < 0x20) {
            continue;
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

// Finds `"key":` in a request body and returns a pointer just past the
// colon (skipping whitespace), or NULL if not present. Good enough for the
// small, fixed-shape bodies our own dashboard JS sends.
static const char *json_find_value(const char *body, const char *key) {
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(body, pattern);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return p;
}

static bool json_extract_string(const char *p, char *out, size_t out_len) {
    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    size_t oi = 0;
    while (*p != '"' && *p != '\0' && oi + 1 < out_len) {
        out[oi++] = *p++;
    }
    out[oi] = '\0';
    return true;
}

static bool json_extract_bool(const char *p, bool *out) {
    if (p == NULL) {
        return false;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_extract_int(const char *p, int *out) {
    if (p == NULL) {
        return false;
    }
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool json_extract_int64(const char *p, int64_t *out) {
    if (p == NULL) {
        return false;
    }
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

static bool read_body(httpd_req_t *req, char *buf, size_t buf_len) {
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= buf_len) {
        buf[0] = '\0';
        return total == 0; // empty body is fine, just nothing to parse
    }
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            return false;
        }
        received += ret;
    }
    buf[received] = '\0';
    return true;
}

// --- WebSocket live-graph push -------------------------------------------
//
// GET /ws just completes the handshake and returns -- it never loops, so it
// can't block the server's single accept/dispatch task. A background task
// (fed by accel_capture's decimated preview ring) periodically finds
// connected WS clients and pushes to them asynchronously via
// httpd_queue_work()+httpd_ws_send_frame_async(), exactly the pattern
// esp_http_server's own docs call out for async sends.

// httpd's max_open_sockets is capped by CONFIG_LWIP_MAX_SOCKETS (16 in this
// build, see sdkconfig.defaults), minus 3 reserved internally by the HTTP
// server itself. We deliberately keep this at 7 rather than the higher ceiling
// so the remaining sockets stay free for mDNS, SNTP, and the outbound HTTPS
// client that pulls OTA updates -- otherwise a busy dashboard starves that
// client and its socket() fails.
#define MAX_WS_CLIENTS 7

// Backpressure cap on outstanding async preview sends -- same rationale as
// ws_stream.c (a slow/backpressured websocket must not let queued malloc'd
// copies pile up). This channel also fans out to the /wsstream socket, which
// can be badly backpressured while Analyze streams the full rate.
#define WS_PREVIEW_MAX_INFLIGHT 8
static atomic_int s_ws_inflight; // zero-initialised

typedef struct {
    httpd_handle_t hd;
    int fd;
    uint8_t *payload;
    size_t len;
} ws_send_arg_t;

static void ws_async_send(void *arg) {
    ws_send_arg_t *a = (ws_send_arg_t *)arg;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = a->payload,
        .len = a->len,
    };
    httpd_ws_send_frame_async(a->hd, a->fd, &frame);
    free(a->payload);
    free(a);
    atomic_fetch_sub(&s_ws_inflight, 1);
}

static void ws_broadcast(const char *text, size_t len) {
    if (s_server == NULL) {
        return;
    }
    // Drop this preview frame rather than pile more malloc'd copies behind sends
    // the httpd task hasn't drained yet (see WS_PREVIEW_MAX_INFLIGHT).
    if (atomic_load(&s_ws_inflight) >= WS_PREVIEW_MAX_INFLIGHT) {
        return;
    }
    int fds[MAX_WS_CLIENTS];
    size_t count = MAX_WS_CLIENTS;
    if (httpd_get_client_list(s_server, &count, fds) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
        ws_send_arg_t *arg = malloc(sizeof(ws_send_arg_t));
        if (arg == NULL) {
            continue;
        }
        arg->payload = malloc(len);
        if (arg->payload == NULL) {
            free(arg);
            continue;
        }
        memcpy(arg->payload, text, len);
        arg->hd = s_server;
        arg->fd = fds[i];
        arg->len = len;
        if (httpd_queue_work(s_server, ws_async_send, arg) != ESP_OK) {
            free(arg->payload);
            free(arg);
        } else {
            atomic_fetch_add(&s_ws_inflight, 1);
        }
    }
}

static void ws_push_task(void *arg) {
    accel_preview_point_t points[16];
    char json[640];

    for (;;) {
        size_t n = accel_capture_read_preview(points, 16);
        if (n > 0) {
            size_t off = 0;
            off = append(json, sizeof(json), off, "{\"pts\":[");
            for (size_t i = 0; i < n; i++) {
                off = append(json, sizeof(json), off, "%s[%.3f,%.3f,%.3f]", i == 0 ? "" : ",",
                             (double)points[i].x, (double)points[i].y, (double)points[i].z);
            }
            off = append(json, sizeof(json), off, "]}");
            ws_broadcast(json, off);
        }
        // Preview points arrive every ~9.36ms (26.7kHz raw / 250x boxcar
        // decimation, see accel_capture.c). Polling every 10ms keeps each
        // broadcast at a steady ~1 point (occasionally 2 if a tick was
        // delayed), close to the production cadence -- so the dashboard's
        // chart (which advances one point per point received) scrolls
        // evenly rather than in jittery batches.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t handler_ws(httpd_req_t *req) {
    // Handshake is already complete by the time this is called for the
    // initial GET; nothing to do. We never read incoming frames -- this
    // channel is server-push only.
    return ESP_OK;
}

// GET /wsstream -- the high-rate binary live-stream WebSocket used by
// /live. Same "handshake only, never loops" shape as handler_ws; the only
// difference is registering the client's fd with ws_stream.c so its
// always-running send task knows who to broadcast to.
static esp_err_t handler_wsstream(httpd_req_t *req) {
    ws_stream_register_client(httpd_req_to_sockfd(req));
    return ESP_OK;
}

// --- /api/status -----------------------------------------------------------

static const char *wifi_sta_state_str(wifi_sta_state_t s) {
    switch (s) {
        case WIFI_STA_IDLE: return "idle";
        case WIFI_STA_CONNECTING: return "connecting";
        case WIFI_STA_CONNECTED: return "connected";
        case WIFI_STA_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *sd_bench_state_str(sd_bench_state_t s) {
    switch (s) {
        case SD_BENCH_IDLE: return "idle";
        case SD_BENCH_WRITING: return "writing";
        case SD_BENCH_READING: return "reading";
        case SD_BENCH_DONE: return "done";
        case SD_BENCH_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *accel_selftest_state_str(accel_selftest_state_t s) {
    switch (s) {
        case ACCEL_SELFTEST_IDLE: return "idle";
        case ACCEL_SELFTEST_RUNNING: return "running";
        case ACCEL_SELFTEST_PASS: return "pass";
        case ACCEL_SELFTEST_FAIL: return "fail";
        default: return "unknown";
    }
}

static float fs_enum_to_g(iis3dwb_fs_t fs) {
    switch (fs) {
        case IIS3DWB_FS_2G: return 2.0f;
        case IIS3DWB_FS_4G: return 4.0f;
        case IIS3DWB_FS_8G: return 8.0f;
        case IIS3DWB_FS_16G: return 16.0f;
        default: return 2.0f;
    }
}

// Human-readable reason for the most recent reset. esp_reset_reason() stays
// valid for the whole session, so the dashboard can answer "why did the device
// last restart?" -- key for spotting brownout resets (e.g. from Wi-Fi TX bursts
// mid-recording) in the field, where a USB serial monitor isn't available.
static const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "panic (crash)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_EXT:       return "external pin";
        default:                return "unknown";
    }
}

static char *build_status_json(void) {
    // Static, not malloc'd: the dashboard polls this every second, and a
    // transient low-heap moment (busy recording + live stream) must not turn
    // it into a 500 that blanks the whole dashboard. Safe because
    // esp_http_server dispatches handlers serially on its single task, so this
    // is never re-entered. free_heap is reported so heap pressure is visible.
    static char buf[6144];
    const size_t cap = sizeof(buf);
    size_t off = 0;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    off = append(buf, cap, off, "{");
    off = append(buf, cap, off,
                 "\"chip\":{\"cores\":%d,\"freq_mhz\":%d,\"flash_mb\":%u,\"psram_mb\":%u,\"free_heap\":%u},",
                 chip.cores, esp_clk_cpu_freq() / 1000000, (unsigned)(flash_size / (1024 * 1024)),
                 (unsigned)(esp_psram_is_initialized() ? esp_psram_get_size() / (1024 * 1024) : 0),
                 (unsigned)esp_get_free_heap_size());

    // Static device identity (About card): a MAC-derived per-board id + the
    // firmware version/build stamp/author from version.h.
    char devid[16];
    device_id_str(devid, sizeof(devid));
    char author_esc[64], build_esc[48];
    json_escape(author_esc, sizeof(author_esc), FIRMWARE_AUTHOR);
    json_escape(build_esc, sizeof(build_esc), FIRMWARE_BUILD);
    off = append(buf, cap, off,
                 "\"device\":{\"id\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\","
                 "\"build\":\"%s\",\"author\":\"%s\",\"reset_reason\":\"%s\"},",
                 devid, FIRMWARE_NAME, FIRMWARE_VERSION, build_esc, author_esc,
                 reset_reason_str());

    ota_status_t ota_st;
    ota_get_status(&ota_st);
    char ota_part_esc[24];
    json_escape(ota_part_esc, sizeof(ota_part_esc), ota_st.running_partition);
    bool cloud_active = false;
    char cloud_err[80];
    ota_cloud_get_status(&cloud_active, cloud_err, sizeof(cloud_err));
    char cloud_err_esc[96];
    json_escape(cloud_err_esc, sizeof(cloud_err_esc), cloud_err);
    off = append(buf, cap, off,
                 "\"ota\":{\"available\":%s,\"partition\":\"%s\",\"state\":%d,\"bytes\":%u,"
                 "\"total\":%u,\"cloud_active\":%s,\"cloud_error\":\"%s\"},",
                 ota_st.available ? "true" : "false", ota_part_esc, (int)ota_st.state,
                 (unsigned)ota_st.bytes_written, (unsigned)ota_st.total_bytes,
                 cloud_active ? "true" : "false", cloud_err_esc);

    float accel_temp_c = 0;
    bool accel_temp_valid = accel_capture_read_temp_c(&accel_temp_c);
    off = append(buf, cap, off,
                 "\"accel\":{\"ready\":%s,\"fs_g\":%.0f,\"sensor_overruns\":%u,\"measured_odr_hz\":%.1f,"
                 "\"temp_c\":%.1f,\"temp_valid\":%s},",
                 accel_capture_is_ready() ? "true" : "false", (double)fs_enum_to_g(accel_capture_get_fs()),
                 (unsigned)accel_capture_get_sensor_overruns(), (double)accel_capture_get_measured_odr_hz(),
                 (double)accel_temp_c, accel_temp_valid ? "true" : "false");

    motion_status_t mo;
    motion_get_status(&mo);
    off = append(buf, cap, off,
                 "\"motion\":{\"ok\":%s,\"state\":\"%s\",\"rate_hz\":%.2f,\"act\":%.4f,\"peak\":%.3f,"
                 "\"steps\":%u,\"cad\":%.0f,\"step_m\":%.2f,\"speed\":%.2f,\"dist\":%.1f,\"rhy\":%.2f,"
                 "\"axis\":%.0f,\"conf\":%.2f,\"tilt\":%.0f,\"g\":[%.2f,%.2f,%.2f],\"dropped\":%u,\"log\":%s,"
                 "\"range\":%d,\"rows\":%llu,\"mdrops\":%u},",
                 mo.ok ? "true" : "false", motion_state_name(mo.state), (double)mo.rate_hz,
                 (double)mo.activity_g, (double)mo.peak_g, (unsigned)mo.steps, (double)mo.cadence_spm,
                 (double)mo.step_m, (double)mo.speed_mps, (double)mo.dist_m, (double)mo.rhythm, (double)mo.axis_deg,
                 (double)mo.axis_conf, (double)mo.tilt_deg, (double)mo.gx, (double)mo.gy, (double)mo.gz,
                 (unsigned)mo.dropped, motion_log_enabled() ? "true" : "false", motion_get_range_g(),
                 (unsigned long long)recorder_get_motion_rows(), (unsigned)recorder_get_motion_drops());

    accel_selftest_result_t st;
    accel_capture_self_test_get_result(&st);
    off = append(buf, cap, off,
                 "\"selftest\":{\"state\":\"%s\",\"x_mg\":%.0f,\"y_mg\":%.0f,\"z_mg\":%.0f,"
                 "\"min_mg\":%.0f,\"max_mg\":%.0f},",
                 accel_selftest_state_str(st.state), (double)st.change_mg_x, (double)st.change_mg_y,
                 (double)st.change_mg_z, 800.0, 3200.0);

    char fname_esc[200];
    json_escape(fname_esc, sizeof(fname_esc), recorder_get_current_filename());
    char rec_err_esc[80];
    json_escape(rec_err_esc, sizeof(rec_err_esc), recorder_get_last_error());
    char rec_notice_esc[80];
    json_escape(rec_notice_esc, sizeof(rec_notice_esc), recorder_get_last_notice());
    off = append(buf, cap, off,
                 "\"recording\":{\"active\":%s,\"filename\":\"%s\",\"bytes_written\":%llu,"
                 "\"rows\":%llu,\"elapsed_s\":%.1f,\"rate_bps\":%.0f,\"log_drops\":%u,"
                 "\"markers\":%d,\"wifi_off_in\":%d,\"last_error\":\"%s\",\"notice\":\"%s\"},",
                 recorder_is_recording() ? "true" : "false", fname_esc,
                 (unsigned long long)recorder_get_bytes_written(),
                 (unsigned long long)recorder_get_rows_written(), recorder_get_elapsed_s(),
                 (double)recorder_get_bytes_per_s(), (unsigned)recorder_get_log_drops(),
                 recorder_marker_count(), scan_log_wifi_off_in_s(), rec_err_esc, rec_notice_esc);

    scan_log_settings_t ls;
    scan_log_get_settings(&ls);
    off = append(buf, cap, off,
                 "\"settings\":{\"button_mark\":%s,\"autostart\":%s,\"wifi_off\":%s,\"serial\":%s},",
                 power_ctrl_get_button_mark() ? "true" : "false", ls.autostart ? "true" : "false",
                 ls.wifi_off ? "true" : "false", ls.serial ? "true" : "false");

    uint64_t sd_total = 0, sd_free = 0;
    bool sd_space_ok = recorder_get_sd_space(&sd_total, &sd_free);
    off = append(buf, cap, off,
                 "\"sdcard\":{\"mounted\":%s,\"total_bytes\":%llu,\"free_bytes\":%llu},",
                 recorder_is_sd_mounted() ? "true" : "false",
                 (unsigned long long)(sd_space_ok ? sd_total : 0),
                 (unsigned long long)(sd_space_ok ? sd_free : 0));

    off = append(buf, cap, off, "\"download\":{\"active\":%s,\"throttled\":%s},",
                 s_active_downloads > 0 ? "true" : "false",
                 s_download_throttling ? "true" : "false");

    sd_bench_result_t bench;
    sd_benchmark_get_result(&bench);
    char bench_err_esc[80];
    json_escape(bench_err_esc, sizeof(bench_err_esc), bench.error);
    off = append(buf, cap, off,
                 "\"sdbench\":{\"state\":\"%s\",\"total_bytes\":%u,\"bytes_done\":%u,"
                 "\"write_mb_s\":%.2f,\"read_mb_s\":%.2f,\"worst_chunk_ms\":%u,"
                 "\"checksum_ok\":%s,\"error\":\"%s\"},",
                 sd_bench_state_str(bench.state), (unsigned)bench.total_bytes,
                 (unsigned)bench.bytes_done, (double)bench.write_mb_s, (double)bench.read_mb_s,
                 (unsigned)bench.worst_chunk_ms, bench.checksum_ok ? "true" : "false",
                 bench_err_esc);

    power_status_t pw;
    power_ctrl_get_status(&pw);
    char ts_esc[48];
    json_escape(ts_esc, sizeof(ts_esc), pw.ts_status);
    char phase_esc[32];
    json_escape(phase_esc, sizeof(phase_esc), pw.chg_phase);
    off = append(buf, cap, off, "\"charger\":{\"ok\":%s", pw.ok ? "true" : "false");
    if (pw.ok) {
        off = append(buf, cap, off,
                     ",\"vbus_mv\":%d,\"vbat_mv\":%d,\"vsys_mv\":%d,\"ichg_ma\":%d,"
                     "\"ts_status\":\"%s\",\"on_battery\":%s,\"fault\":%s,\"ts_ignore\":%s,"
                     "\"charging\":%s,\"chg_phase\":\"%s\","
                     "\"ibus_ma\":%d,\"tdie_c\":%.1f,\"thermal_regulation\":%s,"
                     "\"input_current_limited\":%s,\"input_voltage_limited\":%s",
                     pw.vbus_mv, pw.vbat_mv, pw.vsys_mv, pw.ichg_ma, ts_esc,
                     pw.on_battery ? "true" : "false", pw.fault ? "true" : "false",
                     pw.ts_ignored ? "true" : "false", pw.charging ? "true" : "false", phase_esc,
                     pw.ibus_ma, (double)pw.tdie_c, pw.thermal_regulation ? "true" : "false",
                     pw.input_current_limited ? "true" : "false",
                     pw.input_voltage_limited ? "true" : "false");
    }
    off = append(buf, cap, off, "},");

    // Best-effort coulomb-counted runtime estimate (power_ctrl.c) -- percent
    // and hours_remaining are what the dashboard actually displays;
    // nominal_mah/learned_mah/learn_cycles are intentionally not shown
    // anywhere in the UI (see GET /api/battery/reset), just along for
    // anyone checking via devtools/curl.
    off = append(buf, cap, off,
                 "\"battery\":{\"present\":%s,\"percent\":%.1f,\"hours_remaining\":%.2f,\"nominal_mah\":%d,"
                 "\"learned_mah\":%d,\"learn_cycles\":%d,\"low_warning\":%s},",
                 pw.battery_present ? "true" : "false", (double)pw.batt_percent,
                 (double)pw.batt_hours_remaining, pw.batt_nominal_mah, pw.batt_learned_mah,
                 pw.batt_learn_cycles, pw.low_warning ? "true" : "false");

    wifi_status_t wifi;
    wifi_manager_get_status(&wifi);
    char ssid_esc[48];
    json_escape(ssid_esc, sizeof(ssid_esc), wifi.sta_ssid);
    char reason_esc[64];
    json_escape(reason_esc, sizeof(reason_esc), wifi.sta_fail_reason);
    // Device wall clock (UTC). Empty until NTP or the browser fallback has set
    // it -- same "year >= 2020 means synced" test the recorder uses for naming.
    char clock_str[24] = "";
    {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        if (tmv.tm_year + 1900 >= 2020) {
            strftime(clock_str, sizeof(clock_str), "%Y-%m-%d %H:%M:%S", &tmv);
        }
    }
    char hostname[40], ap_ssid[40];
    wifi_manager_get_hostname(hostname, sizeof(hostname));
    wifi_manager_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
    // Connected AP BSSID ("" if not associated) + live signal, from one driver call
    // (wifi.rssi is never refreshed after connect), and the optional AP pin.
    uint8_t bssid[6];
    int8_t rssi = 0; // 0 if not associated
    wifi_manager_get_ap_info(bssid, &rssi);
    char bssid_str[18] = "";
    if (bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5]) {
        snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1],
                 bssid[2], bssid[3], bssid[4], bssid[5]);
    }
    uint8_t pin_bssid[6];
    char pin_ssid[33] = "", pin_ssid_esc[64], pin_str[18] = "";
    if (wifi_manager_get_ap_pin(pin_ssid, sizeof(pin_ssid), pin_bssid)) {
        snprintf(pin_str, sizeof(pin_str), "%02X:%02X:%02X:%02X:%02X:%02X", pin_bssid[0],
                 pin_bssid[1], pin_bssid[2], pin_bssid[3], pin_bssid[4], pin_bssid[5]);
    }
    json_escape(pin_ssid_esc, sizeof(pin_ssid_esc), pin_ssid);
    off = append(buf, cap, off,
                 "\"wifi\":{\"sta_state\":\"%s\",\"sta_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                 "\"ntp_synced\":%s,\"clock\":\"%s\",\"sta_fail_reason\":\"%s\",\"tx_power_reduced\":%s,"
                 "\"enterprise\":%s,\"tsf_us\":%lld,\"hostname\":\"%s\",\"ap_ssid\":\"%s\","
                 "\"bssid\":\"%s\",\"pinned_bssid\":\"%s\",\"pinned_ssid\":\"%s\","
                 "\"mdns_friendly\":%s,\"report_enabled\":%s,\"saved\":[",
                 wifi_sta_state_str(wifi.sta_state), ssid_esc, wifi.ip, (int)rssi,
                 wifi.ntp_synced ? "true" : "false", clock_str, reason_esc,
                 wifi.tx_power_reduced ? "true" : "false", wifi.enterprise ? "true" : "false",
                 (long long)wifi_manager_get_tsf_us(), hostname, ap_ssid, bssid_str, pin_str,
                 pin_ssid_esc, wifi_manager_get_mdns_friendly() ? "true" : "false",
                 device_report_get_enabled() ? "true" : "false");
    wifi_saved_info_t saved[WIFI_MAX_SAVED];
    int nsaved = wifi_manager_get_saved(saved, WIFI_MAX_SAVED);
    for (int i = 0; i < nsaved; i++) {
        char sv_esc[48];
        json_escape(sv_esc, sizeof(sv_esc), saved[i].ssid);
        off = append(buf, cap, off,
                     "%s{\"ssid\":\"%s\",\"enterprise\":%s,\"connected\":%s,\"auth_failed\":%s}",
                     i == 0 ? "" : ",", sv_esc, saved[i].enterprise ? "true" : "false",
                     saved[i].connected ? "true" : "false", saved[i].auth_failed ? "true" : "false");
    }
    off = append(buf, cap, off, "]},");

    ble_scanner_status_t ble;
    ble_scanner_get_status(&ble);
    off = append(buf, cap, off,
                 "\"ble\":{\"ok\":%s,\"scanning\":%s,\"active\":%s,\"coded\":%s,"
                 "\"rps\":%u,\"total\":%llu,\"drops\":%u,\"restarts\":%u,"
                 "\"devices\":%u,\"recent\":%u,\"purges\":%u,\"tev\":%u,\"err\":%d},",
                 ble.ok ? "true" : "false", ble.scanning ? "true" : "false",
                 ble.active ? "true" : "false", ble.coded ? "true" : "false",
                 (unsigned)ble.reports_per_s, (unsigned long long)ble.reports_total,
                 (unsigned)ble.queue_drops, (unsigned)ble.restarts, (unsigned)ble.devices_known,
                 (unsigned)ble.devices_recent, (unsigned)ble.table_purges,
                 (unsigned)ble.target_events, ble.last_err);

    udp_stream_status_t udp;
    udp_stream_get_status(&udp);
    off = append(buf, cap, off,
                 "\"udp\":{\"enabled\":%s,\"target_ip\":\"%s\",\"target_port\":%u,"
                 "\"packets_sent\":%llu,\"bytes_sent\":%llu,\"drops\":%u},",
                 udp.enabled ? "true" : "false", udp.target_ip, (unsigned)udp.target_port,
                 (unsigned long long)udp.packets_sent, (unsigned long long)udp.bytes_sent,
                 (unsigned)udp.drops);

    ws_stream_status_t wsstream;
    ws_stream_get_status(&wsstream);
    off = append(buf, cap, off,
                 "\"wsstream\":{\"enabled\":%s,\"frames_sent\":%llu,\"bytes_sent\":%llu,"
                 "\"drops\":%u}",
                 wsstream.enabled ? "true" : "false", (unsigned long long)wsstream.frames_sent,
                 (unsigned long long)wsstream.bytes_sent, (unsigned)wsstream.drops);

    off = append(buf, cap, off, "}");
    return buf;
}

// --- HTTP handlers -----------------------------------------------------

static esp_err_t handler_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, dashboard_html, strlen(dashboard_html));
    return ESP_OK;
}

static esp_err_t handler_live(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, live_html, strlen(live_html));
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req) {
    // The dashboard polls this ~once/second while open -- treat it as the
    // "device is being used" signal for the Wi-Fi reconnect backoff.
    wifi_manager_note_activity();
    char *json = build_status_json();
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK; // json points at a static buffer -- nothing to free
}

static esp_err_t handler_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

static esp_err_t handler_error(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/motion/config {"range":2|4|8|16, "log":0|1} -- either key optional.
static esp_err_t handler_motion_config(httpd_req_t *req) {
    char body[64];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int v = 0;
    if (json_extract_int(json_find_value(body, "range"), &v) && !motion_set_range_g(v)) {
        return handler_error(req, "400 Bad Request", "range must be 2, 4, 8 or 16 (or the sensor is missing)");
    }
    if (json_extract_int(json_find_value(body, "log"), &v)) {
        motion_set_log_enabled(v != 0);
    }
    return handler_ok(req);
}

// GET /api/motion/history -- 1 Hz, oldest first: act (mg), spd (cm/s, -1
// unknown), st (0 unknown, 1 still, 2 moving, 3 walking), cad (steps/min).
static esp_err_t handler_motion_history(httpd_req_t *req) {
    const int N = MOTION_HIST_SECONDS;
    uint16_t *act = malloc(sizeof(uint16_t) * N);
    int16_t *spd = malloc(sizeof(int16_t) * N);
    uint8_t *st = malloc(N), *cad = malloc(N);
    const size_t cap = (size_t)N * 24 + 64;
    char *buf = malloc(cap);
    if (!act || !spd || !st || !cad || !buf) {
        free(act); free(spd); free(st); free(cad); free(buf);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int n = motion_get_history(act, spd, st, cad, N);
    size_t off = append(buf, cap, 0, "{\"act\":[");
    for (int i = 0; i < n; i++) off = append(buf, cap, off, "%s%u", i ? "," : "", act[i]);
    off = append(buf, cap, off, "],\"spd\":[");
    for (int i = 0; i < n; i++) off = append(buf, cap, off, "%s%d", i ? "," : "", spd[i]);
    off = append(buf, cap, off, "],\"st\":[");
    for (int i = 0; i < n; i++) off = append(buf, cap, off, "%s%u", i ? "," : "", st[i]);
    off = append(buf, cap, off, "],\"cad\":[");
    for (int i = 0; i < n; i++) off = append(buf, cap, off, "%s%u", i ? "," : "", cad[i]);
    off = append(buf, cap, off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(act); free(spd); free(st); free(cad); free(buf);
    return ESP_OK;
}

static esp_err_t handler_mdns_friendly(httpd_req_t *req) {
    char body[48];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int enabled = 0;
    if (!json_extract_int(json_find_value(body, "enabled"), &enabled)) {
        return handler_error(req, "400 Bad Request", "missing enabled");
    }
    wifi_manager_set_mdns_friendly(enabled != 0);
    return handler_ok(req);
}

static esp_err_t handler_report_enabled(httpd_req_t *req) {
    char body[48];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int enabled = 0;
    if (!json_extract_int(json_find_value(body, "enabled"), &enabled)) {
        return handler_error(req, "400 Bad Request", "missing enabled");
    }
    device_report_set_enabled(enabled != 0);
    return handler_ok(req);
}

// External sync-trigger log: every rising edge on IO38 with the TSF latched at
// that instant. Fetch from two boards and align by "n" to measure their true
// clock offset (see the multi-device-sync tooling).
static esp_err_t handler_sync_edges(httpd_req_t *req) {
    sync_edge_t edges[128];
    int n = sync_trigger_get_edges(edges, 128);
    httpd_resp_set_type(req, "application/json");
    char head[64];
    int hl = snprintf(head, sizeof(head), "{\"count\":%u,\"edges\":[",
                      (unsigned)sync_trigger_get_count());
    httpd_resp_send_chunk(req, head, hl);
    for (int i = 0; i < n; i++) {
        char item[64];
        int il = snprintf(item, sizeof(item), "%s{\"n\":%u,\"tsf\":%lld}", i ? "," : "",
                          (unsigned)edges[i].n, (long long)edges[i].tsf_us);
        httpd_resp_send_chunk(req, item, il);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_sync_reset(httpd_req_t *req) {
    sync_trigger_reset();
    return handler_ok(req);
}

static esp_err_t handler_accel_selftest(httpd_req_t *req) {
    if (recorder_is_recording()) {
        return handler_error(req, "409 Conflict", "cannot self-test while recording");
    }
    if (!accel_capture_self_test_start()) {
        return handler_error(req, "409 Conflict",
                             "self-test unavailable (already running or sensor not ready)");
    }
    return handler_ok(req);
}

static esp_err_t handler_record_start(httpd_req_t *req) {
    char body[128];
    char name[64] = "";
    if (read_body(req, body, sizeof(body))) {
        json_extract_string(json_find_value(body, "name"), name, sizeof(name));
    }
    if (!recorder_request_start(name)) {
        return handler_error(req, "409 Conflict", recorder_get_last_error());
    }
    return handler_ok(req);
}

static esp_err_t handler_record_stop(httpd_req_t *req) {
    recorder_request_stop();
    return handler_ok(req);
}

static esp_err_t handler_sdbench_run(httpd_req_t *req) {
    if (!sd_benchmark_start()) {
        return handler_error(req, "409 Conflict", "already running (or SD not mounted)");
    }
    return handler_ok(req);
}

static esp_err_t handler_charger_ship(httpd_req_t *req) {
    power_ctrl_trigger_ship_mode();
    return handler_ok(req);
}

static esp_err_t handler_charger_ts_ignore(httpd_req_t *req) {
    char body[64];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    bool ignore = false;
    if (!json_extract_bool(json_find_value(body, "ignore"), &ignore)) {
        return handler_error(req, "400 Bad Request", "missing ignore");
    }
    if (!power_ctrl_set_ts_ignore(ignore)) {
        return handler_error(req, "500 Internal Server Error", "charger write failed");
    }
    return handler_ok(req);
}

// GET /api/battery/reset[?mah=<capacity>] -- resets the adaptive
// battery-runtime learning state (see power_ctrl_reset_battery_learning()).
// Deliberately not linked from anywhere in the dashboard UI -- this is a
// "you have to know the URL" maintenance action for after physically
// changing the battery, not a normal setting. GET rather than POST for
// bare browser-address-bar convenience; the only consequence of an
// accidental trigger is the estimate re-learning over a few cycles, not
// anything destructive.
// POST /api/party -- a few seconds of colourful status-LED animation, for fun.
static esp_err_t handler_party(httpd_req_t *req) {
    power_ctrl_party_start();
    return handler_ok(req);
}

// POST /api/led/test -- TEMPORARY: hold the RGB LED at {r,g,b} (0-255) so status
// colours can be eyeballed, or {"off":true} to clear. Remove with the palette set.
static esp_err_t handler_led_test(httpd_req_t *req) {
    char body[96];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    bool off = false;
    json_extract_bool(json_find_value(body, "off"), &off);
    if (off) {
        power_ctrl_led_test(false, 0, 0, 0);
        return handler_ok(req);
    }
    int r = 0, g = 0, b = 0;
    json_extract_int(json_find_value(body, "r"), &r);
    json_extract_int(json_find_value(body, "g"), &g);
    json_extract_int(json_find_value(body, "b"), &b);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    power_ctrl_led_test(true, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    return handler_ok(req);
}

static esp_err_t handler_battery_reset(httpd_req_t *req) {
    char query[32];
    char mah_str[16] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "mah", mah_str, sizeof(mah_str));
    }
    power_ctrl_reset_battery_learning(mah_str[0] != '\0' ? atoi(mah_str) : -1);
    return handler_ok(req);
}

static esp_err_t handler_wifi_connect(httpd_req_t *req) {
    // Roomier than the personal-only case used to need: an enterprise body
    // carries ssid + username + password together.
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char ssid[33] = "";
    char pass[65] = "";
    char user[65] = "";
    if (!json_extract_string(json_find_value(body, "ssid"), ssid, sizeof(ssid))) {
        return handler_error(req, "400 Bad Request", "missing ssid");
    }
    json_extract_string(json_find_value(body, "password"), pass, sizeof(pass));
    bool enterprise = false;
    json_extract_bool(json_find_value(body, "enterprise"), &enterprise);

    bool ok;
    if (enterprise) {
        if (!json_extract_string(json_find_value(body, "username"), user, sizeof(user)) ||
            user[0] == '\0') {
            return handler_error(req, "400 Bad Request", "missing username");
        }
        ok = wifi_manager_connect_enterprise(ssid, user, pass);
    } else {
        ok = wifi_manager_connect(ssid, pass);
    }
    if (!ok) {
        return handler_error(req, "500 Internal Server Error", "connect failed");
    }
    return handler_ok(req);
}

static bool wifi_ssid_from_body(httpd_req_t *req, char *ssid, size_t ssid_len) {
    char body[64];
    if (!read_body(req, body, sizeof(body))) {
        return false;
    }
    return json_extract_string(json_find_value(body, "ssid"), ssid, ssid_len) && ssid[0] != '\0';
}

static esp_err_t handler_wifi_select(httpd_req_t *req) {
    char ssid[33] = "";
    if (!wifi_ssid_from_body(req, ssid, sizeof(ssid))) {
        return handler_error(req, "400 Bad Request", "missing ssid");
    }
    wifi_manager_select(ssid);
    return handler_ok(req);
}

static esp_err_t handler_wifi_forget(httpd_req_t *req) {
    char ssid[33] = "";
    if (!wifi_ssid_from_body(req, ssid, sizeof(ssid))) {
        return handler_error(req, "400 Bad Request", "missing ssid");
    }
    wifi_manager_forget(ssid);
    return handler_ok(req);
}

static esp_err_t handler_wifi_scan(httpd_req_t *req) {
    wifi_scan_result_t results[20];
    int count = wifi_manager_scan(results, 20);
    if (count < 0) {
        return handler_error(req, "500 Internal Server Error", "scan failed");
    }

    // ~120 bytes/entry worst case: a 32-char SSID can escape to ~64 bytes
    // (every char a quote/backslash doubling), plus the ~50-byte
    // {"ssid":"...","rssi":-100,"secure":false} scaffolding. The old 80
    // under-budgeted that and could truncate the JSON for many long SSIDs
    // (append() is bounded so it never overflowed, but the result was
    // unparseable and the scan silently "failed" in the UI).
    size_t cap = 64 + (size_t)count * 120;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return handler_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t off = 0;
    off = append(buf, cap, off, "{\"networks\":[");
    for (int i = 0; i < count; i++) {
        char ssid_esc[64];
        json_escape(ssid_esc, sizeof(ssid_esc), results[i].ssid);
        off = append(buf, cap, off,
                     "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s,\"enterprise\":%s}",
                     i == 0 ? "" : ",", ssid_esc, results[i].rssi,
                     results[i].secure ? "true" : "false",
                     results[i].enterprise ? "true" : "false");
    }
    off = append(buf, cap, off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    return ESP_OK;
}

// GET /api/wifi/aps -- per-BSSID scan of the connected SSID (the APs you can pin
// to). Not collapsed like /api/wifi/scan.
static esp_err_t handler_wifi_aps(httpd_req_t *req) {
    wifi_status_t st;
    wifi_manager_get_status(&st);
    const char *filter =
        (st.sta_state == WIFI_STA_CONNECTED && st.sta_ssid[0] != '\0') ? st.sta_ssid : NULL;
    wifi_ap_scan_t aps[24];
    int count = wifi_manager_scan_aps(filter, aps, 24);
    if (count < 0) {
        return handler_error(req, "500 Internal Server Error", "scan failed");
    }
    size_t cap = 64 + (size_t)count * 140;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return handler_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t off = append(buf, cap, 0, "{\"aps\":[");
    for (int i = 0; i < count; i++) {
        char ssid_esc[64], bstr[18];
        json_escape(ssid_esc, sizeof(ssid_esc), aps[i].ssid);
        snprintf(bstr, sizeof(bstr), "%02X:%02X:%02X:%02X:%02X:%02X", aps[i].bssid[0],
                 aps[i].bssid[1], aps[i].bssid[2], aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        off = append(buf, cap, off,
                     "%s{\"ssid\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d,\"channel\":%d,"
                     "\"secure\":%s,\"enterprise\":%s}",
                     i == 0 ? "" : ",", ssid_esc, bstr, aps[i].rssi, aps[i].channel,
                     aps[i].secure ? "true" : "false", aps[i].enterprise ? "true" : "false");
    }
    off = append(buf, cap, off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    return ESP_OK;
}

static bool parse_bssid(const char *s, uint8_t out[6]) {
    unsigned b[6];
    if (s == NULL ||
        sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[i];
    }
    return true;
}

// POST /api/wifi/pin {ssid, bssid} -- pin that SSID to that exact AP.
static esp_err_t handler_wifi_pin(httpd_req_t *req) {
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char ssid[33] = "", bssid_str[24] = "";
    json_extract_string(json_find_value(body, "ssid"), ssid, sizeof(ssid));
    json_extract_string(json_find_value(body, "bssid"), bssid_str, sizeof(bssid_str));
    uint8_t bssid[6];
    if (ssid[0] == '\0' || !parse_bssid(bssid_str, bssid)) {
        return handler_error(req, "400 Bad Request", "need ssid + bssid");
    }
    if (!wifi_manager_set_ap_pin(ssid, bssid)) {
        return handler_error(req, "500 Internal Server Error", "could not set pin");
    }
    return handler_ok(req);
}

static esp_err_t handler_wifi_pin_clear(httpd_req_t *req) {
    wifi_manager_clear_ap_pin();
    return handler_ok(req);
}

static esp_err_t handler_time_set(httpd_req_t *req) {
    char body[48];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int64_t unix_ms = 0;
    if (!json_extract_int64(json_find_value(body, "unix_ms"), &unix_ms)) {
        return handler_error(req, "400 Bad Request", "missing unix_ms");
    }
    // Best-effort: false just means NTP already owns the clock, or the
    // value was rejected as implausible -- neither is an error worth
    // surfacing to the caller, the browser fire-and-forgets this anyway.
    wifi_manager_set_time_from_client(unix_ms);
    return handler_ok(req);
}

// Matches WRITE_CHUNK_SIZE/BENCH_CHUNK_SIZE elsewhere in this project --
// 4KB reads measured at only ~0.47MB/s in practice (per-chunk SD-read and
// HTTP chunk-framing overhead dominates at that size); 32KB gets close to
// the ~1.46MB/s ceiling sd_benchmark.c already measured for this card.
#define DOWNLOAD_CHUNK_SIZE (32 * 1024)

// Adaptive SD-contention throttle. A download reads the SD card while a
// recording writes it; if the card can't sustain both, the recorder's ring
// overflows and drops samples. This closed loop watches the recording's actual
// drop counter and slows the download just enough to stop the drops, then
// speeds back up once it's clean. It only ever engages while a recording is
// active AND drops are climbing, so it never throttles needlessly; and it's
// bounded (never fully stalls the download), so if the drops turn out not to be
// the download's fault it settles at the floor rather than choking to a halt.
#define DL_THROTTLE_EVAL_TICKS pdMS_TO_TICKS(300) // re-evaluate the drop rate this often
#define DL_THROTTLE_STEP_MS 10                    // per-chunk delay added when drops are seen
#define DL_THROTTLE_MAX_MS 60                     // floor: ~350 KB/s, never fully stalled
#define DL_THROTTLE_DECAY_MS 10                   // delay removed each clean interval

// Serves RECORDER_DIR/<file> (a session's .csv part, .log or .mark) as a
// chunked download -- files can be much larger than available RAM, so this
// streams from the SD card rather than buffering the whole thing.
// The actual file-streaming work. Shared by the async download worker (the
// normal path) and by a synchronous fallback if going async ever fails.
static esp_err_t do_download(httpd_req_t *req) {
    // Sized for the longest recording names (rename allows ~128 chars) plus a
    // "_partN.aux.csv" suffix, URL-encoded in the query.
    char query[384];
    char filename[160] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }
    if (filename[0] == '\0' || strstr(filename, "..") != NULL || strchr(filename, '/') != NULL) {
        return handler_error(req, "400 Bad Request", "missing or invalid file param");
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", REC_DIR, filename);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return handler_error(req, "404 Not Found", "file not found");
    }

    size_t nlen = strlen(filename);
    const char *content_type = (nlen > 4 && strcmp(filename + nlen - 4, ".csv") == 0)
                                    ? "text/csv"
                                    : "application/octet-stream";
    httpd_resp_set_type(req, content_type);
    char cd_header[200];
    snprintf(cd_header, sizeof(cd_header), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", cd_header);

    // Read into DMA-capable *internal* RAM and pull the file with raw read(): a
    // plain malloc of this size lands in PSRAM (which the SDMMC DMA can't reach,
    // forcing a slow per-transfer bounce), and stdio would add its own buffering.
    //
    // RECORDING IS THE TOP PRIORITY. The recorder's own SD writes bounce through a
    // small internal DMA buffer the driver allocates per write, so a download must
    // never take the internal RAM it needs. Therefore we only use the fast internal
    // path when NO recording is active, and even then cap it at 16KB -- already far
    // faster than Wi-Fi can send (the real bottleneck), so a bigger buffer would buy
    // nothing while enlarging the footprint. While recording -- or if internal DMA
    // RAM isn't free -- we fall back to a PSRAM buffer: the old, slower bounce path,
    // but one that can never compete with the recorder for internal memory. (SD-bus
    // contention during a recording is handled separately by the throttle below.)
    size_t dl_chunk = 0;
    uint8_t *chunk = NULL;
    if (!recorder_is_recording()) {
        for (dl_chunk = 16 * 1024; dl_chunk >= 4096; dl_chunk /= 2) {
            chunk = heap_caps_malloc(dl_chunk, MALLOC_CAP_DMA);
            if (chunk != NULL) {
                break;
            }
        }
    }
    if (chunk == NULL) { // recording active, or no internal DMA free -- safe PSRAM path
        dl_chunk = DOWNLOAD_CHUNK_SIZE;
        chunk = malloc(dl_chunk);
    }
    if (chunk == NULL) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int in_fd = fileno(f);
    ssize_t n;
    bool send_failed = false;
    int throttle_ms = 0;
    uint32_t last_drops = recorder_get_log_drops();
    TickType_t last_eval = xTaskGetTickCount();
    while ((n = read(in_fd, chunk, dl_chunk)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)chunk, (size_t)n) != ESP_OK) {
            send_failed = true;
            break;
        }
        // Adaptive throttle -- only relevant while a recording competes for the card.
        if (recorder_is_recording()) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_eval >= DL_THROTTLE_EVAL_TICKS) {
                uint32_t drops = recorder_get_log_drops();
                if (drops != last_drops) { // recorder is losing samples -> back off
                    throttle_ms += DL_THROTTLE_STEP_MS;
                    if (throttle_ms > DL_THROTTLE_MAX_MS) {
                        throttle_ms = DL_THROTTLE_MAX_MS;
                    }
                } else if (throttle_ms > 0) { // clean interval -> speed back up
                    throttle_ms -= DL_THROTTLE_DECAY_MS;
                }
                last_drops = drops;
                last_eval = now;
            }
            s_download_throttling = throttle_ms > 0;
            if (throttle_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(throttle_ms));
            }
        } else if (throttle_ms != 0 || s_download_throttling) {
            throttle_ms = 0; // recording ended -- resume full speed
            s_download_throttling = false;
        }
    }
    s_download_throttling = false;
    free(chunk);
    fclose(f);
    if (send_failed) {
        return ESP_FAIL; // connection dropped mid-transfer -- nothing more we can send
    }
    httpd_resp_send_chunk(req, NULL, 0); // terminate the chunked response
    return ESP_OK;
}

// A large file transfer takes minutes, and esp_http_server runs *every* request
// on one task -- so streaming a download inline blocks the entire dashboard
// (status polls, WebSocket, everything) until it finishes. Instead the download
// is handed to this dedicated worker via the async-handler API, freeing the
// httpd task immediately so the UI stays responsive during a download.
#define MAX_CONCURRENT_DOWNLOADS 1
static QueueHandle_t s_download_queue = NULL;

static void download_worker_task(void *arg) {
    httpd_req_t *areq;
    for (;;) {
        if (xQueueReceive(s_download_queue, &areq, portMAX_DELAY) == pdTRUE) {
            do_download(areq);
            httpd_req_async_handler_complete(areq); // releases the socket back to httpd
            s_active_downloads--;
        }
    }
}

static esp_err_t handler_download(httpd_req_t *req) {
    if (s_download_queue == NULL) {
        return do_download(req); // worker not up (shouldn't happen) -- stream inline
    }
    // The recordings detail sheet loads two tiny text sidecars (.log and .mark)
    // at the same time. Routing both through the single big-file download worker
    // made the second one 503 -- which is why a recording's markers sometimes
    // didn't show, or only appeared seconds later on the next list refresh.
    // They're only a few KB, so stream them inline and skip the one-at-a-time
    // worker gate entirely (big .csv downloads still go through it).
    {
        char query[384];
        char filename[160] = "";
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            httpd_query_key_value(query, "file", filename, sizeof(filename));
        }
        size_t n = strlen(filename);
        if ((n >= 5 && strcmp(filename + n - 5, ".mark") == 0) ||
            (n >= 4 && strcmp(filename + n - 4, ".log") == 0)) {
            return do_download(req);
        }
    }
    // Single-threaded httpd task, so this check + the increment below are not
    // racy against another download request.
    if (s_active_downloads >= MAX_CONCURRENT_DOWNLOADS) {
        return handler_error(req, "503 Service Unavailable",
                             "a download is already in progress; try again shortly");
    }
    httpd_req_t *areq = NULL;
    if (httpd_req_async_handler_begin(req, &areq) != ESP_OK) {
        return do_download(req); // couldn't detach -- fall back to inline streaming
    }
    s_active_downloads++;
    if (xQueueSend(s_download_queue, &areq, 0) != pdTRUE) {
        s_active_downloads--;
        httpd_req_async_handler_complete(areq);
        return handler_error(req, "503 Service Unavailable", "busy, try again shortly");
    }
    return ESP_OK; // the worker finishes and completes the async request
}

static esp_err_t handler_sdcard_remount(httpd_req_t *req) {
    // No error on "no card found" -- that's an expected outcome, not a
    // failure; /api/status's sdcard.mounted reflects the real result.
    recorder_remount();
    return handler_ok(req);
}

// --- Logging sessions on the card ---------------------------------------------
// A session is every file in RECORDER_DIR sharing one base name:
// <base>.csv, <base>_partN.csv, <base>.log, <base>.mark.

#define SESSION_MAX_FILES 512

typedef struct {
    char name[96];
    uint32_t size;
} dir_file_t;

// If `name` belongs to session `base`, returns the suffix after the base
// (".csv", ".motion.csv", "_part2.csv", "_part2.motion.csv", ".log", ".mark"),
// else NULL.
static const char *session_suffix(const char *name, const char *base) {
    size_t bl = strlen(base);
    if (strncmp(name, base, bl) != 0) {
        return NULL;
    }
    const char *suf = name + bl;
    if (strcmp(suf, ".csv") == 0 || strcmp(suf, ".motion.csv") == 0 || strcmp(suf, ".log") == 0 ||
        strcmp(suf, ".mark") == 0) {
        return suf;
    }
    if (strncmp(suf, "_part", 5) == 0) {
        const char *c = suf + 5;
        if (!isdigit((unsigned char)*c)) {
            return NULL;
        }
        while (isdigit((unsigned char)*c)) c++;
        if (strcmp(c, ".csv") == 0 || strcmp(c, ".motion.csv") == 0) {
            return suf;
        }
    }
    return NULL;
}

// The session base of a scan-log .csv file name ("x_part3.csv" -> "x"), or
// false if it isn't one. A session's motion file ("x.motion.csv") is not a
// scan log -- it belongs to session "x", it doesn't start one of its own.
static bool csv_base(const char *name, char *out, size_t out_len) {
    size_t n = strlen(name);
    if (n < 5 || strcmp(name + n - 4, ".csv") != 0) {
        return false;
    }
    if (n >= 11 && strcmp(name + n - 11, ".motion.csv") == 0) {
        return false;
    }
    snprintf(out, out_len, "%.*s", (int)(n - 4), name);
    char *last = NULL;
    for (char *c = out; (c = strstr(c, "_part")) != NULL; c++) {
        last = c;
    }
    if (last != NULL && last[5] != '\0') {
        bool digits = true;
        for (char *c = last + 5; *c; c++) {
            digits &= isdigit((unsigned char)*c) != 0;
        }
        if (digits) {
            *last = '\0';
        }
    }
    return out[0] != '\0';
}

// Reads RECORDER_DIR into a PSRAM array; returns the count (or -1).
static int read_session_dir(dir_file_t **out) {
    DIR *d = opendir(REC_DIR);
    if (d == NULL) {
        return -1;
    }
    dir_file_t *files = heap_caps_calloc(SESSION_MAX_FILES, sizeof(dir_file_t), MALLOC_CAP_SPIRAM);
    if (files == NULL) {
        closedir(d);
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < SESSION_MAX_FILES) {
        if (e->d_type == DT_DIR || strlen(e->d_name) >= sizeof(files[0].name)) {
            continue;
        }
        memcpy(files[n].name, e->d_name, strlen(e->d_name) + 1); // length checked above
        char path[200];
        snprintf(path, sizeof(path), "%s/%s", REC_DIR, files[n].name);
        struct stat st;
        files[n].size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
        n++;
    }
    closedir(d);
    *out = files;
    return n;
}

// Pulls `key=` values from the last and first lines of a session's .log.
typedef struct {
    char start_utc[24];
    char end[12]; // stop / full / error / "" (still open or power loss)
    double dur_s;
    unsigned long long rows;
    unsigned long long motion_rows;
    unsigned log_drops, motion_drops;
} session_log_t;

static void parse_session_log(const char *base, session_log_t *out) {
    memset(out, 0, sizeof(*out));
    char path[200];
    snprintf(path, sizeof(path), "%s/%s.log", REC_DIR, base);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[256];
    bool first = true;
    while (fgets(line, sizeof(line), f) != NULL) {
        char ev[12] = "";
        sscanf(line, "%11s", ev);
        const char *u = strstr(line, " utc=");
        if (first && u != NULL) {
            sscanf(u + 5, "%23s", out->start_utc);
            first = false;
        }
        const char *r = strstr(line, " rows=");
        if (r) out->rows = strtoull(r + 6, NULL, 10);
        const char *du = strstr(line, " dur_s=");
        if (du) out->dur_s = strtod(du + 7, NULL);
        const char *ld = strstr(line, " log_drops=");
        if (ld) out->log_drops = (unsigned)strtoul(ld + 11, NULL, 10);
        const char *mr = strstr(line, " motion_rows=");
        if (mr) out->motion_rows = strtoull(mr + 13, NULL, 10);
        const char *md = strstr(line, " motion_drops=");
        if (md) out->motion_drops = (unsigned)strtoul(md + 14, NULL, 10);
        if (strcmp(ev, "start") == 0 || strcmp(ev, "rotate") == 0) {
            out->end[0] = '\0';
        } else {
            snprintf(out->end, sizeof(out->end), "%s", ev);
        }
    }
    fclose(f);
}

// GET /api/recordings -- one JSON object per session, streamed in chunks.
static esp_err_t handler_recordings_list(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    dir_file_t *files = NULL;
    int nf = read_session_dir(&files);
    if (nf < 0) {
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }
    // The base of the session being written right now (if any).
    char active_base[100] = "";
    const char *cur = recorder_get_current_filename();
    if (cur[0]) {
        const char *slash = strrchr(cur, '/');
        csv_base(slash ? slash + 1 : cur, active_base, sizeof(active_base));
    }

    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    for (int i = 0; i < nf; i++) {
        char base[100];
        // One entry per session: anchor on part 1 (<base>.csv) only.
        if (!csv_base(files[i].name, base, sizeof(base))) continue;
        char want[110];
        snprintf(want, sizeof(want), "%s.csv", base);
        if (strcmp(files[i].name, want) != 0) continue;

        uint64_t bytes = 0;
        int parts = 0;
        bool has_log = false, has_mark = false;
        char flist[2048];
        size_t fo = 0;
        for (int j = 0; j < nf; j++) {
            const char *suf = session_suffix(files[j].name, base);
            if (suf == NULL) continue;
            bytes += files[j].size;
            if (strcmp(suf, ".log") == 0) has_log = true;
            else if (strcmp(suf, ".mark") == 0) has_mark = true;
            else {
                bool motion = strstr(suf, ".motion.csv") != NULL;
                parts += !motion;
                char ne[200];
                json_escape(ne, sizeof(ne), files[j].name);
                fo = append(flist, sizeof(flist), fo, "%s{\"name\":\"%s\",\"bytes\":%u,\"motion\":%s}",
                            fo ? "," : "", ne, (unsigned)files[j].size, motion ? "true" : "false");
            }
        }
        session_log_t lg;
        parse_session_log(base, &lg);
        bool active = strcmp(base, active_base) == 0;
        char base_esc[200];
        json_escape(base_esc, sizeof(base_esc), base);
        char item[2600];
        int n = snprintf(item, sizeof(item),
                         "%s{\"id\":\"%s\",\"bytes\":%llu,\"parts\":%d,\"files\":[%s],\"log\":%s,\"mark\":%s,"
                         "\"start\":\"%s\",\"dur_s\":%.1f,\"rows\":%llu,\"end\":\"%s\",\"active\":%s,"
                         "\"log_drops\":%u,\"motion_rows\":%llu,\"motion_drops\":%u}",
                         first ? "" : ",", base_esc, (unsigned long long)bytes, parts, fo ? flist : "",
                         has_log ? "true" : "false", has_mark ? "true" : "false", lg.start_utc, lg.dur_s,
                         lg.rows, lg.end, active ? "true" : "false", lg.log_drops, lg.motion_rows,
                         lg.motion_drops);
        if (n > 0) {
            httpd_resp_send_chunk(req, item, strlen(item));
            first = false;
        }
    }
    free(files);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_udp_start(httpd_req_t *req) {
    char body[96];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char ip[16] = "";
    if (!json_extract_string(json_find_value(body, "ip"), ip, sizeof(ip))) {
        return handler_error(req, "400 Bad Request", "missing ip");
    }
    int port = 0;
    if (!json_extract_int(json_find_value(body, "port"), &port)) {
        return handler_error(req, "400 Bad Request", "missing port");
    }
    if (port <= 0 || port > 65535) {
        return handler_error(req, "400 Bad Request", "invalid port");
    }

    // Mutually exclusive with the browser's high-rate WS stream (sending
    // full-rate data twice would double the load for no benefit) -- starting
    // one silently stops the other rather than erroring, per the user's
    // preference.
    ws_stream_stop();

    if (!udp_stream_start(ip, (uint16_t)port)) {
        return handler_error(req, "400 Bad Request", "invalid ip");
    }
    return handler_ok(req);
}

static esp_err_t handler_udp_stop(httpd_req_t *req) {
    udp_stream_stop();
    return handler_ok(req);
}

static esp_err_t handler_wsstream_start(httpd_req_t *req) {
    udp_stream_stop(); // see handler_udp_start's comment -- exclusive with UDP streaming
    ws_stream_start();
    return handler_ok(req);
}

static esp_err_t handler_wsstream_stop(httpd_req_t *req) {
    ws_stream_stop();
    return handler_ok(req);
}

// The base of the session being written, or "" -- delete/rename refuse it.
static void active_session_base(char *out, size_t out_len) {
    out[0] = '\0';
    const char *cur = recorder_get_current_filename();
    if (cur[0]) {
        const char *slash = strrchr(cur, '/');
        csv_base(slash ? slash + 1 : cur, out, out_len);
    }
}

static bool valid_base(const char *b) {
    return b[0] != '\0' && strchr(b, '/') == NULL && strstr(b, "..") == NULL;
}

// POST /api/recordings/delete {"id":"<base>"} -- removes every file of the session.
static esp_err_t handler_recordings_delete(httpd_req_t *req) {
    char body[192];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char id[120] = "";
    if (!json_extract_string(json_find_value(body, "id"), id, sizeof(id)) || !valid_base(id)) {
        return handler_error(req, "400 Bad Request", "missing or invalid id");
    }
    char active[100];
    active_session_base(active, sizeof(active));
    if (strcmp(id, active) == 0) {
        return handler_error(req, "409 Conflict", "this session is still being logged - stop it first");
    }
    dir_file_t *files = NULL;
    int nf = read_session_dir(&files);
    if (nf < 0) {
        return handler_error(req, "503 Service Unavailable", "SD card not available");
    }
    int removed = 0;
    for (int i = 0; i < nf; i++) {
        if (session_suffix(files[i].name, id) != NULL) {
            char path[200];
            snprintf(path, sizeof(path), "%s/%s", REC_DIR, files[i].name);
            removed += (remove(path) == 0);
        }
    }
    free(files);
    if (removed == 0) {
        return handler_error(req, "404 Not Found", "no such session");
    }
    return handler_ok(req);
}

// Keeps letters/digits/_/-, turns spaces into underscores, drops the rest.
static void sanitize_rec_name(const char *in, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            out[j++] = c;
        } else if (c == ' ') {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

// POST /api/recordings/rename {"id":"<base>","name":"<new base>"} -- renames
// every file of the session. Refuses the active session and taken names.
static esp_err_t handler_recordings_rename(httpd_req_t *req) {
    char body[288];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char id[120] = "", name_raw[120] = "", name[96];
    if (!json_extract_string(json_find_value(body, "id"), id, sizeof(id)) ||
        !json_extract_string(json_find_value(body, "name"), name_raw, sizeof(name_raw)) || !valid_base(id)) {
        return handler_error(req, "400 Bad Request", "missing id or name");
    }
    sanitize_rec_name(name_raw, name, sizeof(name));
    if (name[0] == '\0') {
        return handler_error(req, "400 Bad Request", "name has no usable characters");
    }
    if (strcmp(id, name) == 0) {
        return handler_ok(req);
    }
    char active[100];
    active_session_base(active, sizeof(active));
    if (strcmp(id, active) == 0) {
        return handler_error(req, "409 Conflict", "this session is still being logged - stop it first");
    }
    dir_file_t *files = NULL;
    int nf = read_session_dir(&files);
    if (nf < 0) {
        return handler_error(req, "503 Service Unavailable", "SD card not available");
    }
    for (int i = 0; i < nf; i++) {
        if (session_suffix(files[i].name, name) != NULL) {
            free(files);
            return handler_error(req, "409 Conflict", "a session with that name already exists");
        }
    }
    int renamed = 0;
    bool failed = false;
    for (int i = 0; i < nf; i++) {
        const char *suf = session_suffix(files[i].name, id);
        if (suf == NULL) continue;
        char from[200], to[200];
        snprintf(from, sizeof(from), "%s/%s", REC_DIR, files[i].name);
        snprintf(to, sizeof(to), "%s/%s%s", REC_DIR, name, suf);
        if (rename(from, to) == 0) {
            renamed++;
        } else {
            failed = true;
        }
    }
    free(files);
    if (renamed == 0) {
        return handler_error(req, "404 Not Found", "no such session");
    }
    if (failed) {
        return handler_error(req, "500 Internal Server Error", "some files could not be renamed");
    }
    return handler_ok(req);
}

// --- Logging settings, targets, device detail, history ------------------------

// POST /api/log/config {"autostart":0|1, "wifi_off":0|1, "serial":0|1} -- any subset.
static esp_err_t handler_log_config(httpd_req_t *req) {
    char body[96];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int v = 0;
    if (json_extract_int(json_find_value(body, "autostart"), &v)) scan_log_set_autostart(v != 0);
    if (json_extract_int(json_find_value(body, "wifi_off"), &v)) scan_log_set_wifi_off(v != 0);
    if (json_extract_int(json_find_value(body, "serial"), &v)) scan_log_set_serial(v != 0);
    return handler_ok(req);
}

// GET /api/targets -> the rule text as plain text.
static esp_err_t handler_targets_get(httpd_req_t *req) {
    char *text = malloc(SCAN_LOG_RULES_MAX + 1);
    if (text == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    size_t n = scan_log_get_rules(text, SCAN_LOG_RULES_MAX + 1);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, text, n);
    free(text);
    return ESP_OK;
}

// POST /api/targets with the rule text as a plain-text body.
static esp_err_t handler_targets_set(httpd_req_t *req) {
    if (req->content_len > SCAN_LOG_RULES_MAX) {
        return handler_error(req, "400 Bad Request", "rules too long");
    }
    char *text = malloc(SCAN_LOG_RULES_MAX + 1);
    if (text == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, text + got, req->content_len - got);
        if (r <= 0) {
            free(text);
            return handler_error(req, "400 Bad Request", "bad body");
        }
        got += r;
    }
    text[got] = '\0';
    char err[120];
    bool ok = scan_log_set_rules(text, err, sizeof(err));
    free(text);
    if (!ok) {
        return handler_error(req, "400 Bad Request", err);
    }
    return handler_ok(req);
}

static bool parse_addr_param(const char *s, uint8_t out[BLE_ADDR_LEN]) {
    unsigned b[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

// GET /api/ble/device?a=AA:BB:CC:DD:EE:FF&t=<addr type> -- one device with its
// last payloads and RSSI history (oldest first: r = mean RSSI per second,
// null = not heard; c = packets in that second).
static esp_err_t handler_ble_device(httpd_req_t *req) {
    char query[64], a[24] = "", t[6] = "0";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "a", a, sizeof(a));
        httpd_query_key_value(query, "t", t, sizeof(t));
    }
    // %3A-encoded colons come through literally from fetch(); accept both.
    char addr_txt[24];
    size_t o = 0;
    for (size_t i = 0; a[i] && o + 1 < sizeof(addr_txt); i++) {
        if (a[i] == '%' && a[i + 1] == '3' && (a[i + 2] == 'A' || a[i + 2] == 'a')) {
            addr_txt[o++] = ':';
            i += 2;
        } else {
            addr_txt[o++] = a[i];
        }
    }
    addr_txt[o] = '\0';
    uint8_t addr[BLE_ADDR_LEN];
    if (!parse_addr_param(addr_txt, addr)) {
        return handler_error(req, "400 Bad Request", "bad address");
    }
    ble_device_t *d = heap_caps_malloc(sizeof(ble_device_t), MALLOC_CAP_SPIRAM);
    if (d == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    uint32_t sec = 0;
    if (!ble_scanner_get_device(addr, (uint8_t)atoi(t), d, &sec)) {
        free(d);
        return handler_error(req, "404 Not Found", "device not known (any more)");
    }
    int64_t now = esp_timer_get_time();
    char *buf = malloc(4096);
    if (buf == NULL) {
        free(d);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    const size_t cap = 4096;
    char astr[18], name_esc[2 * BLE_NAME_MAX];
    ble_addr_str(d->addr, astr);
    json_escape(name_esc, sizeof(name_esc), d->name);
    size_t off = append(buf, cap, 0,
                        "{\"a\":\"%s\",\"t\":%u,\"k\":\"%s\",\"n\":\"%s\",\"nk\":%u,\"r\":%d,\"ra\":%.1f,"
                        "\"rn\":%d,\"rx\":%d,\"c\":%u,\"ps\":%u,\"f\":%lld,\"l\":%lld,\"m\":%d,\"ap\":%u,"
                        "\"u\":%u,\"tx\":%d,\"p\":%u,\"ph\":%u,\"tg\":%d,\"adv\":\"",
                        astr, d->addr_type, ble_addr_kind(d->addr_type, d->addr), name_esc, d->name_kind,
                        d->rssi_last, d->rssi_avg, d->rssi_min, d->rssi_max, (unsigned)d->count,
                        (unsigned)d->rate, (long long)((now - d->first_us) / 1000),
                        (long long)((now - d->last_us) / 1000),
                        d->company_id == 0xFFFF ? -1 : (int)d->company_id, (unsigned)d->appearance,
                        (unsigned)d->uuid16, d->tx_power, (unsigned)d->props_seen, (unsigned)d->phys_seen,
                        d->target ? 1 : 0);
    for (int i = 0; i < d->adv_len; i++) off = append(buf, cap, off, "%02X", d->adv_data[i]);
    off = append(buf, cap, off, "\",\"rsp\":\"");
    for (int i = 0; i < d->rsp_len; i++) off = append(buf, cap, off, "%02X", d->rsp_data[i]);
    off = append(buf, cap, off, "\",\"hr\":[");
    for (int i = 0; i < BLE_HIST_SECONDS; i++) {
        int8_t v = d->hist_rssi[(sec + 1 + i) % BLE_HIST_SECONDS];
        off = append(buf, cap, off, v == 127 ? "%snull" : "%s%d", i ? "," : "", (int)v);
    }
    off = append(buf, cap, off, "],\"hc\":[");
    for (int i = 0; i < BLE_HIST_SECONDS; i++) {
        off = append(buf, cap, off, "%s%u", i ? "," : "", d->hist_cnt[(sec + 1 + i) % BLE_HIST_SECONDS]);
    }
    off = append(buf, cap, off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    free(d);
    return ESP_OK;
}

// GET /api/ble/history -- reports/s and devices heard per second, oldest first.
static esp_err_t handler_ble_history(httpd_req_t *req) {
    uint16_t *rps = malloc(sizeof(uint16_t) * BLE_GHIST_SECONDS * 2);
    char *buf = malloc(BLE_GHIST_SECONDS * 14 + 64);
    if (rps == NULL || buf == NULL) {
        free(rps);
        free(buf);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    uint16_t *dev = rps + BLE_GHIST_SECONDS;
    int n = ble_scanner_get_history(rps, dev, BLE_GHIST_SECONDS);
    const size_t cap = BLE_GHIST_SECONDS * 14 + 64;
    size_t off = append(buf, cap, 0, "{\"rps\":[");
    for (int i = 0; i < n; i++) off = append(buf, cap, off, "%s%u", i ? "," : "", rps[i]);
    off = append(buf, cap, off, "],\"dev\":[");
    for (int i = 0; i < n; i++) off = append(buf, cap, off, "%s%u", i ? "," : "", dev[i]);
    off = append(buf, cap, off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    free(rps);
    return ESP_OK;
}

// Places a marker at "now" (= the moment of this request = the user's click).
// The note is added separately afterwards, so it never moves the position.
static esp_err_t handler_record_mark(httpd_req_t *req) {
    int id = recorder_marker_add(0);
    if (id < 0) {
        return handler_error(req, "409 Conflict", "not recording, or marker limit reached");
    }
    char body[32];
    snprintf(body, sizeof(body), "{\"id\":%d}", id);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t handler_record_mark_note(httpd_req_t *req) {
    char body[160];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int id = 0;
    if (!json_extract_int(json_find_value(body, "id"), &id)) {
        return handler_error(req, "400 Bad Request", "missing id");
    }
    char note[64] = "";
    json_extract_string(json_find_value(body, "note"), note, sizeof(note)); // optional
    if (!recorder_marker_set_note((uint32_t)id, note)) {
        return handler_error(req, "404 Not Found", "no such marker");
    }
    return handler_ok(req);
}

static esp_err_t handler_record_mark_delete(httpd_req_t *req) {
    char body[64];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int id = 0;
    if (!json_extract_int(json_find_value(body, "id"), &id)) {
        return handler_error(req, "400 Bad Request", "missing id");
    }
    if (!recorder_marker_delete((uint32_t)id)) {
        return handler_error(req, "404 Not Found", "no such marker");
    }
    return handler_ok(req);
}

static esp_err_t handler_record_markers(httpd_req_t *req) {
    recorder_marker_t *marks = malloc(64 * sizeof(recorder_marker_t));
    if (marks == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int n = recorder_marker_list(marks, 64);
    size_t cap = 64 * 160 + 16;
    char *buf = malloc(cap);
    if (buf == NULL) {
        free(marks);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    size_t off = append(buf, cap, 0, "[");
    for (int i = 0; i < n; i++) {
        char note_esc[96];
        json_escape(note_esc, sizeof(note_esc), marks[i].note);
        off = append(buf, cap, off,
                     "%s{\"id\":%u,\"t_s\":%.1f,\"unix\":%lld,\"ago_ms\":%lld,\"note\":\"%s\"}",
                     i ? "," : "", (unsigned)marks[i].id, marks[i].elapsed_s, (long long)marks[i].wall_unix_s,
                     (long long)((esp_timer_get_time() - marks[i].at_us) / 1000), note_esc);
    }
    off = append(buf, cap, off, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    free(marks);
    return ESP_OK;
}

static esp_err_t handler_settings_button_mark(httpd_req_t *req) {
    char body[48];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int enabled = 0;
    if (!json_extract_int(json_find_value(body, "enabled"), &enabled)) {
        return handler_error(req, "400 Bad Request", "missing enabled");
    }
    power_ctrl_set_button_mark(enabled != 0);
    return handler_ok(req);
}

// --- BLE scanner ----------------------------------------------------------

#define BLE_LIST_MAX 1024 // devices per /api/ble/devices response

// GET /api/ble/devices?age=<seconds> -- devices seen within `age` s (default
// 60, 0 = all known). Compact keys, since a busy place can list hundreds:
//   a addr, t addr type (0 public, 1 random), k address kind, n name,
//   r/ra/rn/rx RSSI last/avg/min/max,
//   c reports, ps reports in the last second, f/l first/last seen (ms ago),
//   m company id (-1 none), ap appearance, u first 16-bit service UUID,
//   tx TX power (127 unknown), p props bits, ph PHY bits, tg target.
// Streamed in chunks from a PSRAM snapshot, so the table lock is held only
// for the copy, never while sending.
static esp_err_t handler_ble_devices(httpd_req_t *req) {
    char query[32], val[12];
    uint32_t age_s = 60;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "age", val, sizeof(val)) == ESP_OK) {
        age_s = (uint32_t)strtoul(val, NULL, 10);
    }
    ble_device_t *devs = heap_caps_malloc(sizeof(ble_device_t) * BLE_LIST_MAX, MALLOC_CAP_SPIRAM);
    if (devs == NULL) {
        return handler_error(req, "503 Service Unavailable", "out of memory");
    }
    int64_t now_us = 0;
    int n = ble_scanner_snapshot(devs, BLE_LIST_MAX, age_s * 1000, &now_us);

    httpd_resp_set_type(req, "application/json");
    char chunk[2048];
    size_t off = append(chunk, sizeof(chunk), 0, "{\"n\":%d,\"devices\":[", n);
    for (int i = 0; i < n; i++) {
        const ble_device_t *d = &devs[i];
        char addr[18], name_esc[2 * BLE_NAME_MAX];
        ble_addr_str(d->addr, addr);
        json_escape(name_esc, sizeof(name_esc), d->name);
        char item[400];
        snprintf(item, sizeof(item),
                 "%s{\"a\":\"%s\",\"t\":%u,\"k\":\"%s\",\"n\":\"%s\",\"r\":%d,\"ra\":%.1f,\"rn\":%d,"
                 "\"rx\":%d,\"c\":%u,\"ps\":%u,\"f\":%lld,\"l\":%lld,\"m\":%d,\"ap\":%u,"
                 "\"u\":%u,\"tx\":%d,\"p\":%u,\"ph\":%u,\"tg\":%d}",
                 i == 0 ? "" : ",", addr, (unsigned)d->addr_type, ble_addr_kind(d->addr_type, d->addr), name_esc,
                 d->rssi_last, d->rssi_avg, d->rssi_min, d->rssi_max, (unsigned)d->count,
                 (unsigned)d->rate, (long long)((now_us - d->first_us) / 1000),
                 (long long)((now_us - d->last_us) / 1000),
                 d->company_id == 0xFFFF ? -1 : (int)d->company_id, (unsigned)d->appearance,
                 (unsigned)d->uuid16, d->tx_power, (unsigned)d->props_seen, (unsigned)d->phys_seen,
                 d->target ? 1 : 0);
        size_t il = strlen(item);
        if (off + il >= sizeof(chunk)) {
            if (httpd_resp_send_chunk(req, chunk, off) != ESP_OK) {
                free(devs);
                return ESP_FAIL; // client went away
            }
            off = 0;
        }
        memcpy(chunk + off, item, il);
        off += il;
    }
    free(devs);
    off = append(chunk, sizeof(chunk), off, "]}");
    httpd_resp_send_chunk(req, chunk, off);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// POST /api/ble/config {"active":0|1, "coded":0|1} -- either key optional.
static esp_err_t handler_ble_config(httpd_req_t *req) {
    char body[64];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int v = 0;
    if (json_extract_int(json_find_value(body, "active"), &v)) {
        ble_scanner_set_active(v != 0);
    }
    if (json_extract_int(json_find_value(body, "coded"), &v)) {
        ble_scanner_set_coded(v != 0);
    }
    return handler_ok(req);
}

// Firmware update: the request body is the raw firmware.bin, streamed straight
// into the inactive OTA slot. Runs inline on the httpd task (blocking the
// dashboard for the ~20-30s transfer, which is fine for a deliberate update the
// user is watching) -- the browser shows upload progress, and the device
// reboots into the new image on success. The reusable core is in ota.c, so a
// pull-from-URL path later would reuse ota_begin/write/end unchanged.
#define OTA_RECV_CHUNK 4096
static esp_err_t handler_ota_upload(httpd_req_t *req) {
    int content_len = req->content_len;
    if (content_len <= 0) {
        return handler_error(req, "400 Bad Request", "empty upload (no Content-Length)");
    }
    if (!ota_begin((uint32_t)content_len)) {
        ota_status_t st;
        ota_get_status(&st);
        return handler_error(req, "409 Conflict", st.error[0] ? st.error : "cannot start update");
    }
    char *buf = malloc(OTA_RECV_CHUNK);
    if (buf == NULL) {
        ota_abort();
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int remaining = content_len;
    bool ok = true;
    while (remaining > 0) {
        int want = remaining < OTA_RECV_CHUNK ? remaining : OTA_RECV_CHUNK;
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) {
            ok = false; // timeout / connection dropped
            break;
        }
        if (!ota_write(buf, (size_t)r)) {
            ok = false;
            break;
        }
        remaining -= r;
    }
    free(buf);
    if (!ok) {
        ota_abort();
        ota_status_t st;
        ota_get_status(&st);
        return handler_error(req, "400 Bad Request", st.error[0] ? st.error : "upload failed");
    }
    if (!ota_end()) {
        ota_status_t st;
        ota_get_status(&st);
        return handler_error(req, "400 Bad Request", st.error[0] ? st.error : "invalid firmware image");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ota_schedule_reboot();
    return ESP_OK;
}

// --- Internet firmware updates (ota_cloud.c) -------------------------------
// Check the configured HTTPS endpoint for a newer build, then optionally pull +
// install it. The manual upload above stays as-is; this is an additional source.

static esp_err_t handler_ota_check(httpd_req_t *req) {
    ota_cloud_check_t r;
    ota_cloud_check(&r); // synchronous GET /version (a few seconds at most)
    char latest_esc[32], cur_esc[32], err_esc[144];
    json_escape(latest_esc, sizeof(latest_esc), r.latest);
    json_escape(cur_esc, sizeof(cur_esc), r.current);
    json_escape(err_esc, sizeof(err_esc), r.error);
    char out[360];
    int n = snprintf(out, sizeof(out),
                     "{\"configured\":%s,\"available\":%s,\"current\":\"%s\",\"latest\":\"%s\","
                     "\"size\":%u,\"error\":\"%s\"}",
                     r.configured ? "true" : "false", r.update_available ? "true" : "false",
                     cur_esc, latest_esc, (unsigned)r.size, err_esc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, n);
    return ESP_OK;
}

static esp_err_t handler_ota_install(httpd_req_t *req) {
    char err[96] = "";
    if (!ota_cloud_start_install(err, sizeof(err))) {
        return handler_error(req, "409 Conflict", err[0] ? err : "cannot start update");
    }
    return handler_ok(req); // progress + result then come via /api/status (ota block)
}

static esp_err_t handler_ota_server_get(httpd_req_t *req) {
    char url[OTA_CLOUD_URL_MAX], id[OTA_CLOUD_ID_MAX];
    bool has_secret = false;
    ota_cloud_get_config(url, sizeof(url), id, sizeof(id), &has_secret);
    char url_esc[192], id_esc[160];
    json_escape(url_esc, sizeof(url_esc), url);
    json_escape(id_esc, sizeof(id_esc), id);
    char out[420];
    int n = snprintf(out, sizeof(out), "{\"url\":\"%s\",\"client_id\":\"%s\",\"has_secret\":%s}",
                     url_esc, id_esc, has_secret ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, n);
    return ESP_OK;
}

static esp_err_t handler_ota_server_set(httpd_req_t *req) {
    char body[512];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char url[OTA_CLOUD_URL_MAX] = "", id[OTA_CLOUD_ID_MAX] = "", secret[OTA_CLOUD_SECRET_MAX] = "";
    json_extract_string(json_find_value(body, "url"), url, sizeof(url));
    json_extract_string(json_find_value(body, "client_id"), id, sizeof(id));
    json_extract_string(json_find_value(body, "client_secret"), secret, sizeof(secret));
    if (!ota_cloud_set_config(url, id, secret)) {
        return handler_error(req, "500 Internal Server Error", "could not save settings");
    }
    return handler_ok(req);
}

static void register_handlers(httpd_handle_t server) {
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_index},
        {.uri = "/ws", .method = HTTP_GET, .handler = handler_ws, .is_websocket = true},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handler_status},
        {.uri = "/api/ble/devices", .method = HTTP_GET, .handler = handler_ble_devices},
        {.uri = "/api/ble/config", .method = HTTP_POST, .handler = handler_ble_config},
        {.uri = "/api/ble/device", .method = HTTP_GET, .handler = handler_ble_device},
        {.uri = "/api/ble/history", .method = HTTP_GET, .handler = handler_ble_history},
        {.uri = "/api/log/config", .method = HTTP_POST, .handler = handler_log_config},
        {.uri = "/api/targets", .method = HTTP_GET, .handler = handler_targets_get},
        {.uri = "/api/targets", .method = HTTP_POST, .handler = handler_targets_set},
        {.uri = "/api/motion/config", .method = HTTP_POST, .handler = handler_motion_config},
        {.uri = "/api/motion/history", .method = HTTP_GET, .handler = handler_motion_history},
        {.uri = "/api/accel/selftest", .method = HTTP_POST, .handler = handler_accel_selftest},
        {.uri = "/api/record/start", .method = HTTP_POST, .handler = handler_record_start},
        {.uri = "/api/record/stop", .method = HTTP_POST, .handler = handler_record_stop},
        {.uri = "/api/record/mark", .method = HTTP_POST, .handler = handler_record_mark},
        {.uri = "/api/record/mark/note", .method = HTTP_POST, .handler = handler_record_mark_note},
        {.uri = "/api/record/mark/delete", .method = HTTP_POST, .handler = handler_record_mark_delete},
        {.uri = "/api/record/markers", .method = HTTP_GET, .handler = handler_record_markers},
        {.uri = "/api/settings/button_mark", .method = HTTP_POST, .handler = handler_settings_button_mark},
        {.uri = "/api/settings/mdns_friendly", .method = HTTP_POST, .handler = handler_mdns_friendly},
        {.uri = "/api/settings/report_enabled", .method = HTTP_POST, .handler = handler_report_enabled},
        {.uri = "/api/sync/edges", .method = HTTP_GET, .handler = handler_sync_edges},
        {.uri = "/api/sync/reset", .method = HTTP_POST, .handler = handler_sync_reset},
        {.uri = "/api/ota/upload", .method = HTTP_POST, .handler = handler_ota_upload},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = handler_ota_check},
        {.uri = "/api/ota/install", .method = HTTP_POST, .handler = handler_ota_install},
        {.uri = "/api/ota/server", .method = HTTP_GET, .handler = handler_ota_server_get},
        {.uri = "/api/ota/server", .method = HTTP_POST, .handler = handler_ota_server_set},
        {.uri = "/api/sdbench/run", .method = HTTP_POST, .handler = handler_sdbench_run},
        {.uri = "/api/charger/ship", .method = HTTP_POST, .handler = handler_charger_ship},
        {.uri = "/api/charger/ts_ignore", .method = HTTP_POST, .handler = handler_charger_ts_ignore},
        {.uri = "/api/battery/reset", .method = HTTP_GET, .handler = handler_battery_reset},
        {.uri = "/api/party", .method = HTTP_POST, .handler = handler_party},
        {.uri = "/api/led/test", .method = HTTP_POST, .handler = handler_led_test},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handler_wifi_connect},
        {.uri = "/api/wifi/select", .method = HTTP_POST, .handler = handler_wifi_select},
        {.uri = "/api/wifi/forget", .method = HTTP_POST, .handler = handler_wifi_forget},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handler_wifi_scan},
        {.uri = "/api/wifi/aps", .method = HTTP_GET, .handler = handler_wifi_aps},
        {.uri = "/api/wifi/pin", .method = HTTP_POST, .handler = handler_wifi_pin},
        {.uri = "/api/wifi/pin/clear", .method = HTTP_POST, .handler = handler_wifi_pin_clear},
        {.uri = "/api/time/set", .method = HTTP_POST, .handler = handler_time_set},
        {.uri = "/api/sdcard/remount", .method = HTTP_POST, .handler = handler_sdcard_remount},
        {.uri = "/api/recordings", .method = HTTP_GET, .handler = handler_recordings_list},
        {.uri = "/api/recordings/delete", .method = HTTP_POST, .handler = handler_recordings_delete},
        {.uri = "/api/recordings/rename", .method = HTTP_POST, .handler = handler_recordings_rename},
        {.uri = "/api/download", .method = HTTP_GET, .handler = handler_download},
        {.uri = "/api/udp/start", .method = HTTP_POST, .handler = handler_udp_start},
        {.uri = "/api/udp/stop", .method = HTTP_POST, .handler = handler_udp_stop},
        {.uri = "/live", .method = HTTP_GET, .handler = handler_live},
        {.uri = "/wsstream", .method = HTTP_GET, .handler = handler_wsstream, .is_websocket = true},
        {.uri = "/api/wsstream/start", .method = HTTP_POST, .handler = handler_wsstream_start},
        {.uri = "/api/wsstream/stop", .method = HTTP_POST, .handler = handler_wsstream_stop},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

void dashboard_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 12288; // headroom for the TLS handshake in the /version check
    config.max_uri_handlers = 60; // headroom above the ~54 currently registered (cheap: a pointer array)
    config.max_open_sockets = MAX_WS_CLIENTS;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        // Most likely internal RAM for the server task ran out. Say so before
        // the abort below (which keeps OTA rollback protection).
        printf("dashboard: httpd_start failed (%s) -- internal heap free %u, largest block %u\n",
               esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    ESP_ERROR_CHECK(err);
    register_handlers(s_server);
    ws_stream_set_server(s_server);

    // Dedicated worker for file downloads so a long transfer never blocks the
    // single httpd task (see handler_download). Pinned to core 0 (networking
    // side), off the real-time capture core.
    s_download_queue = xQueueCreate(MAX_CONCURRENT_DOWNLOADS, sizeof(httpd_req_t *));
    xTaskCreatePinnedToCore(download_worker_task, "http_dl", 8192, NULL, 5, NULL, 0);

    xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 6, NULL);

    printf("\n=============================================================\n");
    printf(" Recorder dashboard running -- http://192.168.4.1/\n");
    printf("=============================================================\n");
}
