#include "device_report.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "ota_cloud.h"
#include "power_ctrl.h"
#include "recorder.h"
#include "version.h"
#include "wifi_manager.h"

#define REPORT_PATH "/device/report"
#define REPORT_HEARTBEAT_US (5LL * 60 * 1000000) // ~5 min while online
#define REPORT_POLL_MS 2000                       // how often the task checks for an IP change
#define REPORT_RETRY_MIN_MS 15000                 // first backoff after a failed POST
#define REPORT_RETRY_MAX_MS 120000                // backoff ceiling (doubles up to here)

// Own namespace, not VibraShock's "dev_report" (see ota_cloud.c).
#define NVS_NS "bts_dev_report"

// Cached copy of the on/off setting (loaded from NVS at start, updated on set),
// so the poll task never has to touch NVS. Default on.
static volatile bool s_enabled = true;

// Task handle + cooperative-stop flag (see device_report_request_stop()).
static TaskHandle_t s_task = NULL;
static volatile bool s_stop_req = false;

void device_report_request_stop(void) {
    s_stop_req = true;
}

bool device_report_is_running(void) {
    return s_task != NULL;
}

bool device_report_get_enabled(void) {
    return s_enabled;
}

void device_report_set_enabled(bool enabled) {
    s_enabled = enabled;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "enabled", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Appends `src` to `dst` as an escaped JSON string body (no surrounding quotes),
// bounded by `cap`. Only the SSID needs this -- an AP name can contain a quote
// or backslash; control bytes are dropped. device_id/hostname/firmware are
// known-safe. dst must already be a valid (possibly empty) NUL-terminated string.
static void json_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)ch;
        } else if (ch >= 0x20) {
            dst[o++] = (char)ch;
        }
    }
    dst[o] = '\0';
}

// Builds the report JSON from current firmware state. `did`/`ip` are required;
// optional fields are simply omitted when unavailable (never sent as "unknown").
static void build_body(char *buf, size_t cap, const char *ip, const char *host,
                       const char *did, const wifi_status_t *w) {
    power_status_t p;
    power_ctrl_get_status(&p);

    int off = snprintf(buf, cap, "{\"device_id\":\"%s\",\"ip\":\"%s\"", did, ip);
    if (host && host[0]) {
        off += snprintf(buf + off, cap - off, ",\"hostname\":\"%s\"", host);
    }
    if (w->sta_ssid[0]) {
        char ssid_esc[2 * sizeof(w->sta_ssid)];
        json_escape(ssid_esc, sizeof(ssid_esc), w->sta_ssid);
        off += snprintf(buf + off, cap - off, ",\"wifi_ssid\":\"%s\"", ssid_esc);
    }
    off += snprintf(buf + off, cap - off, ",\"firmware\":\"%s\"", FIRMWARE_VERSION);
    if (p.ok && p.battery_present) { // a real cell is connected -> figures are meaningful
        off += snprintf(buf + off, cap - off, ",\"battery_percent\":%.1f,\"charging\":%s",
                        p.batt_percent, p.charging ? "true" : "false");
    }
    // metadata: small best-effort extras (the worker treats this as opaque).
    off += snprintf(buf + off, cap - off, ",\"metadata\":{\"recording\":%s",
                    recorder_is_recording() ? "true" : "false");
    uint64_t total = 0, freeb = 0;
    if (recorder_get_sd_space(&total, &freeb)) {
        off += snprintf(buf + off, cap - off, ",\"sd_free_gb\":%.1f", (double)freeb / 1e9);
    }
    snprintf(buf + off, cap - off, "}}");
}

static void report_task(void *arg) {
    (void)arg;
    // Let Wi-Fi associate and settle before the first attempt.
    vTaskDelay(pdMS_TO_TICKS(8000));

    char last_ip[16] = "";         // IP of the last successful report ("" => none yet)
    int64_t next_heartbeat_us = 0; // 0 => a report is due as soon as we're online
    int64_t retry_at_us = 0;       // nonzero => in post-failure backoff until this time
    int64_t backoff_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_POLL_MS));

        // Safe self-exit point: no locks held here. Freed on request (e.g. to
        // hand this task's stack to the OTA download's TLS buffers).
        if (s_stop_req) {
            s_stop_req = false;
            s_task = NULL;
            vTaskDelete(NULL);
        }

        if (!s_enabled) {
            continue; // reporting turned off in settings -- send nothing
        }

        // Never run a report's TLS session while a firmware download is in
        // flight: two concurrent TLS handshakes contend for the same scarce
        // internal RAM, and the download (the higher-value transfer) must win.
        bool ota_active = false;
        ota_cloud_get_status(&ota_active, NULL, 0);
        if (ota_active) {
            continue;
        }

        wifi_status_t w;
        wifi_manager_get_status(&w);
        bool online = (w.sta_state == WIFI_STA_CONNECTED) && w.ip[0] &&
                      strcmp(w.ip, "0.0.0.0") != 0;
        if (!online) {
            // Not up (or just dropped): forget the last IP so a fresh connect
            // reports immediately, and clear any pending backoff.
            last_ip[0] = '\0';
            retry_at_us = 0;
            backoff_ms = 0;
            continue;
        }

        int64_t now = esp_timer_get_time();
        bool should_attempt;
        if (retry_at_us) {
            should_attempt = now >= retry_at_us; // honor backoff; ignore other triggers
        } else {
            bool ip_changed = strcmp(w.ip, last_ip) != 0;
            should_attempt = ip_changed || now >= next_heartbeat_us;
        }
        if (!should_attempt) {
            continue;
        }
        if (!ota_cloud_is_configured()) {
            // No server to report to -- stay quiet until the next heartbeat window.
            next_heartbeat_us = now + REPORT_HEARTBEAT_US;
            retry_at_us = 0;
            backoff_ms = 0;
            continue;
        }

        // device_id == the hostname's suffix (e.g. "btscanner-cc1588" -> "cc1588"),
        // so it always matches what /find/<device_id> resolves to.
        char host[40];
        wifi_manager_get_hostname(host, sizeof(host));
        const char *did = strrchr(host, '-');
        did = did ? did + 1 : host;

        char body[384];
        build_body(body, sizeof(body), w.ip, host, did, &w);

        char err[80];
        if (ota_cloud_post_json(REPORT_PATH, body, err, sizeof(err))) {
            strncpy(last_ip, w.ip, sizeof(last_ip) - 1);
            last_ip[sizeof(last_ip) - 1] = '\0';
            next_heartbeat_us = now + REPORT_HEARTBEAT_US;
            retry_at_us = 0;
            backoff_ms = 0;
        } else {
            printf("device_report: report failed (%s) -- will retry\n", err);
            backoff_ms = backoff_ms ? backoff_ms * 2 : REPORT_RETRY_MIN_MS;
            if (backoff_ms > REPORT_RETRY_MAX_MS) backoff_ms = REPORT_RETRY_MAX_MS;
            retry_at_us = now + backoff_ms * 1000;
        }
    }
}

void device_report_start(void) {
    if (s_task != NULL) {
        return; // already running (e.g. a redundant restart after a failed OTA)
    }
    s_stop_req = false;
    // Load the persisted on/off setting (default on if never set).
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 1;
        if (nvs_get_u8(h, "enabled", &v) != ESP_OK) v = 1;
        s_enabled = (v != 0);
        nvs_close(h);
    }
    // TLS (handshake + cert-bundle verify) is stack-hungry. Low priority so it
    // never competes with sampling/SD/Wi-Fi.
    if (xTaskCreate(report_task, "dev_report", 8192, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        printf("device_report: task create failed -- address reporting disabled\n");
    }
}
