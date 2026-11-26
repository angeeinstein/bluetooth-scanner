#include "wifi_manager.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "device_id.h"
#include "esp_eap_client.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "recorder.h"
#include "udp_stream.h"
#include "ws_stream.h"

#define AP_PASSWORD "recorder123"
// The friendly, shared mDNS name (opt-in per device, default on). Each device
// ALSO always has a unique name/SSID derived from its device id (see
// compute_device_names) so multiple boards never collide and a future single-page
// controller can address each one individually.
#define MDNS_FRIENDLY "btscanner"

// Per-device unique identity, computed once at start from the device id:
//   s_host_unique  e.g. "btscanner-ccef4c"  (mDNS + DHCP hostname)
//   s_ap_ssid      e.g. "BTScanner-CCEF4C"  (SoftAP SSID)
static char s_host_unique[32];
static char s_ap_ssid[32];
static esp_netif_t *s_sta_netif = NULL;
static bool s_mdns_ready = false;
// Whether this device also answers to the shared MDNS_FRIENDLY name. Persisted;
// default on so a lone device is reachable at btscanner.local exactly as before.
static bool s_mdns_friendly = true;

#define REDUCED_TX_POWER 34 // 8dBm, see esp_wifi_set_max_tx_power()'s mapping table
#define BROWNOUT_BACKOFF_THRESHOLD 2
#define BROWNOUT_NVS_NS "boot_cfg"

// --- Multi-network reconnect tuning -----------------------------------------
#define WIFI_PER_NET_RETRIES 3            // attempts on an in-range network before moving on
#define WIFI_CONNECT_TIMEOUT_MS 15000     // safety net if no connect result event arrives
#define WIFI_IDLE_RETRY_MS 5000           // between failed passes when the device is idle
#define WIFI_BACKOFF_START_MS 20000       // between failed passes when actively used (escalates)
#define WIFI_BACKOFF_MAX_MS 120000
// After this many fully-failed passes in a row, clear the "auth failed" marks so
// a network flagged on a transient/ambiguous failure (an AP that was refusing us,
// a roam, weak signal) gets a fresh full-budget attempt instead of being stuck on
// the reduced one-try-per-pass path until a manual reconnect. A genuinely wrong
// password just gets re-flagged on the next pass.
#define WIFI_AUTHFAIL_AMNESTY_PASSES 3
#define WIFI_ACTIVITY_WINDOW_US (15 * 1000000LL) // "recently used" window for the web UI signal

#define NETS_NVS_NS "wifi_cfg"
#define NETS_NVS_KEY "nets"

// One remembered network (also the on-disk NVS-blob record). `used_seq` is a
// monotonic stamp bumped on connect + manual add/update; the smallest is
// evicted when a 9th network is saved (least-recently-used). `auth_failed` is
// runtime-only -- reset to 0 on load.
typedef struct {
    char ssid[33];
    char pass[65];
    char user[65]; // enterprise username/identity ("" for personal)
    uint8_t enterprise;
    uint8_t auth_failed;
    uint32_t used_seq;
} wifi_net_t;

typedef enum {
    WMSG_DISCONNECTED, // .reason set
    WMSG_GOT_IP,
    WMSG_CONNECT,   // add/update (.ssid/.pass/.user/.enterprise) + make it the target
    WMSG_SELECT,    // connect to a saved network (.ssid)
    WMSG_FORGET,    // remove a saved network (.ssid)
    WMSG_SET_PIN,   // pin .ssid to .bssid and reconnect
    WMSG_CLEAR_PIN, // remove the AP pin
} wmsg_type_t;

typedef struct {
    wmsg_type_t type;
    uint8_t reason;
    char ssid[33];
    char pass[65];
    char user[65];
    uint8_t enterprise;
    uint8_t bssid[6];
} wmsg_t;

typedef enum { WPHASE_IDLE, WPHASE_CONNECTING, WPHASE_CONNECTED, WPHASE_BACKOFF } wphase_t;
typedef enum { FAIL_NOT_FOUND, FAIL_AUTH, FAIL_TRANSIENT } fail_class_t;

static SemaphoreHandle_t s_status_mutex; // guards s_status + s_nets/s_net_count
static wifi_status_t s_status = {0};
static QueueHandle_t s_wmsg_queue;

// Saved-network list (guarded by s_status_mutex; only the reconnect task
// writes it, the HTTP task only reads).
static wifi_net_t s_nets[WIFI_MAX_SAVED];
static int s_net_count = 0;
static uint32_t s_use_counter = 0;

// Optional AP pin (guarded by s_status_mutex; written by the reconnect task,
// read by both). When set, joining s_pin_ssid targets exactly s_pin_bssid on the
// first try (see attempt_current). Stored separately from wifi_net_t so the
// saved-network NVS blob format is unchanged.
static bool s_pin_set = false;
static char s_pin_ssid[33] = "";
static uint8_t s_pin_bssid[6] = {0};

// Reconnect state machine -- touched only by the reconnect task.
static wphase_t s_phase = WPHASE_IDLE;
static int s_order[WIFI_MAX_SAVED]; // candidate indices for the current pass
static int s_order_count = 0;
static int s_cand_idx = 0;
static int s_cand_attempt = 0;
static int s_backoff_ms = WIFI_BACKOFF_START_MS;
static int s_backoff_wait_ms = WIFI_IDLE_RETRY_MS;
static int s_failed_passes = 0; // consecutive fully-failed passes (drives the auth-fail amnesty)
static bool s_restart_pending = false;
static char s_forced_ssid[33] = "";

static volatile int64_t s_last_activity_us = 0;

// Spawned fresh on every got-IP (see on_got_ip). SNTP itself must be set up only
// ONCE for the app's lifetime -- it keeps the clock synced and re-polls on its
// own whenever connectivity returns. Re-initialising it on a later reconnect
// asserts inside lwip ("operating mode must not be set while SNTP client is
// running") and panics the whole device -- which, mid-recording, silently ends
// the recording. So guard init/start behind a one-shot flag and, on later
// connects, only re-check the sync status.
static void ntp_sync_task(void *arg) {
    static bool s_sntp_started = false;
    if (!s_sntp_started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&config);
        esp_netif_sntp_start();
        s_sntp_started = true;
    }
    bool synced = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK;
    // Only ever latch true: once synced the clock stays valid across reconnects
    // even if a later re-check times out (SNTP keeps re-syncing itself), so we
    // never flip it back to false.
    if (synced && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.ntp_synced = true;
        xSemaphoreGive(s_status_mutex);
    }
    vTaskDelete(NULL);
}

// Friendly explanation for the wifi_err_reason_t codes users actually hit --
// ESP-IDF reports a wrong PSK as a 4-way-handshake timeout, not an explicit
// "auth failed", so that mapping is the one worth calling out.
static const char *disconnect_reason_str(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "wrong password?";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
            return "authentication failed (wrong password?)";
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "network not found (out of range or 5GHz-only?)";
        case WIFI_REASON_ASSOC_FAIL:
            return "association rejected";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "lost connection (out of range?)";
        default:
            return "connection failed";
    }
}

// Classifies a disconnect reason for the retry logic: NOT_FOUND (the AP isn't
// in range -- skip on quickly), AUTH (found it, credentials rejected -- retry
// a few times since a handshake timeout can also be transient RF, then flag
// it), or TRANSIENT (found it, some other hiccup -- retry).
static fail_class_t classify_reason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return FAIL_NOT_FOUND;
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_MIC_FAILURE:
            return FAIL_AUTH;
        default:
            return FAIL_TRANSIENT;
    }
}

// True while the device is actively in use, so the reconnect task should be
// gentle (longer backoff) to keep the SoftAP smooth. Idle -> retry fast.
static bool is_active(void) {
    if (esp_timer_get_time() - s_last_activity_us < WIFI_ACTIVITY_WINDOW_US) {
        return true; // a client polled the dashboard recently
    }
    if (recorder_is_recording()) {
        return true;
    }
    udp_stream_status_t u;
    udp_stream_get_status(&u);
    if (u.enabled) {
        return true;
    }
    ws_stream_status_t w;
    ws_stream_get_status(&w);
    return w.enabled;
}

void wifi_manager_note_activity(void) {
    s_last_activity_us = esp_timer_get_time();
}

// --- NVS: the remembered-network list --------------------------------------

static void persist_nets(void) {
    nvs_handle_t h;
    if (nvs_open(NETS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (s_net_count > 0) {
        nvs_set_blob(h, NETS_NVS_KEY, s_nets, (size_t)s_net_count * sizeof(wifi_net_t));
    } else {
        nvs_erase_key(h, NETS_NVS_KEY);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_nets(void) {
    s_net_count = 0;
    s_use_counter = 0;

    nvs_handle_t h;
    if (nvs_open(NETS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = 0;
        if (nvs_get_blob(h, NETS_NVS_KEY, NULL, &sz) == ESP_OK && sz > 0 &&
            sz % sizeof(wifi_net_t) == 0 && sz <= sizeof(s_nets)) {
            if (nvs_get_blob(h, NETS_NVS_KEY, s_nets, &sz) == ESP_OK) {
                s_net_count = (int)(sz / sizeof(wifi_net_t));
                for (int i = 0; i < s_net_count; i++) {
                    s_nets[i].auth_failed = 0;
                    if (s_nets[i].used_seq > s_use_counter) {
                        s_use_counter = s_nets[i].used_seq;
                    }
                }
            }
        }
        nvs_close(h);
    }
}

// --- AP pin persistence (separate NVS keys; wifi_net_t blob is untouched) ----

static void persist_pin(void) {
    nvs_handle_t h;
    if (nvs_open(NETS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (s_pin_set) {
        nvs_set_str(h, "pin_ssid", s_pin_ssid);
        nvs_set_blob(h, "pin_bssid", s_pin_bssid, sizeof(s_pin_bssid));
    } else {
        nvs_erase_key(h, "pin_ssid");  // ignore "not found"
        nvs_erase_key(h, "pin_bssid");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_pin(void) {
    s_pin_set = false;
    s_pin_ssid[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NETS_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_pin_ssid);
    if (nvs_get_str(h, "pin_ssid", s_pin_ssid, &n) == ESP_OK && s_pin_ssid[0] != '\0') {
        size_t bn = sizeof(s_pin_bssid);
        if (nvs_get_blob(h, "pin_bssid", s_pin_bssid, &bn) == ESP_OK && bn == sizeof(s_pin_bssid)) {
            s_pin_set = true;
        } else {
            s_pin_ssid[0] = '\0';
        }
    }
    nvs_close(h);
}

// Caller must hold s_status_mutex.
static int find_net(const char *ssid) {
    for (int i = 0; i < s_net_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static void add_or_update_net(const char *ssid, const char *pass, const char *user,
                              uint8_t enterprise) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    int idx = find_net(ssid);
    if (idx < 0) {
        if (s_net_count < WIFI_MAX_SAVED) {
            idx = s_net_count++;
        } else {
            // Evict the least-recently-used (smallest used_seq).
            idx = 0;
            for (int i = 1; i < s_net_count; i++) {
                if (s_nets[i].used_seq < s_nets[idx].used_seq) {
                    idx = i;
                }
            }
        }
        memset(&s_nets[idx], 0, sizeof(s_nets[idx]));
    }
    strncpy(s_nets[idx].ssid, ssid, sizeof(s_nets[idx].ssid) - 1);
    strncpy(s_nets[idx].pass, pass ? pass : "", sizeof(s_nets[idx].pass) - 1);
    strncpy(s_nets[idx].user, user ? user : "", sizeof(s_nets[idx].user) - 1);
    s_nets[idx].enterprise = enterprise;
    s_nets[idx].auth_failed = 0;
    s_nets[idx].used_seq = ++s_use_counter;
    xSemaphoreGive(s_status_mutex);
    persist_nets();
}

static void remove_net(const char *ssid) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    int idx = find_net(ssid);
    if (idx >= 0) {
        for (int j = idx; j < s_net_count - 1; j++) {
            s_nets[j] = s_nets[j + 1];
        }
        s_net_count--;
    }
    xSemaphoreGive(s_status_mutex);
    persist_nets();
}

// --- STA config helpers -----------------------------------------------------

// `bssid` (6 bytes) pins the connection to that exact AP; NULL = connect to the
// strongest matching BSSID as usual.
static bool apply_personal_config(const char *ssid, const char *pass, const uint8_t *bssid) {
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (pass != NULL) {
        strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
    }
    if (bssid != NULL) {
        memcpy(sta_config.sta.bssid, bssid, 6);
        sta_config.sta.bssid_set = true;
    }
    return esp_wifi_set_config(WIFI_IF_STA, &sta_config) == ESP_OK;
}

// SSID only (no shared key), then username/password via the EAP client, and
// enable enterprise mode. `user` is used as both the outer EAP identity and
// the inner phase-2 username (PEAP/TTLS-MSCHAPv2, no separate anonymous
// identity). No CA cert -> server certificate is not validated.
static bool apply_enterprise_config(const char *ssid, const char *user, const char *pass) {
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
    if (esp_wifi_set_config(WIFI_IF_STA, &sta_config) != ESP_OK) {
        return false;
    }
    size_t user_len = strlen(user);
    size_t pass_len = pass != NULL ? strlen(pass) : 0;
    esp_eap_client_set_identity((const unsigned char *)user, (int)user_len);
    esp_eap_client_set_username((const unsigned char *)user, (int)user_len);
    esp_eap_client_set_password((const unsigned char *)(pass != NULL ? pass : ""), (int)pass_len);
    esp_eap_client_clear_ca_cert();
    esp_eap_client_set_disable_time_check(true); // we connect before NTP -- clock reads ~1970
    return esp_wifi_sta_enterprise_enable() == ESP_OK;
}

// --- Reconnect state machine (reconnect task only) --------------------------

static void set_sta_state(wifi_sta_state_t state, const char *ssid, bool enterprise) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.sta_state = state;
        if (ssid != NULL) {
            strncpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid) - 1);
            s_status.sta_ssid[sizeof(s_status.sta_ssid) - 1] = '\0';
            s_status.enterprise = enterprise;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

static void enter_backoff(void) {
    s_phase = WPHASE_BACKOFF;
    if (is_active()) {
        s_backoff_wait_ms = s_backoff_ms;
        s_backoff_ms *= 2;
        if (s_backoff_ms > WIFI_BACKOFF_MAX_MS) {
            s_backoff_ms = WIFI_BACKOFF_MAX_MS;
        }
    } else {
        s_backoff_wait_ms = WIFI_IDLE_RETRY_MS;
    }
    // Still "connecting" from the UI's point of view -- it hasn't given up.
    set_sta_state(WIFI_STA_CONNECTING, NULL, false);
}

static void attempt_current(void) {
    if (s_cand_idx >= s_order_count) {
        enter_backoff();
        return;
    }
    wifi_net_t *net = &s_nets[s_order[s_cand_idx]];
    esp_wifi_sta_enterprise_disable(); // clear any prior EAP state
    if (net->enterprise) {
        apply_enterprise_config(net->ssid, net->user, net->pass);
    } else {
        // Honor the AP pin only on the first attempt for this candidate; a retry
        // drops the pin and connects to any BSSID, so an unreachable pinned AP
        // falls back to a normal connect instead of stranding the device.
        const uint8_t *bssid = (s_pin_set && s_cand_attempt == 0 &&
                                strcmp(net->ssid, s_pin_ssid) == 0)
                                   ? s_pin_bssid
                                   : NULL;
        apply_personal_config(net->ssid, net->pass, bssid);
    }
    s_phase = WPHASE_CONNECTING;
    set_sta_state(WIFI_STA_CONNECTING, net->ssid, net->enterprise != 0);
    esp_wifi_connect();
}

static void advance_candidate(void) {
    s_cand_idx++;
    s_cand_attempt = 0;
    attempt_current(); // enters backoff if the pass is exhausted
}

// Builds the candidate order for a pass: most-recently-used first, then any
// s_forced_ssid moved to the very front (a manual connect / retry-this-one).
static void build_order(void) {
    s_order_count = s_net_count;
    for (int i = 0; i < s_net_count; i++) {
        s_order[i] = i;
    }
    for (int i = 1; i < s_order_count; i++) { // insertion sort, used_seq desc
        int k = s_order[i], j = i - 1;
        while (j >= 0 && s_nets[s_order[j]].used_seq < s_nets[k].used_seq) {
            s_order[j + 1] = s_order[j];
            j--;
        }
        s_order[j + 1] = k;
    }
    if (s_forced_ssid[0] != '\0') {
        for (int i = 0; i < s_order_count; i++) {
            if (strcmp(s_nets[s_order[i]].ssid, s_forced_ssid) == 0) {
                int idx = s_order[i];
                for (int j = i; j > 0; j--) {
                    s_order[j] = s_order[j - 1];
                }
                s_order[0] = idx;
                break;
            }
        }
        s_forced_ssid[0] = '\0';
    }
}

static void start_pass(void) {
    build_order();
    s_cand_idx = 0;
    s_cand_attempt = 0;
    if (s_order_count == 0) {
        s_phase = WPHASE_IDLE;
        set_sta_state(WIFI_STA_IDLE, "", false);
        return;
    }
    attempt_current();
}

// Manual (re)connect: restart the pass with s_forced_ssid first. If we have a
// live/in-flight connection, disconnect it and let the resulting DISCONNECTED
// event drive the restart; otherwise start straight away.
static void request_restart(void) {
    if (s_phase == WPHASE_CONNECTED || s_phase == WPHASE_CONNECTING) {
        s_restart_pending = true;
        esp_wifi_disconnect();
    } else {
        start_pass();
    }
}

// Clears every saved network's "auth failed" mark so each gets a fresh
// full-budget attempt on the next pass. Called by the amnesty (see
// WIFI_AUTHFAIL_AMNESTY_PASSES) so a network flagged on a transient/ambiguous
// failure recovers without a manual reconnect. Runs on the wifi task; takes the
// status mutex because the dashboard reads these flags.
static void clear_auth_failed_marks(void) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < s_net_count; i++) {
            s_nets[i].auth_failed = 0;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

static void apply_failure(fail_class_t fc) {
    if (fc == FAIL_NOT_FOUND) {
        advance_candidate(); // not in range -- don't waste effort here
        return;
    }
    wifi_net_t *net = &s_nets[s_order[s_cand_idx]];
    int budget = net->auth_failed ? 1 : WIFI_PER_NET_RETRIES;
    s_cand_attempt++;
    if (s_cand_attempt >= budget) {
        if (fc == FAIL_AUTH) {
            if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                net->auth_failed = 1; // likely a changed/wrong password
                xSemaphoreGive(s_status_mutex);
            }
        }
        advance_candidate();
    } else {
        attempt_current(); // give the in-range network another go
    }
}

// Derives the per-device unique SSID + hostname from the device id ("BTS-CCEF4C"
// -> "BTScanner-CCEF4C" / "btscanner-ccef4c"). mDNS/DHCP hostnames must be
// lowercase; the SSID keeps the branded casing.
static void compute_device_names(void) {
    char devid[16];
    device_id_str(devid, sizeof(devid));
    const char *tail = strchr(devid, '-');
    tail = tail ? tail + 1 : devid;
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "BTScanner-%s", tail);
    snprintf(s_host_unique, sizeof(s_host_unique), "btscanner-%s", tail);
    for (char *p = s_host_unique; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
}

static void load_mdns_cfg(void) {
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(NETS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mdns_friendly", &v); // absent -> keep default 1
        nvs_close(h);
    }
    s_mdns_friendly = v != 0;
}

// (Re)applies the shared "btscanner.local" alias as a delegated mDNS hostname
// pointing at our own current STA IP -- so the designated main device answers the
// friendly name while every device always keeps its unique s_host_unique name.
// Called at mDNS init, on each got-IP (the STA IP may change), and on toggle.
// Entirely best-effort: any failure just means the friendly alias is absent, never
// affects the unique name, Wi-Fi, or recording.
static void apply_friendly_mdns(void) {
    if (!s_mdns_ready) {
        return;
    }
    mdns_delegate_hostname_remove(MDNS_FRIENDLY); // idempotent reset (ignores "absent")
    if (!s_mdns_friendly || s_sta_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return; // not associated yet -- re-applied from on_got_ip once we have an IP
    }
    mdns_ip_addr_t addr = {0};
    addr.addr.type = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr = ip.ip.addr;
    addr.next = NULL;
    mdns_delegate_hostname_add(MDNS_FRIENDLY, &addr);
}

static void on_got_ip(void) {
    s_phase = WPHASE_CONNECTED;
    s_backoff_ms = WIFI_BACKOFF_START_MS; // reset the escalation
    s_failed_passes = 0;                  // connected -- reset the amnesty counter
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    int idx = find_net(s_status.sta_ssid); // the network we were attempting
    if (idx >= 0) {
        s_nets[idx].used_seq = ++s_use_counter;
        s_nets[idx].auth_failed = 0;
    }
    s_status.sta_state = WIFI_STA_CONNECTED;
    s_status.sta_fail_reason[0] = '\0';
    xSemaphoreGive(s_status_mutex);
    persist_nets();
    apply_friendly_mdns(); // (re)point btscanner.local at the new STA IP
    xTaskCreate(ntp_sync_task, "ntp_sync", 4096, NULL, 5, NULL);
}

static void on_disconnected(uint8_t reason) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(s_status.sta_fail_reason, disconnect_reason_str(reason),
                sizeof(s_status.sta_fail_reason) - 1);
        xSemaphoreGive(s_status_mutex);
    }

    if (s_restart_pending) {
        s_restart_pending = false;
        start_pass();
        return;
    }
    if (s_phase == WPHASE_CONNECTED) {
        // A working connection dropped -- retry that same network first.
        strncpy(s_forced_ssid, s_status.sta_ssid, sizeof(s_forced_ssid) - 1);
        s_forced_ssid[sizeof(s_forced_ssid) - 1] = '\0';
        start_pass();
        return;
    }
    if (s_phase == WPHASE_CONNECTING) {
        apply_failure(classify_reason(reason));
        return;
    }
    // WPHASE_BACKOFF / WPHASE_IDLE: spurious, ignore.
}

static void handle_msg(const wmsg_t *m) {
    switch (m->type) {
        case WMSG_GOT_IP:
            on_got_ip();
            break;
        case WMSG_DISCONNECTED:
            on_disconnected(m->reason);
            break;
        case WMSG_CONNECT:
            add_or_update_net(m->ssid, m->pass, m->user, m->enterprise);
            strncpy(s_forced_ssid, m->ssid, sizeof(s_forced_ssid) - 1);
            s_forced_ssid[sizeof(s_forced_ssid) - 1] = '\0';
            request_restart();
            break;
        case WMSG_SELECT:
            if (find_net(m->ssid) >= 0) {
                strncpy(s_forced_ssid, m->ssid, sizeof(s_forced_ssid) - 1);
                s_forced_ssid[sizeof(s_forced_ssid) - 1] = '\0';
                request_restart();
            }
            break;
        case WMSG_FORGET: {
            bool active = (s_phase == WPHASE_CONNECTED || s_phase == WPHASE_CONNECTING) &&
                          strcmp(s_status.sta_ssid, m->ssid) == 0;
            remove_net(m->ssid);
            if (active) {
                request_restart(); // disconnect it, fall through to the others
            } else if (s_phase != WPHASE_CONNECTED) {
                request_restart(); // mid-pass: rebuild order so no stale index lingers
            }
            // else connected to a *different* network -- leave it; order
            // rebuilds naturally on the next disconnect.
            break;
        }
        case WMSG_SET_PIN:
            if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
                strncpy(s_pin_ssid, m->ssid, sizeof(s_pin_ssid) - 1);
                s_pin_ssid[sizeof(s_pin_ssid) - 1] = '\0';
                memcpy(s_pin_bssid, m->bssid, sizeof(s_pin_bssid));
                s_pin_set = true;
                xSemaphoreGive(s_status_mutex);
            }
            persist_pin();
            // Reconnect to that SSID so the pin applies now (targets the exact AP).
            strncpy(s_forced_ssid, m->ssid, sizeof(s_forced_ssid) - 1);
            s_forced_ssid[sizeof(s_forced_ssid) - 1] = '\0';
            request_restart();
            break;
        case WMSG_CLEAR_PIN:
            if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
                s_pin_set = false;
                s_pin_ssid[0] = '\0';
                xSemaphoreGive(s_status_mutex);
            }
            persist_pin();
            // No reconnect -- keep the current link; the pin just stops applying
            // to future connects.
            break;
    }
}

static void wifi_task(void *arg) {
    start_pass(); // initial auto-connect from the loaded list

    for (;;) {
        TickType_t wait;
        switch (s_phase) {
            case WPHASE_CONNECTING: wait = pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS); break;
            case WPHASE_BACKOFF: wait = pdMS_TO_TICKS(s_backoff_wait_ms); break;
            default: wait = portMAX_DELAY; break; // CONNECTED / IDLE
        }
        wmsg_t m;
        if (xQueueReceive(s_wmsg_queue, &m, wait) == pdTRUE) {
            handle_msg(&m);
        } else if (s_phase == WPHASE_CONNECTING) {
            apply_failure(FAIL_TRANSIENT); // no result within the timeout
        } else if (s_phase == WPHASE_BACKOFF) {
            if (++s_failed_passes >= WIFI_AUTHFAIL_AMNESTY_PASSES) {
                s_failed_passes = 0;
                clear_auth_failed_marks(); // let flagged networks retry fully -- self-heals a mis-flag
            }
            start_pass(); // backoff elapsed -- try the whole list again
        }
    }
}

// --- Event handler: just forwards to the reconnect task ---------------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        wmsg_t m = {.type = WMSG_DISCONNECTED, .reason = event->reason};
        xQueueSend(s_wmsg_queue, &m, 0);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
            xSemaphoreGive(s_status_mutex);
        }
        wmsg_t m = {.type = WMSG_GOT_IP};
        xQueueSend(s_wmsg_queue, &m, 0);
    }
}

static bool maybe_backoff_tx_power(void) {
    nvs_handle_t h;
    if (nvs_open(BROWNOUT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t count = 0;
    nvs_get_u8(h, "brn_count", &count);
    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        count++;
    } else {
        count = 0;
    }
    nvs_set_u8(h, "brn_count", count);
    nvs_commit(h);
    nvs_close(h);

    if (count >= BROWNOUT_BACKOFF_THRESHOLD) {
        printf("wifi_manager: %u consecutive brownout resets -- backing off to reduced TX power\n",
               (unsigned)count);
        return true;
    }
    return false;
}

bool wifi_manager_start(void) {
    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
        return false;
    }
    s_wmsg_queue = xQueueCreate(12, sizeof(wmsg_t));
    if (s_wmsg_queue == NULL) {
        return false;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }

    compute_device_names(); // s_host_unique / s_ap_ssid from the device id
    load_mdns_cfg();         // s_mdns_friendly (default on)

    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) {
        return false;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
                                         NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
                                         NULL);

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK) {
        return false;
    }

    load_nets();
    load_pin();

    if (esp_wifi_start() != ESP_OK) {
        return false;
    }

    // DHCP client hostname (default "espressif") -- lets the board resolve as
    // a plain name via unicast DNS on networks that register DHCP hostnames
    // (works across subnets where mDNS's link-local multicast can't). Also a
    // friendly name in the network's device list. Best-effort.
    if (s_sta_netif != NULL) {
        esp_err_t hn = esp_netif_set_hostname(s_sta_netif, s_host_unique);
        if (hn != ESP_OK) {
            printf("wifi_manager: set STA hostname failed: %s\n", esp_err_to_name(hn));
        }
    }

    if (maybe_backoff_tx_power()) {
        esp_wifi_set_max_tx_power(REDUCED_TX_POWER);
        s_status.tx_power_reduced = true;
    }

    // The reconnect task drives all STA connection attempts (auto-connect on
    // boot, retries, manual switches) -- it starts a pass immediately.
    xTaskCreate(wifi_task, "wifi_reconnect", 4096, NULL, 5, NULL);

    if (mdns_init() == ESP_OK && mdns_hostname_set(s_host_unique) == ESP_OK) {
        // Unique service instance name so a future single-page controller can
        // enumerate every board on the network and tell them apart.
        char inst[48];
        snprintf(inst, sizeof(inst), "Bluetooth Scanner (%s)", s_ap_ssid + strlen("BTScanner-"));
        mdns_instance_name_set(inst);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_ready = true;
        apply_friendly_mdns(); // add the shared btscanner.local alias if enabled
        printf("wifi_manager: mDNS hostname http://%s.local/%s\n", s_host_unique,
               s_mdns_friendly ? " (+ btscanner.local)" : "");
    } else {
        printf("wifi_manager: mDNS init failed (non-fatal, IP access still works)\n");
    }

    printf("wifi_manager: AP SSID \"%s\" password \"%s\", http://192.168.4.1/\n", s_ap_ssid,
           AP_PASSWORD);
    return true;
}

void wifi_manager_get_status(wifi_status_t *out) {
    // The LED-status poll task reads Wi-Fi status during early boot, which can be
    // before wifi_manager_start() has created the mutex (notably on a no-card
    // boot, where the SD mount timeout delays init). Report "nothing yet" instead
    // of asserting inside xSemaphoreTake(NULL). The mutex, once created, is never
    // destroyed, so this check only ever passes during that early window.
    if (s_status_mutex == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_status_mutex);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

int64_t wifi_manager_get_tsf_us(void) {
    // Only meaningful while associated -- an unassociated STA has no AP beacon to
    // discipline its TSF, so the value wouldn't be shared with anything. Plain
    // read of sta_state (an aligned enum) is fine here; a stale value at worst
    // returns 0 for one poll around a (dis)connect edge.
    if (s_status.sta_state != WIFI_STA_CONNECTED) {
        return 0;
    }
    return esp_wifi_get_tsf_time(WIFI_IF_STA);
}

void wifi_manager_get_hostname(char *out, size_t len) {
    snprintf(out, len, "%s", s_host_unique);
}

void wifi_manager_get_ap_ssid(char *out, size_t len) {
    snprintf(out, len, "%s", s_ap_ssid);
}

bool wifi_manager_get_ap_info(uint8_t bssid_out[6], int8_t *rssi_out) {
    wifi_ap_record_t info;
    if (s_status.sta_state == WIFI_STA_CONNECTED && esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        if (bssid_out) {
            memcpy(bssid_out, info.bssid, 6);
        }
        if (rssi_out) {
            *rssi_out = info.rssi;
        }
        return true;
    }
    if (bssid_out) {
        memset(bssid_out, 0, 6); // not associated -- no AP scope
    }
    return false;
}

void wifi_manager_get_bssid(uint8_t out[6]) {
    wifi_manager_get_ap_info(out, NULL);
}

bool wifi_manager_get_rssi(int8_t *out) {
    return wifi_manager_get_ap_info(NULL, out);
}

bool wifi_manager_get_mdns_friendly(void) {
    return s_mdns_friendly;
}

void wifi_manager_set_mdns_friendly(bool enabled) {
    s_mdns_friendly = enabled;
    nvs_handle_t h;
    if (nvs_open(NETS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mdns_friendly", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    apply_friendly_mdns(); // add or drop the btscanner.local alias immediately
}

int wifi_manager_get_saved(wifi_saved_info_t *out, int max) {
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int idxs[WIFI_MAX_SAVED];
        for (int i = 0; i < s_net_count; i++) {
            idxs[i] = i;
        }
        for (int i = 1; i < s_net_count; i++) { // used_seq desc (recent first)
            int k = idxs[i], j = i - 1;
            while (j >= 0 && s_nets[idxs[j]].used_seq < s_nets[k].used_seq) {
                idxs[j + 1] = idxs[j];
                j--;
            }
            idxs[j + 1] = k;
        }
        bool connected = s_status.sta_state == WIFI_STA_CONNECTED;
        for (int i = 0; i < s_net_count && n < max; i++) {
            wifi_net_t *net = &s_nets[idxs[i]];
            strncpy(out[n].ssid, net->ssid, sizeof(out[n].ssid) - 1);
            out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
            out[n].enterprise = net->enterprise != 0;
            out[n].connected = connected && strcmp(net->ssid, s_status.sta_ssid) == 0;
            out[n].auth_failed = net->auth_failed != 0;
            n++;
        }
        xSemaphoreGive(s_status_mutex);
    }
    return n;
}

static bool post_msg(const wmsg_t *m) {
    return s_wmsg_queue != NULL && xQueueSend(s_wmsg_queue, m, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool wifi_manager_connect(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    wmsg_t m = {.type = WMSG_CONNECT, .enterprise = 0};
    strncpy(m.ssid, ssid, sizeof(m.ssid) - 1);
    strncpy(m.pass, password != NULL ? password : "", sizeof(m.pass) - 1);
    m.user[0] = '\0';
    return post_msg(&m);
}

bool wifi_manager_connect_enterprise(const char *ssid, const char *username,
                                     const char *password) {
    if (ssid == NULL || ssid[0] == '\0' || username == NULL || username[0] == '\0') {
        return false;
    }
    wmsg_t m = {.type = WMSG_CONNECT, .enterprise = 1};
    strncpy(m.ssid, ssid, sizeof(m.ssid) - 1);
    strncpy(m.user, username, sizeof(m.user) - 1);
    strncpy(m.pass, password != NULL ? password : "", sizeof(m.pass) - 1);
    return post_msg(&m);
}

bool wifi_manager_select(const char *ssid) {
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    wmsg_t m = {.type = WMSG_SELECT};
    strncpy(m.ssid, ssid, sizeof(m.ssid) - 1);
    return post_msg(&m);
}

bool wifi_manager_forget(const char *ssid) {
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    wmsg_t m = {.type = WMSG_FORGET};
    strncpy(m.ssid, ssid, sizeof(m.ssid) - 1);
    return post_msg(&m);
}

bool wifi_manager_set_ap_pin(const char *ssid, const uint8_t bssid[6]) {
    if (ssid == NULL || ssid[0] == '\0' || bssid == NULL) {
        return false;
    }
    wmsg_t m = {.type = WMSG_SET_PIN};
    strncpy(m.ssid, ssid, sizeof(m.ssid) - 1);
    memcpy(m.bssid, bssid, sizeof(m.bssid));
    return post_msg(&m);
}

void wifi_manager_clear_ap_pin(void) {
    wmsg_t m = {.type = WMSG_CLEAR_PIN};
    post_msg(&m);
}

bool wifi_manager_get_ap_pin(char *ssid_out, size_t ssid_len, uint8_t bssid_out[6]) {
    bool set = false;
    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set = s_pin_set;
        if (set) {
            if (ssid_out != NULL && ssid_len > 0) {
                strncpy(ssid_out, s_pin_ssid, ssid_len - 1);
                ssid_out[ssid_len - 1] = '\0';
            }
            if (bssid_out != NULL) {
                memcpy(bssid_out, s_pin_bssid, sizeof(s_pin_bssid));
            }
        }
        xSemaphoreGive(s_status_mutex);
    }
    return set;
}

static int scan_result_cmp(const void *a, const void *b) {
    const wifi_scan_result_t *ra = (const wifi_scan_result_t *)a;
    const wifi_scan_result_t *rb = (const wifi_scan_result_t *)b;
    return (int)rb->rssi - (int)ra->rssi;
}

static int ap_scan_cmp(const void *a, const void *b) {
    return (int)((const wifi_ap_scan_t *)b)->rssi - (int)((const wifi_ap_scan_t *)a)->rssi;
}

int wifi_manager_scan_aps(const char *ssid, wifi_ap_scan_t *out, int max_results) {
    if (out == NULL || max_results <= 0) {
        return -1;
    }
    wifi_scan_config_t scan_config = {0};
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
        return -1;
    }
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }
    if (found > 32) {
        found = 32;
    }
    wifi_ap_record_t *records = calloc(found, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        return -1;
    }
    uint16_t actual = found;
    if (esp_wifi_scan_get_ap_records(&actual, records) != ESP_OK) {
        free(records);
        return -1;
    }
    int n = 0;
    for (int i = 0; i < actual && n < max_results; i++) {
        if (records[i].ssid[0] == '\0') {
            continue; // hidden
        }
        if (ssid != NULL && ssid[0] != '\0' &&
            strcmp((const char *)records[i].ssid, ssid) != 0) {
            continue; // filtered to one SSID
        }
        strncpy(out[n].ssid, (const char *)records[i].ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        memcpy(out[n].bssid, records[i].bssid, sizeof(out[n].bssid));
        out[n].rssi = records[i].rssi;
        out[n].channel = records[i].primary;
        out[n].secure = records[i].authmode != WIFI_AUTH_OPEN;
        wifi_auth_mode_t am = records[i].authmode;
        out[n].enterprise = am == WIFI_AUTH_ENTERPRISE || am == WIFI_AUTH_WPA2_ENTERPRISE ||
                            am == WIFI_AUTH_WPA3_ENT_192;
        n++;
    }
    free(records);
    qsort(out, n, sizeof(wifi_ap_scan_t), ap_scan_cmp);
    return n;
}

int wifi_manager_scan(wifi_scan_result_t *out, int max_results) {
    if (out == NULL || max_results <= 0) {
        return -1;
    }

    wifi_scan_config_t scan_config = {0};
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
        return -1;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }
    if (found > 32) {
        found = 32;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        return -1;
    }
    uint16_t actual = found;
    if (esp_wifi_scan_get_ap_records(&actual, records) != ESP_OK) {
        free(records);
        return -1;
    }

    wifi_scan_result_t *dedup = calloc(actual, sizeof(wifi_scan_result_t));
    if (dedup == NULL) {
        free(records);
        return -1;
    }
    int dedup_count = 0;
    for (int i = 0; i < actual; i++) {
        if (records[i].ssid[0] == '\0') {
            continue; // hidden network -- nothing to show/select, user types it manually
        }
        int existing = -1;
        for (int j = 0; j < dedup_count; j++) {
            if (strcmp(dedup[j].ssid, (const char *)records[i].ssid) == 0) {
                existing = j;
                break;
            }
        }
        if (existing >= 0) {
            if ((int8_t)records[i].rssi > dedup[existing].rssi) {
                dedup[existing].rssi = records[i].rssi;
            }
            continue;
        }
        strncpy(dedup[dedup_count].ssid, (const char *)records[i].ssid,
                sizeof(dedup[dedup_count].ssid) - 1);
        dedup[dedup_count].rssi = records[i].rssi;
        dedup[dedup_count].secure = records[i].authmode != WIFI_AUTH_OPEN;
        wifi_auth_mode_t am = records[i].authmode;
        dedup[dedup_count].enterprise = am == WIFI_AUTH_ENTERPRISE ||
                                        am == WIFI_AUTH_WPA2_ENTERPRISE ||
                                        am == WIFI_AUTH_WPA3_ENT_192;
        dedup_count++;
    }

    qsort(dedup, dedup_count, sizeof(wifi_scan_result_t), scan_result_cmp);

    int copy_count = dedup_count < max_results ? dedup_count : max_results;
    memcpy(out, dedup, copy_count * sizeof(wifi_scan_result_t));

    free(dedup);
    free(records);
    return copy_count;
}

bool wifi_manager_set_time_from_client(int64_t unix_ms) {
    if (unix_ms < 1577836800000LL || unix_ms > 4102444800000LL) {
        return false; // reject an obviously-wrong client clock (before 2020 / after 2100)
    }

    bool ntp_synced = false;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ntp_synced = s_status.ntp_synced;
        xSemaphoreGive(s_status_mutex);
    }
    if (ntp_synced) {
        return false;
    }

    struct timeval tv = {
        .tv_sec = (time_t)(unix_ms / 1000),
        .tv_usec = (suseconds_t)((unix_ms % 1000) * 1000),
    };
    return settimeofday(&tv, NULL) == 0;
}
