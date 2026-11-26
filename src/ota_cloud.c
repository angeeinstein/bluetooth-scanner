#include "ota_cloud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_report.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ota.h"
#include "recorder.h"
#include "udp_stream.h"
#include "version.h"
#include "ws_stream.h"

#define NVS_NS "ota_cloud"
#define DL_CHUNK 4096

// Transient-failure retries. A firmware download can fail at the connect (a
// socket momentarily unavailable, the just-closed /version check still in TCP
// TIME_WAIT, a DNS hiccup) or -- the one that actually bit us -- the TLS stream
// breaking partway through a ~1.5 MB transfer. An OTA write can't resume
// mid-stream, so the whole download is retried: each pass re-opens and
// re-streams from the start (see install_task). The lighter authed POST used by
// device reporting retries just the connect (see ota_cloud_post_json).
#define OTA_DOWNLOAD_ATTEMPTS 3   // whole-download retries on a transient transport failure
#define OTA_DOWNLOAD_RETRY_MS 3000 // backoff between them (let Wi-Fi recover)
#define OTA_CONNECT_RETRY_MS 1500 // backoff between the POST helper's connect attempts

static volatile bool s_active;   // a cloud install task is running
static char s_status_err[80];    // last cloud-install error (connect phase etc.)

// --- NVS config -------------------------------------------------------------

static bool load_config(char *url, size_t url_sz, char *id, size_t id_sz, char *secret,
                        size_t secret_sz) {
    if (url) url[0] = '\0';
    if (id) id[0] = '\0';
    if (secret) secret[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false; // never configured yet
    }
    size_t n;
    if (url) {
        n = url_sz;
        if (nvs_get_str(h, "url", url, &n) != ESP_OK) url[0] = '\0';
    }
    if (id) {
        n = id_sz;
        if (nvs_get_str(h, "cf_id", id, &n) != ESP_OK) id[0] = '\0';
    }
    if (secret) {
        n = secret_sz;
        if (nvs_get_str(h, "cf_secret", secret, &n) != ESP_OK) secret[0] = '\0';
    }
    nvs_close(h);
    return url ? (url[0] != '\0') : true;
}

bool ota_cloud_get_config(char *url, size_t url_sz, char *client_id, size_t id_sz,
                          bool *has_secret) {
    char secret[OTA_CLOUD_SECRET_MAX];
    bool configured = load_config(url, url_sz, client_id, id_sz, secret, sizeof(secret));
    if (has_secret) *has_secret = (secret[0] != '\0');
    return configured;
}

// --- input sanitizing -------------------------------------------------------
// The dashboard fields are easy to paste "almost right": a base URL with a
// trailing slash (which the "/version" path then doubles into "//version" ->
// 404), or a token grabbed from a headers view with its label still attached
// ("CF-Access-Client-Id: <id>"). Normalize here, at the single point where
// values enter NVS, so every later request is built from clean parts no matter
// how the client sent them.

// Copies src into dst[dst_sz] with leading/trailing ASCII whitespace removed.
static void copy_trimmed(char *dst, size_t dst_sz, const char *src) {
    if (dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\r' ||
                     src[n - 1] == '\n')) {
        n--;
    }
    if (n > dst_sz - 1) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Removes any trailing '/' from a base URL in place.
static void strip_trailing_slashes(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = '\0';
}

// Drops a leading HTTP-header-style label ("CF-Access-Client-Id: ...") in place.
// A label is a run of [A-Za-z0-9-] immediately followed by ':', which never
// occurs inside a real client id (hex + ".access") or secret ("cfast_..."), so
// this is safe to always apply. Also skips the whitespace after the colon.
static void strip_label(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    char *scan = p;
    while ((*scan >= 'A' && *scan <= 'Z') || (*scan >= 'a' && *scan <= 'z') ||
           (*scan >= '0' && *scan <= '9') || *scan == '-') {
        scan++;
    }
    if (scan > p && *scan == ':') {
        p = scan + 1;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (p != s) memmove(s, p, strlen(p) + 1);
}

bool ota_cloud_set_config(const char *url, const char *client_id, const char *client_secret) {
    char url_c[OTA_CLOUD_URL_MAX], id_c[OTA_CLOUD_ID_MAX], secret_c[OTA_CLOUD_SECRET_MAX];
    copy_trimmed(url_c, sizeof(url_c), url);
    strip_trailing_slashes(url_c);
    copy_trimmed(id_c, sizeof(id_c), client_id);
    strip_label(id_c);
    copy_trimmed(secret_c, sizeof(secret_c), client_secret);
    strip_label(secret_c);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_str(h, "url", url_c);
    nvs_set_str(h, "cf_id", id_c);
    // Only replace the secret when a new one is actually supplied, so the form
    // (which never echoes the stored secret back) can be re-saved without it.
    if (secret_c[0] != '\0') {
        nvs_set_str(h, "cf_secret", secret_c);
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// --- version compare + tiny JSON scan ---------------------------------------

static void parse_ver(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (s && (*s == 'v' || *s == 'V')) s++;
    if (s) sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

static bool is_newer(const char *server, const char *current) {
    int a[3], b[3];
    parse_ver(server, a);
    parse_ver(current, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

// Extracts the string value of "<key>" from a small JSON object. Good enough for
// the trusted, flat response our own worker returns; not a general parser.
static bool json_str(const char *buf, const char *key, char *out, size_t sz) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < sz - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static long json_num(const char *buf, const char *key) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    return strtol(p + 1, NULL, 10);
}

// --- HTTP helpers -----------------------------------------------------------

static esp_http_client_handle_t open_authed(const char *url_base, const char *path, int timeout_ms,
                                            const char *id, const char *secret, char *err,
                                            size_t err_sz) {
    char full[OTA_CLOUD_URL_MAX + 16];
    snprintf(full, sizeof(full), "%s%s", url_base, path);
    esp_http_client_config_t cfg = {
        .url = full,
        .crt_bundle_attach = esp_crt_bundle_attach, // verify TLS against the CA bundle
        .timeout_ms = timeout_ms,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        snprintf(err, err_sz, "http init failed");
        return NULL;
    }
    if (id && id[0]) esp_http_client_set_header(c, "CF-Access-Client-Id", id);
    if (secret && secret[0]) esp_http_client_set_header(c, "CF-Access-Client-Secret", secret);
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        snprintf(err, err_sz, "connect failed: %s", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

static bool http_status_ok(esp_http_client_handle_t c, char *err, size_t err_sz) {
    int status = esp_http_client_get_status_code(c);
    if (status == 200) return true;
    if (status == 401 || status == 403) {
        snprintf(err, err_sz, "access denied (HTTP %d -- check the Cloudflare token)", status);
    } else {
        snprintf(err, err_sz, "server returned HTTP %d", status);
    }
    return false;
}

// --- version check ----------------------------------------------------------

void ota_cloud_check(ota_cloud_check_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->current, sizeof(out->current), "%s", FIRMWARE_VERSION);

    char url[OTA_CLOUD_URL_MAX], id[OTA_CLOUD_ID_MAX], secret[OTA_CLOUD_SECRET_MAX];
    if (!load_config(url, sizeof(url), id, sizeof(id), secret, sizeof(secret))) {
        snprintf(out->error, sizeof(out->error), "no update server configured");
        return;
    }
    out->configured = true;

    esp_http_client_handle_t c =
        open_authed(url, "/version", 10000, id, secret, out->error, sizeof(out->error));
    if (c == NULL) return;

    esp_http_client_fetch_headers(c);
    if (!http_status_ok(c, out->error, sizeof(out->error))) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return;
    }
    char body[256];
    int total = 0, r;
    while (total < (int)sizeof(body) - 1 &&
           (r = esp_http_client_read(c, body + total, sizeof(body) - 1 - total)) > 0) {
        total += r;
    }
    body[total > 0 ? total : 0] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (!json_str(body, "version", out->latest, sizeof(out->latest))) {
        snprintf(out->error, sizeof(out->error), "unexpected response from server");
        return;
    }
    long sz = json_num(body, "size");
    out->size = sz > 0 ? (uint32_t)sz : 0;
    out->update_available = is_newer(out->latest, out->current);
}

// --- download + install -----------------------------------------------------

static void set_status_err(const char *msg) {
    snprintf(s_status_err, sizeof(s_status_err), "%s", msg ? msg : "");
}

// One full download+install pass. Returns 1 on success (image set as boot
// partition -- the caller reboots), 0 on a transient transport failure worth
// retrying (connect failed, the stream broke mid-transfer, or the server closed
// early -- the OTA session is left aborted so the next pass starts clean), or -1
// on a fatal failure retrying can't fix (a definitive HTTP status, the session
// can't begin, a flash-write error, or a complete-but-rejected image). `err`
// gets a short reason on any non-success.
static int download_once(const char *url, const char *id, const char *secret, char *err,
                         size_t err_sz) {
    esp_http_client_handle_t c = open_authed(url, "/firmware", 20000, id, secret, err, err_sz);
    if (c == NULL) {
        return 0; // connect failed -- transient
    }
    int clen = esp_http_client_fetch_headers(c);
    if (!http_status_ok(c, err, err_sz)) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return -1; // an HTTP status is a definitive answer
    }

    // Hand off to the shared OTA core (also enforces "not while recording", a
    // valid OTA layout, and the build-token check on ota_end).
    if (!ota_begin(clen > 0 ? (uint32_t)clen : 0)) {
        ota_status_t ost;
        ota_get_status(&ost);
        snprintf(err, err_sz, "%s", ost.error[0] ? ost.error : "cannot start update");
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return -1; // a state problem (recording / no partition) -- retry won't help
    }

    uint8_t *buf = malloc(DL_CHUNK);
    if (buf == NULL) {
        snprintf(err, err_sz, "out of memory");
        ota_abort();
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return 0; // memory pressure may pass -- worth a retry
    }
    bool stream_ok = true;    // no transport error mid-stream
    bool write_failed = false; // the flash-write side rejected a chunk
    for (;;) {
        int r = esp_http_client_read(c, (char *)buf, DL_CHUNK);
        if (r < 0) {
            stream_ok = false; // mid-stream drop -- transient
            break;
        }
        if (r == 0) {
            break; // connection closed -- completeness is checked below
        }
        if (!ota_write(buf, (size_t)r)) {
            write_failed = true; // ota_write already aborted the session + set the error
            break;
        }
    }
    free(buf);

    bool complete = stream_ok && !write_failed && esp_http_client_is_complete_data_received(c);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (write_failed) {
        ota_status_t ost;
        ota_get_status(&ost);
        snprintf(err, err_sz, "%s", ost.error[0] ? ost.error : "flash write failed");
        return -1; // a flash-side failure, not transport
    }
    if (!complete) {
        ota_abort(); // free the half-written session before the next pass
        snprintf(err, err_sz, "download interrupted");
        return 0; // the transfer didn't finish -- transient
    }
    if (!ota_end()) {
        ota_status_t ost;
        ota_get_status(&ost);
        snprintf(err, err_sz, "%s", ost.error[0] ? ost.error : "image rejected");
        ota_abort();
        return -1; // a complete-but-invalid image is a real problem, not transport
    }
    return 1; // success
}

static void install_task(void *arg) {
    (void)arg;
    char url[OTA_CLOUD_URL_MAX], id[OTA_CLOUD_ID_MAX], secret[OTA_CLOUD_SECRET_MAX];
    if (!load_config(url, sizeof(url), id, sizeof(id), secret, sizeof(secret))) {
        set_status_err("no update server configured");
        s_active = false;
        vTaskDelete(NULL);
        return;
    }

    // Reclaim internal RAM for the download's TLS record buffers on this
    // memory-tight board. All three are idle or irrelevant during an OTA
    // (recording is already refused while one runs), so this frees real headroom
    // without touching the recording path: pause live streaming, and free the
    // device-report task's ~8 KB stack -- which is exactly the difference that
    // makes the reporter's own TLS POSTs succeed while the download's reads fail.
    ws_stream_stop();
    udp_stream_stop();
    device_report_request_stop();
    for (int i = 0; i < 40 && device_report_is_running(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100)); // reporter self-exits at its next poll (<=2 s)
    }

    char err[80] = "";
    int result = -1;
    for (int attempt = 1; attempt <= OTA_DOWNLOAD_ATTEMPTS; attempt++) {
        result = download_once(url, id, secret, err, sizeof(err));
        if (result != 0) {
            break; // success or a fatal error -- no point retrying
        }
        printf("ota_cloud: download attempt %d/%d failed (%s)%s\n", attempt, OTA_DOWNLOAD_ATTEMPTS,
               err, attempt < OTA_DOWNLOAD_ATTEMPTS ? " -- retrying" : "");
        if (attempt < OTA_DOWNLOAD_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(OTA_DOWNLOAD_RETRY_MS));
        }
    }

    if (result > 0) {
        set_status_err("");
        ota_schedule_reboot(); // success -- device reboots into the new image
        // leave s_active true; the reboot ends this task
        vTaskDelete(NULL);
        return;
    }
    set_status_err(err[0] ? err : "update failed");
    s_active = false;
    device_report_start(); // update didn't reboot the device -- bring reporting back
    vTaskDelete(NULL);
}

bool ota_cloud_start_install(char *err, size_t err_sz) {
    if (s_active) {
        snprintf(err, err_sz, "an update is already in progress");
        return false;
    }
    char url[OTA_CLOUD_URL_MAX];
    if (!load_config(url, sizeof(url), NULL, 0, NULL, 0)) {
        snprintf(err, err_sz, "no update server configured");
        return false;
    }
    if (recorder_is_recording()) {
        snprintf(err, err_sz, "stop the recording before updating");
        return false;
    }
    s_status_err[0] = '\0';
    s_active = true;
    // TLS (handshake + cert-bundle verification) is stack-hungry; keep it off the
    // httpd task so the dashboard stays responsive and can show progress. 8 KB
    // matches ESP-IDF's own esp_https_ota example (same TLS + flash-write
    // workload) and, unlike a 12 KB stack, reliably fits in the scarce *internal*
    // heap (task stacks can't live in PSRAM) -- without relocating Wi-Fi/LWIP to
    // PSRAM, which would contend with the recording ring during record+stream.
    if (xTaskCreate(install_task, "ota_cloud", 8192, NULL, 5, NULL) != pdPASS) {
        s_active = false;
        // This is a low-internal-RAM failure (task stacks must live in internal
        // RAM); report the numbers so it's diagnosable without a serial console.
        size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snprintf(err, err_sz, "low memory: %u KB free internal, largest block %u KB",
                 (unsigned)(freeb / 1024), (unsigned)(largest / 1024));
        return false;
    }
    return true;
}

void ota_cloud_get_status(bool *active, char *err, size_t err_sz) {
    if (active) *active = s_active;
    if (err) snprintf(err, err_sz, "%s", s_status_err);
}

// --- authed JSON POST (device-address reporting reuses this) -----------------

// A couple fewer connect attempts than the firmware download: a report is
// low-value and frequent, so it shouldn't tie up its task for as long on a dead
// network -- the caller's own heartbeat/backoff picks it up again shortly.
#define REPORT_CONNECT_ATTEMPTS 3

bool ota_cloud_is_configured(void) {
    char url[OTA_CLOUD_URL_MAX];
    return load_config(url, sizeof(url), NULL, 0, NULL, 0);
}

bool ota_cloud_post_json(const char *path, const char *body, char *err, size_t err_sz) {
    if (err && err_sz) err[0] = '\0';
    char url[OTA_CLOUD_URL_MAX], id[OTA_CLOUD_ID_MAX], secret[OTA_CLOUD_SECRET_MAX];
    if (!load_config(url, sizeof(url), id, sizeof(id), secret, sizeof(secret))) {
        if (err) snprintf(err, err_sz, "no update server configured");
        return false;
    }
    char full[OTA_CLOUD_URL_MAX + 24];
    snprintf(full, sizeof(full), "%s%s", url, path);
    int blen = (int)strlen(body);

    esp_err_t last = ESP_FAIL;
    for (int attempt = 1; attempt <= REPORT_CONNECT_ATTEMPTS; attempt++) {
        esp_http_client_config_t cfg = {
            .url = full,
            .crt_bundle_attach = esp_crt_bundle_attach, // same CA bundle as OTA
            .timeout_ms = 8000,
            .keep_alive_enable = false,
            .method = HTTP_METHOD_POST,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (c == NULL) {
            if (err) snprintf(err, err_sz, "http init failed");
            return false;
        }
        esp_http_client_set_header(c, "Content-Type", "application/json");
        if (id[0]) esp_http_client_set_header(c, "CF-Access-Client-Id", id);
        if (secret[0]) esp_http_client_set_header(c, "CF-Access-Client-Secret", secret);

        esp_err_t e = esp_http_client_open(c, blen);
        if (e == ESP_OK && esp_http_client_write(c, body, blen) == blen) {
            esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            char sink[128]; // drain the small response so the socket closes cleanly
            while (esp_http_client_read(c, sink, sizeof(sink)) > 0) {
            }
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            if (status == 200) return true;
            // An HTTP status is a definitive answer -- retrying won't change it.
            if (status == 401 || status == 403) {
                if (err) snprintf(err, err_sz, "access denied (HTTP %d)", status);
            } else if (err) {
                snprintf(err, err_sz, "server returned HTTP %d", status);
            }
            return false;
        }
        // Transport-level failure (connect/write) -- transient; retry.
        last = (e != ESP_OK) ? e : ESP_FAIL;
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (attempt < REPORT_CONNECT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(OTA_CONNECT_RETRY_MS));
        }
    }
    if (err) snprintf(err, err_sz, "connect failed: %s", esp_err_to_name(last));
    return false;
}
