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
#include "csv_export.h"
#include "dashboard_html.h"
#include "sync_trigger.h"
#include "device_id.h"
#include "device_report.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
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
#include "recording_format.h"
#include "sd_benchmark.h"
#include "udp_stream.h"
#include "version.h"
#include "wifi_manager.h"
#include "ws_stream.h"

#define REC_DIR "/sdcard/rec"

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

static const char *csv_export_state_str(csv_export_state_t s) {
    switch (s) {
        case CSV_EXPORT_IDLE: return "idle";
        case CSV_EXPORT_RUNNING: return "running";
        case CSV_EXPORT_DONE: return "done";
        case CSV_EXPORT_FAILED: return "failed";
        case CSV_EXPORT_CANCELLED: return "cancelled";
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
    static char buf[4096];
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
    float clip[3];
    accel_capture_get_clip_pct(clip);
    off = append(buf, cap, off,
                 "\"accel\":{\"ready\":%s,\"fs_g\":%.0f,\"lpf2\":%s,"
                 "\"filter\":%d,\"cutoff\":%d,\"cutoff_hz\":%d,"
                 "\"sensor_overruns\":%u,\"ring_drops\":%u,\"decim_rate_hz\":%.1f,"
                 "\"measured_odr_hz\":%.1f,\"overrun_lost\":%u,\"samples_total\":%llu,"
                 "\"temp_c\":%.1f,\"temp_valid\":%s,\"remember_config\":%s,"
                 "\"clip\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}},",
                 accel_capture_is_ready() ? "true" : "false",
                 (double)fs_enum_to_g(accel_capture_get_fs()),
                 accel_capture_get_lpf2() ? "true" : "false",
                 (int)accel_capture_get_filter_mode(), (int)accel_capture_get_hpcf(),
                 iis3dwb_hpcf_cutoff_hz(accel_capture_get_hpcf()),
                 (unsigned)accel_capture_get_sensor_overruns(),
                 (unsigned)accel_capture_get_ring_drops(),
                 (double)accel_capture_get_recording_decim_rate_hz(),
                 (double)accel_capture_get_measured_odr_hz(),
                 (unsigned)accel_capture_get_overrun_lost_samples(),
                 (unsigned long long)accel_capture_get_samples_total(),
                 (double)accel_temp_c, accel_temp_valid ? "true" : "false",
                 accel_capture_get_remember_config() ? "true" : "false",
                 (double)clip[0], (double)clip[1], (double)clip[2]);

    accel_selftest_result_t st;
    accel_capture_self_test_get_result(&st);
    off = append(buf, cap, off,
                 "\"selftest\":{\"state\":\"%s\",\"x_mg\":%.0f,\"y_mg\":%.0f,\"z_mg\":%.0f,"
                 "\"min_mg\":%.0f,\"max_mg\":%.0f},",
                 accel_selftest_state_str(st.state), (double)st.change_mg_x, (double)st.change_mg_y,
                 (double)st.change_mg_z, 800.0, 3200.0);

    char fname_esc[160];
    json_escape(fname_esc, sizeof(fname_esc), recorder_get_current_filename());
    char rec_err_esc[80];
    json_escape(rec_err_esc, sizeof(rec_err_esc), recorder_get_last_error());
    char rec_notice_esc[80];
    json_escape(rec_notice_esc, sizeof(rec_notice_esc), recorder_get_last_notice());
    off = append(buf, cap, off,
                 "\"recording\":{\"active\":%s,\"filename\":\"%s\",\"bytes_written\":%llu,"
                 "\"rate_hz\":%.1f,\"markers\":%d,\"last_error\":\"%s\",\"notice\":\"%s\"},",
                 recorder_is_recording() ? "true" : "false", fname_esc,
                 (unsigned long long)recorder_get_bytes_written(),
                 (double)recorder_get_current_sample_rate_hz(), recorder_marker_count(), rec_err_esc,
                 rec_notice_esc);

    off = append(buf, cap, off, "\"settings\":{\"button_mark\":%s,\"auto_range\":%s},",
                 power_ctrl_get_button_mark() ? "true" : "false",
                 recorder_get_auto_range() ? "true" : "false");

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

    csv_export_result_t exp;
    csv_export_get_result(&exp);
    char exp_fname_esc[80], exp_err_esc[80];
    json_escape(exp_fname_esc, sizeof(exp_fname_esc), exp.filename);
    json_escape(exp_err_esc, sizeof(exp_err_esc), exp.error);
    char resume_name[64];
    char resume_esc[80];
    json_escape(resume_esc, sizeof(resume_esc),
                csv_export_get_pending(resume_name, sizeof(resume_name)) ? resume_name : "");
    off = append(buf, cap, off,
                 "\"csvexport\":{\"state\":\"%s\",\"filename\":\"%s\",\"total_samples\":%llu,"
                 "\"samples_done\":%llu,\"eta_s\":%d,\"resume_file\":\"%s\",\"error\":\"%s\"},",
                 csv_export_state_str(exp.state), exp_fname_esc,
                 (unsigned long long)exp.total_samples, (unsigned long long)exp.samples_done,
                 (int)exp.eta_s, resume_esc, exp_err_esc);

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

static esp_err_t handler_accel_config(httpd_req_t *req) {
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    if (recorder_is_recording()) {
        return handler_error(req, "409 Conflict", "recording in progress");
    }

    int fs_g = (int)fs_enum_to_g(accel_capture_get_fs());
    int filter = (int)accel_capture_get_filter_mode(); // 0 off, 1 low-pass, 2 high-pass
    int cutoff = (int)accel_capture_get_hpcf();         // 0..7 (HPCF_XL)
    json_extract_int(json_find_value(body, "fs_g"), &fs_g);
    bool has_filter = json_extract_int(json_find_value(body, "filter"), &filter);
    json_extract_int(json_find_value(body, "cutoff"), &cutoff);
    // Backward compatibility with an older client that only sends the lpf2 bool.
    bool lpf2 = false;
    if (!has_filter && json_extract_bool(json_find_value(body, "lpf2"), &lpf2)) {
        filter = lpf2 ? (int)IIS3DWB_FILTER_LOWPASS : (int)IIS3DWB_FILTER_OFF;
    }
    if (filter < 0 || filter > (int)IIS3DWB_FILTER_HIGHPASS) filter = (int)IIS3DWB_FILTER_OFF;
    if (cutoff < 0 || cutoff > (int)IIS3DWB_HPCF_ODR_800) cutoff = (int)IIS3DWB_HPCF_ODR_4;

    iis3dwb_fs_t fs;
    switch (fs_g) {
        case 4: fs = IIS3DWB_FS_4G; break;
        case 8: fs = IIS3DWB_FS_8G; break;
        case 16: fs = IIS3DWB_FS_16G; break;
        default: fs = IIS3DWB_FS_2G; break;
    }

    if (!accel_capture_set_config(fs, (iis3dwb_filter_mode_t)filter, (iis3dwb_hpcf_t)cutoff)) {
        return handler_error(req, "500 Internal Server Error", "sensor config write failed");
    }
    return handler_ok(req);
}

static esp_err_t handler_accel_remember(httpd_req_t *req) {
    char body[64];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    bool enabled = false;
    if (!json_extract_bool(json_find_value(body, "enabled"), &enabled)) {
        return handler_error(req, "400 Bad Request", "missing enabled");
    }
    accel_capture_set_remember_config(enabled);
    return handler_ok(req);
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
    bool decimated = false;
    if (read_body(req, body, sizeof(body))) {
        json_extract_string(json_find_value(body, "name"), name, sizeof(name));
        json_extract_bool(json_find_value(body, "decimated"), &decimated);
    }
    if (!recorder_request_start(name, decimated)) {
        return handler_error(req, "409 Conflict", "already recording (or SD not mounted)");
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

// Serves /sdcard/rec/<file> (raw .bin or a previously-exported .csv) as a
// chunked download -- files can be much larger than available RAM, so this
// streams from the SD card rather than buffering the whole thing. Also the
// fetch() target for client-side (in-browser) CSV conversion: the browser
// downloads the raw .bin via this same endpoint and parses it itself, since
// a modern browser's JS engine does that conversion far faster than the
// ESP32 can.
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
    uint32_t last_drops = accel_capture_get_ring_drops();
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
                uint32_t drops = accel_capture_get_ring_drops();
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
    // worker gate entirely (big .bin/.csv downloads still go through it).
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

// Returns the part number (>=2) if `name` is a rotation part file
// ("<base>_part<N>.bin"), else 0. Part files are absorbed into their base
// recording, not listed on their own.
static int bin_part_number(const char *name) {
    size_t nlen = strlen(name);
    if (nlen < 4 || strcmp(name + nlen - 4, ".bin") != 0) {
        return 0;
    }
    const char *last = NULL;
    for (const char *s = name; (s = strstr(s, "_part")) != NULL; s++) {
        last = s;
    }
    if (last == NULL) {
        return 0;
    }
    const char *digits = last + 5;
    const char *end = name + nlen - 4; // before ".bin"
    if (digits >= end) {
        return 0;
    }
    for (const char *c = digits; c < end; c++) {
        if (!isdigit((unsigned char)*c)) {
            return 0;
        }
    }
    return atoi(digits);
}

// Reduces any recording filename to its grouping base: "rec_7_part2.bin" and
// "rec_7.bin" both -> "rec_7". Also accepts a bare base unchanged.
static void recording_base(const char *name, char *out, size_t outlen) {
    snprintf(out, outlen, "%s", name);
    size_t n = strlen(out);
    if (n > 4 && strcmp(out + n - 4, ".bin") == 0) {
        out[n - 4] = '\0';
    }
    char *last = NULL, *s = out;
    while ((s = strstr(s, "_part")) != NULL) {
        last = s;
        s++;
    }
    if (last != NULL && last[5] != '\0') {
        bool all_digits = true;
        for (char *c = last + 5; *c; c++) {
            if (!isdigit((unsigned char)*c)) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            *last = '\0';
        }
    }
}

static bool read_recording_header(const char *path, recording_header_t *hdr) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = fread(hdr, sizeof(*hdr), 1, f) == 1 && memcmp(hdr->magic, RECORDING_MAGIC, 4) == 0;
    fclose(f);
    return ok;
}

typedef struct {
    bool have;
    int lpf2;          // -1 unknown, 0 off, 1 on
    char filter[12];   // "off"/"lowpass"/"highpass" ("" if unknown / older log)
    int filter_hz;     // second-stage cutoff in Hz (0 if off/unknown)
    float temp_start;  // NaN if unknown
    float temp_end;
    long overruns_start; // cumulative FIFO-overrun counter at recording start (-1 unknown)
    long overruns;       // ...and at end; the badge shows the difference (overruns *during* this recording)
    long drops_start;
    long drops;
    float clip_pct; // worst 1s-window clip% (last part's cumulative max), NaN if absent
    int clipped;    // 1 = sustained clipping occurred, 0 = not, -1 = unknown
} rec_log_t;

static float log_first_float(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    return p ? strtof(p + strlen(key), NULL) : NAN;
}
static float log_last_float(const char *buf, const char *key) {
    const char *p = buf, *last = NULL;
    size_t kl = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        last = p;
        p += kl;
    }
    return last ? strtof(last + kl, NULL) : NAN;
}
static long log_last_long(const char *buf, const char *key) {
    const char *p = buf, *last = NULL;
    size_t kl = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        last = p;
        p += kl;
    }
    return last ? strtol(last + kl, NULL, 10) : -1;
}
static long log_first_long(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

// Parses one recording's tiny text .log. `overruns`/`drops`/`temp_end` take
// their final (last) values; `temp_start`/`lpf2` the first (start line).
static void parse_recording_log(const char *log_path, rec_log_t *out) {
    FILE *f = fopen(log_path, "rb");
    if (f == NULL) {
        return;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    out->have = true;
    const char *lp = strstr(buf, "lpf2=");
    if (lp != NULL) {
        out->lpf2 = (strncmp(lp + 5, "on", 2) == 0) ? 1 : 0;
    }
    const char *fp = strstr(buf, "filter="); // "filter=" won't match "filter_hz="
    if (fp != NULL) {
        fp += 7;
        size_t i = 0;
        while (fp[i] && fp[i] != ' ' && fp[i] != '\n' && i < sizeof(out->filter) - 1) {
            out->filter[i] = fp[i];
            i++;
        }
        out->filter[i] = '\0';
    }
    out->filter_hz = (int)log_last_long(buf, "filter_hz=");
    out->temp_start = log_first_float(buf, "temp_c=");
    out->temp_end = log_last_float(buf, "temp_c=");
    out->overruns_start = log_first_long(buf, "overruns=");
    out->overruns = log_last_long(buf, "overruns=");
    out->drops_start = log_first_long(buf, "drops=");
    out->drops = log_last_long(buf, "drops=");
    out->clip_pct = log_last_float(buf, "clip_pct=");
    out->clipped = (int)log_last_long(buf, "clipped=");
}

// One JSON object per recording (rotation parts grouped under their base),
// streamed as chunks so any number of recordings fits. Each carries enough for
// the dashboard's recording cards: identity + decoded header metadata + the
// integrity/temperature/LPF2 read from its .log.
static esp_err_t handler_recordings_list(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    DIR *d = opendir(REC_DIR);
    if (d == NULL) {
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        if (nlen < 4 || strcmp(entry->d_name + nlen - 4, ".bin") != 0) {
            continue;
        }
        if (bin_part_number(entry->d_name) != 0) {
            continue; // rotation part -- folded into its base below
        }

        char base[128];
        snprintf(base, sizeof(base), "%.*s", (int)(nlen - 4), entry->d_name); // strip ".bin"

        uint64_t total_size = 0;
        uint64_t total_samples = 0;
        int parts = 0;
        char first_bin[300] = "", last_log[300] = "";
        char fs_list_json[160] = "["; // per-part full-scale ranges, e.g. [2,8,16]
        size_t fj = 1;
        bool fs_first = true;
        char size_list_json[160] = "["; // per-part sizes in bytes, e.g. [3758096384,...]
        size_t sj = 1;
        bool size_first = true;
        for (int pn = 1;; pn++) {
            char pbin[300];
            if (pn == 1) {
                snprintf(pbin, sizeof(pbin), "%s/%s.bin", REC_DIR, base);
            } else {
                snprintf(pbin, sizeof(pbin), "%s/%s_part%d.bin", REC_DIR, base, pn);
            }
            struct stat pst;
            if (stat(pbin, &pst) != 0) {
                break;
            }
            parts++;
            // st_size is a 32-bit off_t on this build, so a 2-4GB file wraps
            // negative -- but the low 32 bits are the true size (FATFS caps
            // files at 4GB), so read it back unsigned. Accumulate in 64-bit for
            // multi-part totals.
            uint32_t psize = (uint32_t)pst.st_size;
            total_size += psize;
            recording_header_t phdr;
            bool have_phdr = read_recording_header(pbin, &phdr);
            // Sample data starts after the header + the (v3) sync-checkpoint table;
            // a v2 file has sync_table_bytes == 0, so this stays correct for both.
            uint32_t data_off = (uint32_t)sizeof(recording_header_t) +
                                (have_phdr ? phdr.sync_table_bytes : 0);
            if (psize > data_off) {
                total_samples += (uint64_t)(psize - data_off) / 6;
            }
            if (sj < sizeof(size_list_json) - 12) {
                sj += snprintf(size_list_json + sj, sizeof(size_list_json) - sj, "%s%u",
                               size_first ? "" : ",", (unsigned)psize);
                size_first = false;
            }
            if (have_phdr && fj < sizeof(fs_list_json) - 12) {
                fj += snprintf(fs_list_json + fj, sizeof(fs_list_json) - fj, "%s%d",
                               fs_first ? "" : ",", (int)phdr.fs_range_g);
                fs_first = false;
            }
            if (pn == 1) {
                snprintf(first_bin, sizeof(first_bin), "%s", pbin);
                snprintf(last_log, sizeof(last_log), "%s/%s.log", REC_DIR, base);
            } else {
                snprintf(last_log, sizeof(last_log), "%s/%s_part%d.log", REC_DIR, base, pn);
            }
        }
        if (parts == 0) {
            continue; // vanished between readdir and stat
        }
        snprintf(fs_list_json + fj, sizeof(fs_list_json) - fj, "]");
        snprintf(size_list_json + sj, sizeof(size_list_json) - sj, "]");

        recording_header_t hdr;
        bool have_hdr = read_recording_header(first_bin, &hdr);
        double fs_g = have_hdr ? hdr.fs_range_g : 0;
        double rate_hz = have_hdr ? hdr.sample_rate_hz : 0;
        double sens_mg = have_hdr ? hdr.sensitivity_mg_per_lsb : 0;
        bool wall_valid = have_hdr && hdr.wall_time_valid;

        char date[24] = "";
        if (wall_valid) {
            time_t t = (time_t)hdr.wall_time_unix_s;
            struct tm tm_info;
            localtime_r(&t, &tm_info);
            strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm_info);
        }

        rec_log_t lg = {.lpf2 = -1, .temp_start = NAN, .temp_end = NAN, .overruns_start = -1,
                        .overruns = -1, .drops_start = -1, .drops = -1, .clip_pct = NAN, .clipped = -1};
        char first_log[300];
        snprintf(first_log, sizeof(first_log), "%s/%s.log", REC_DIR, base);
        parse_recording_log(first_log, &lg);
        if (parts > 1) {
            rec_log_t lg2 = {.lpf2 = -1, .temp_start = NAN, .temp_end = NAN, .overruns_start = -1,
                             .overruns = -1, .drops_start = -1, .drops = -1, .clip_pct = NAN, .clipped = -1};
            parse_recording_log(last_log, &lg2);
            if (lg2.have) {
                lg.temp_end = lg2.temp_end;
                lg.overruns = lg2.overruns; // end value comes from the last part's log
                lg.drops = lg2.drops;
                lg.clip_pct = lg2.clip_pct; // last part carries the recording's cumulative max
                lg.clipped = lg2.clipped;
            }
        }

        // Report overruns/drops that occurred *during this recording* (end minus
        // the start baseline of the cumulative-since-boot counters), not the raw
        // counter -- otherwise every recording inherits the couple of benign FIFO
        // overruns that happen at boot. -1 (unknown) if no baseline was logged.
        long ov_delta = (lg.overruns_start >= 0 && lg.overruns >= lg.overruns_start)
                            ? lg.overruns - lg.overruns_start
                            : -1;
        long dr_delta = (lg.drops_start >= 0 && lg.drops >= lg.drops_start)
                            ? lg.drops - lg.drops_start
                            : -1;

        char csv_path[300];
        snprintf(csv_path, sizeof(csv_path), "%s/%s.csv", REC_DIR, base);
        struct stat cst;
        bool has_csv = (stat(csv_path, &cst) == 0);
        long long csv_size = has_csv ? (long long)(uint32_t)cst.st_size : -1; // unsigned: see above

        char mark_path[300];
        snprintf(mark_path, sizeof(mark_path), "%s/%s.mark", REC_DIR, base);
        struct stat mst;
        bool has_mark = (stat(mark_path, &mst) == 0);

        // 1 Hz aux side-channel file (one per part; part 1's presence stands for all).
        char aux_path[300];
        snprintf(aux_path, sizeof(aux_path), "%s/%s.aux.csv", REC_DIR, base);
        struct stat ast;
        bool has_aux = (stat(aux_path, &ast) == 0);

        char id_esc[160], date_esc[48];
        json_escape(id_esc, sizeof(id_esc), base);
        json_escape(date_esc, sizeof(date_esc), date);

        char temp_start_s[16], temp_end_s[16], lpf2_s[8];
        if (isnan(lg.temp_start)) {
            strcpy(temp_start_s, "null");
        } else {
            snprintf(temp_start_s, sizeof(temp_start_s), "%.1f", (double)lg.temp_start);
        }
        if (isnan(lg.temp_end)) {
            strcpy(temp_end_s, "null");
        } else {
            snprintf(temp_end_s, sizeof(temp_end_s), "%.1f", (double)lg.temp_end);
        }
        strcpy(lpf2_s, lg.lpf2 < 0 ? "null" : (lg.lpf2 ? "true" : "false"));
        char filter_s[24];
        if (lg.filter[0] == '\0') {
            strcpy(filter_s, "null");
        } else {
            snprintf(filter_s, sizeof(filter_s), "\"%s\"", lg.filter);
        }

        char obj[1024];
        double clip_pct_v = isnan(lg.clip_pct) ? 0.0 : (double)lg.clip_pct;
        size_t ol = append(obj, sizeof(obj), 0,
                           "%s{\"id\":\"%s\",\"date\":\"%s\",\"parts\":%d,\"size\":%llu,"
                           "\"size_list\":%s,\"samples\":%llu,\"fs_g\":%.0f,\"fs_list\":%s,"
                           "\"rate_hz\":%.1f,\"sens_mg\":%.4f,\"lpf2\":%s,\"filter\":%s,"
                           "\"filter_hz\":%d,\"temp_start\":%s,"
                           "\"temp_end\":%s,\"overruns\":%ld,\"drops\":%ld,\"clip_pct\":%.2f,"
                           "\"clipped\":%s,\"wall_valid\":%s,\"has_csv\":%s,\"csv_size\":%lld,"
                           "\"has_log\":%s,\"has_mark\":%s,\"has_aux\":%s}",
                           first ? "" : ",", id_esc, date_esc, parts,
                           (unsigned long long)total_size, size_list_json,
                           (unsigned long long)total_samples, fs_g,
                           fs_list_json, rate_hz, sens_mg, lpf2_s, filter_s, lg.filter_hz,
                           temp_start_s, temp_end_s, ov_delta,
                           dr_delta, clip_pct_v, (lg.clipped == 1) ? "true" : "false",
                           wall_valid ? "true" : "false", has_csv ? "true" : "false", csv_size,
                           lg.have ? "true" : "false", has_mark ? "true" : "false",
                           has_aux ? "true" : "false");
        httpd_resp_send_chunk(req, obj, ol);
        first = false;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0); // terminate the chunked response
    return ESP_OK;
}

static esp_err_t handler_export_csv(httpd_req_t *req) {
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char filename[64] = "";
    if (!json_extract_string(json_find_value(body, "filename"), filename, sizeof(filename))) {
        return handler_error(req, "400 Bad Request", "missing filename");
    }
    if (!csv_export_start(filename)) {
        return handler_error(req, "409 Conflict",
                              "already exporting, recording active, or bad filename");
    }
    return handler_ok(req);
}

static esp_err_t handler_export_resume(httpd_req_t *req) {
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char filename[64] = "";
    if (!json_extract_string(json_find_value(body, "filename"), filename, sizeof(filename))) {
        return handler_error(req, "400 Bad Request", "missing filename");
    }
    if (!csv_export_resume(filename)) {
        return handler_error(req, "409 Conflict", "nothing pending, or already exporting");
    }
    return handler_ok(req);
}

static esp_err_t handler_export_discard(httpd_req_t *req) {
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char filename[64] = "";
    if (!json_extract_string(json_find_value(body, "filename"), filename, sizeof(filename))) {
        return handler_error(req, "400 Bad Request", "missing filename");
    }
    if (!csv_export_discard(filename)) {
        return handler_error(req, "409 Conflict", "nothing pending for that file");
    }
    return handler_ok(req);
}

// No body needed -- only one conversion runs at a time.
static esp_err_t handler_export_cancel(httpd_req_t *req) {
    if (!csv_export_cancel()) {
        return handler_error(req, "409 Conflict", "no conversion running");
    }
    return handler_ok(req);
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

// Deletes a recording's .bin plus any sidecar (.csv export, .log overrun
// log) it may have. Refuses to delete the file currently being written to.
static esp_err_t handler_recordings_delete(httpd_req_t *req) {
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char filename[128] = "";
    if (!json_extract_string(json_find_value(body, "filename"), filename, sizeof(filename))) {
        return handler_error(req, "400 Bad Request", "missing filename");
    }
    if (filename[0] == '\0' || strstr(filename, "..") != NULL || strchr(filename, '/') != NULL) {
        return handler_error(req, "400 Bad Request", "invalid filename");
    }

    // Delete the whole recording: every rotation part's .bin, plus its .csv,
    // .log, .mark, .aux.csv, and any interrupted-conversion .csv.tmp/.csv.prog.
    char base[128];
    recording_base(filename, base, sizeof(base));
    if (base[0] == '\0') {
        return handler_error(req, "400 Bad Request", "invalid filename");
    }

    // Refuse while this recording is mid-conversion (the converter holds a
    // part's .bin open) -- cancel first.
    csv_export_result_t exp;
    csv_export_get_result(&exp);
    if (exp.state == CSV_EXPORT_RUNNING) {
        char exp_base[128];
        recording_base(exp.filename, exp_base, sizeof(exp_base));
        if (strcmp(exp_base, base) == 0) {
            return handler_error(req, "409 Conflict", "conversion in progress for this recording");
        }
    }

    // Refuse if any part of it is the recording currently being written.
    const char *active = recorder_get_current_filename();
    if (active[0] != '\0') {
        const char *aname = strrchr(active, '/');
        aname = aname ? aname + 1 : active;
        char active_base[128];
        recording_base(aname, active_base, sizeof(active_base));
        if (strcmp(active_base, base) == 0) {
            return handler_error(req, "409 Conflict", "recording is currently active");
        }
    }

    bool any_deleted = false;
    for (int pn = 1;; pn++) {
        char stem[200];
        if (pn == 1) {
            snprintf(stem, sizeof(stem), "%s/%s", REC_DIR, base);
        } else {
            snprintf(stem, sizeof(stem), "%s/%s_part%d", REC_DIR, base, pn);
        }
        char p[220];
        snprintf(p, sizeof(p), "%s.bin", stem);
        struct stat st;
        bool bin_exists = (stat(p, &st) == 0);
        if (!bin_exists && pn > 1) {
            break; // no further parts
        }
        if (unlink(p) == 0) {
            any_deleted = true;
        }
        snprintf(p, sizeof(p), "%s.csv", stem);
        if (unlink(p) == 0) {
            any_deleted = true;
        }
        snprintf(p, sizeof(p), "%s.log", stem);
        if (unlink(p) == 0) {
            any_deleted = true;
        }
        snprintf(p, sizeof(p), "%s.csv.tmp", stem);
        unlink(p);
        snprintf(p, sizeof(p), "%s.csv.prog", stem);
        unlink(p);
        snprintf(p, sizeof(p), "%s.mark", stem);
        unlink(p);
        snprintf(p, sizeof(p), "%s.aux.csv", stem);
        unlink(p);
        if (!bin_exists) {
            break; // part 1's .bin was already gone -- sidecars (if any) now cleaned
        }
    }

    if (!any_deleted) {
        return handler_error(req, "404 Not Found", "recording not found");
    }
    return handler_ok(req);
}

// Sanitizes a user-supplied recording name into a safe filename base: keeps
// letters/digits/_/-, turns spaces into underscores, drops everything else
// (so no dots, slashes, or path tricks can slip in).
static void sanitize_rec_name(const char *in, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in != NULL && in[i] != '\0' && j < out_len - 1; i++) {
        char c = in[i];
        if (c == ' ') {
            c = '_';
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-') {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

// Renames a whole recording (every rotation part's .bin plus its .csv/.log/
// .mark/.csv.tmp/.csv.prog) to a new base name. Refuses while the recording is
// active or being converted, or if the new name is already taken.
static esp_err_t handler_recordings_rename(httpd_req_t *req) {
    char body[224];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    char id[128] = "", name[128] = "";
    if (!json_extract_string(json_find_value(body, "id"), id, sizeof(id)) ||
        !json_extract_string(json_find_value(body, "name"), name, sizeof(name))) {
        return handler_error(req, "400 Bad Request", "missing id or name");
    }
    if (strchr(id, '/') != NULL || strstr(id, "..") != NULL) {
        return handler_error(req, "400 Bad Request", "invalid id");
    }
    char old_base[128], new_base[128];
    recording_base(id, old_base, sizeof(old_base));
    sanitize_rec_name(name, new_base, sizeof(new_base));
    if (old_base[0] == '\0' || new_base[0] == '\0') {
        return handler_error(req, "400 Bad Request", "invalid name");
    }
    if (strcmp(old_base, new_base) == 0) {
        return handler_ok(req); // nothing to do
    }

    csv_export_result_t exp;
    csv_export_get_result(&exp);
    if (exp.state == CSV_EXPORT_RUNNING) {
        char eb[128];
        recording_base(exp.filename, eb, sizeof(eb));
        if (strcmp(eb, old_base) == 0) {
            return handler_error(req, "409 Conflict", "conversion in progress for this recording");
        }
    }
    const char *active = recorder_get_current_filename();
    if (active[0] != '\0') {
        const char *an = strrchr(active, '/');
        an = an ? an + 1 : active;
        char ab[128];
        recording_base(an, ab, sizeof(ab));
        if (strcmp(ab, old_base) == 0) {
            return handler_error(req, "409 Conflict", "recording is currently active");
        }
    }

    struct stat st;
    char check[220];
    snprintf(check, sizeof(check), "%s/%s.bin", REC_DIR, new_base);
    if (stat(check, &st) == 0) {
        return handler_error(req, "409 Conflict", "a recording with that name already exists");
    }

    static const char *exts[] = {".bin", ".csv", ".log", ".mark", ".aux.csv", ".csv.tmp", ".csv.prog"};
    bool any = false;
    for (int pn = 1;; pn++) {
        char old_stem[200], new_stem[200];
        if (pn == 1) {
            snprintf(old_stem, sizeof(old_stem), "%s/%s", REC_DIR, old_base);
            snprintf(new_stem, sizeof(new_stem), "%s/%s", REC_DIR, new_base);
        } else {
            snprintf(old_stem, sizeof(old_stem), "%s/%s_part%d", REC_DIR, old_base, pn);
            snprintf(new_stem, sizeof(new_stem), "%s/%s_part%d", REC_DIR, new_base, pn);
        }
        char obin[220];
        snprintf(obin, sizeof(obin), "%s.bin", old_stem);
        bool bin_exists = (stat(obin, &st) == 0);
        if (!bin_exists && pn > 1) {
            break; // no more parts
        }
        for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
            char of[224], nf[224];
            snprintf(of, sizeof(of), "%s%s", old_stem, exts[e]);
            snprintf(nf, sizeof(nf), "%s%s", new_stem, exts[e]);
            if (stat(of, &st) == 0 && rename(of, nf) == 0) {
                any = true;
            }
        }
        if (!bin_exists) {
            break; // part 1 had no .bin
        }
    }
    if (!any) {
        return handler_error(req, "404 Not Found", "recording not found");
    }
    return handler_ok(req);
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
    size_t cap = 64 * 128 + 16;
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
        off = append(buf, cap, off, "%s{\"id\":%u,\"t_s\":%.1f,\"unix\":%lld,\"note\":\"%s\"}",
                     i ? "," : "", (unsigned)marks[i].id, marks[i].elapsed_s,
                     (long long)marks[i].wall_unix_s, note_esc);
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

static esp_err_t handler_settings_auto_range(httpd_req_t *req) {
    char body[48];
    if (!read_body(req, body, sizeof(body))) {
        return handler_error(req, "400 Bad Request", "bad body");
    }
    int enabled = 0;
    if (!json_extract_int(json_find_value(body, "enabled"), &enabled)) {
        return handler_error(req, "400 Bad Request", "missing enabled");
    }
    recorder_set_auto_range(enabled != 0);
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
        {.uri = "/api/accel/config", .method = HTTP_POST, .handler = handler_accel_config},
        {.uri = "/api/accel/selftest", .method = HTTP_POST, .handler = handler_accel_selftest},
        {.uri = "/api/accel/remember", .method = HTTP_POST, .handler = handler_accel_remember},
        {.uri = "/api/record/start", .method = HTTP_POST, .handler = handler_record_start},
        {.uri = "/api/record/stop", .method = HTTP_POST, .handler = handler_record_stop},
        {.uri = "/api/record/mark", .method = HTTP_POST, .handler = handler_record_mark},
        {.uri = "/api/record/mark/note", .method = HTTP_POST, .handler = handler_record_mark_note},
        {.uri = "/api/record/mark/delete", .method = HTTP_POST, .handler = handler_record_mark_delete},
        {.uri = "/api/record/markers", .method = HTTP_GET, .handler = handler_record_markers},
        {.uri = "/api/settings/button_mark", .method = HTTP_POST, .handler = handler_settings_button_mark},
        {.uri = "/api/settings/auto_range", .method = HTTP_POST, .handler = handler_settings_auto_range},
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
        {.uri = "/api/export/csv", .method = HTTP_POST, .handler = handler_export_csv},
        {.uri = "/api/export/resume", .method = HTTP_POST, .handler = handler_export_resume},
        {.uri = "/api/export/discard", .method = HTTP_POST, .handler = handler_export_discard},
        {.uri = "/api/export/cancel", .method = HTTP_POST, .handler = handler_export_cancel},
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
    config.max_uri_handlers = 60; // headroom above the ~49 currently registered (cheap: a pointer array)
    config.max_open_sockets = MAX_WS_CLIENTS;
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));
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
