#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WIFI_STA_IDLE,
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
    WIFI_STA_FAILED,
} wifi_sta_state_t;

typedef struct {
    wifi_sta_state_t sta_state;
    char sta_ssid[33];
    char ip[16];
    int8_t rssi;
    bool ntp_synced;
    char sta_fail_reason[40];
    bool tx_power_reduced; // true if running at reduced TX power after repeated brownout resets
    bool enterprise;       // true if the joined/attempted network is WPA2/WPA3-Enterprise (EAP)
} wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
    bool enterprise; // WPA2/WPA3-Enterprise (needs a username + password, not a shared key)
} wifi_scan_result_t;

// One access point from a per-BSSID scan (wifi_manager_scan_aps). Unlike
// wifi_scan_result_t, duplicate SSIDs are NOT collapsed -- every physical AP of a
// mesh/roaming network is listed separately. For the "pin to a specific AP" UI.
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    bool secure;
    bool enterprise;
} wifi_ap_scan_t;

// One entry in the list of remembered networks (see wifi_manager_get_saved()).
typedef struct {
    char ssid[33];
    bool enterprise;
    bool connected;   // this is the currently-joined network
    bool auth_failed; // last attempt failed authentication (likely a changed password)
} wifi_saved_info_t;

#define WIFI_MAX_SAVED 8

// Brings up the SoftAP (fixed SSID/password, always on -- the dashboard
// stays reachable regardless of infra Wi-Fi state) and starts the background
// reconnect task, which auto-connects to whichever remembered network is in
// range (last-connected first). Call once at boot, after nvs_flash_init().
bool wifi_manager_start(void);

void wifi_manager_get_status(wifi_status_t *out);

// The 802.11 TSF timer (microseconds) of the STA interface. While associated,
// every station on the same AP disciplines this timer to the AP's beacon, so it
// is a clock *shared* across all devices on that AP -- accurate to ~tens of us,
// far better than NTP. The basis for time-aligning recordings from multiple
// devices (see the multi-device-sync design). Returns 0 when not associated
// (there is no AP to sync to, so no shared reference exists).
int64_t wifi_manager_get_tsf_us(void);

// This device's unique mDNS / DHCP hostname, e.g. "vibrashock-ccef4c" (reachable
// at <hostname>.local). Derived from the device id so multiple boards never
// collide. Every device always answers to this name.
void wifi_manager_get_hostname(char *out, size_t len);

// This device's unique SoftAP SSID, e.g. "VibraShock-CCEF4C".
void wifi_manager_get_ap_ssid(char *out, size_t len);

// The BSSID (MAC) of the AP the STA is associated to, or all-zero if not
// associated. The TSF clock is shared only among devices on the *same* BSSID, so
// recordings store this to let the analysis tool verify same-AP before aligning.
void wifi_manager_get_bssid(uint8_t out[6]);

// Live signal strength (dBm) of the AP the STA is associated to, read from the
// driver's cached AP info (no radio traffic). False if not associated.
// (wifi_status_t.rssi is never refreshed after connecting -- use this instead.)
bool wifi_manager_get_rssi(int8_t *out);

// Both of the above from one driver call (one consistent snapshot). Either
// pointer may be NULL. False if not associated (bssid_out is then zeroed).
bool wifi_manager_get_ap_info(uint8_t bssid_out[6], int8_t *rssi_out);

// Optional "pin to a specific access point": force the STA to connect to exactly
// this BSSID whenever it joins `ssid`, instead of the strongest one. This is a
// SOFT pin -- if that exact AP isn't reachable, the device falls back to a normal
// any-BSSID connect, so a pin can never strand it and Wi-Fi behaves normally
// otherwise. Used to put several devices on one AP for time-aligned recordings
// (shared 802.11 TSF). Only one pin exists at a time; persisted across reboots.
// Setting a pin triggers a reconnect so it applies immediately.
bool wifi_manager_set_ap_pin(const char *ssid, const uint8_t bssid[6]);
void wifi_manager_clear_ap_pin(void);
// If a pin is set, returns true and fills the outputs (either may be NULL).
bool wifi_manager_get_ap_pin(char *ssid_out, size_t ssid_len, uint8_t bssid_out[6]);

// Blocking active scan (~1-2s) returning individual APs -- one entry per BSSID
// (mesh APs of the same SSID are NOT collapsed). If `ssid` is non-NULL and
// non-empty, only APs of that SSID are returned; otherwise all. Sorted by
// descending RSSI. Fills up to `max_results`; returns the count, or -1 on failure.
int wifi_manager_scan_aps(const char *ssid, wifi_ap_scan_t *out, int max_results);

// Whether this device also answers to the shared friendly name "vibrashock.local"
// (in addition to its unique hostname). Persisted; default on. With several
// devices on one network, leave it on for only the "main" one so vibrashock.local
// unambiguously reaches that board.
bool wifi_manager_get_mdns_friendly(void);
void wifi_manager_set_mdns_friendly(bool enabled);

// Fills `out` with the remembered networks (most-recently-used first), up to
// `max`. Returns the number written. Thread-safe.
int wifi_manager_get_saved(wifi_saved_info_t *out, int max);

// Remembers (adds or updates) a normal WPA2/WPA3-Personal network and makes
// it the active connection target. If it's already saved, this just updates
// its password -- the way to fix a network whose password changed. Returns
// immediately; the connection happens asynchronously in the reconnect task.
bool wifi_manager_connect(const char *ssid, const char *password);

// Connects to an already-remembered network by SSID (a manual switch). No
// effect if the SSID isn't in the saved list.
bool wifi_manager_select(const char *ssid);

// Removes a remembered network. If it's the currently-connected one, the
// device disconnects and the reconnect task falls through to the others.
bool wifi_manager_forget(const char *ssid);

// Notes that the device is being actively used (called by the dashboard's
// status poll). While active, the reconnect task uses a longer escalating
// backoff between failed connection passes to keep the SoftAP smooth; when
// idle, it retries quickly. Cheap -- just stamps a timestamp.
void wifi_manager_note_activity(void);

// Same as wifi_manager_connect() but for a WPA2/WPA3-Enterprise (EAP)
// network -- a per-user `username` + `password` (e.g. a university login)
// instead of one shared key. The username is used as both the outer EAP
// identity and the inner (phase-2) username, covering the common
// PEAP/TTLS-MSCHAPv2 case with no separate anonymous identity. The server
// certificate is NOT validated (no CA cert configured) -- simplest and
// matches how a typical phone is set up for these networks, at the cost
// that a rogue AP with the same SSID could capture the credentials. Both
// the username and password persist to NVS (plaintext, same as the
// personal password) and are re-applied on the next boot.
bool wifi_manager_connect_enterprise(const char *ssid, const char *username,
                                     const char *password);

// Blocking active scan (typically ~1-2s) for nearby APs. Fills up to
// max_results entries in `out`, sorted by descending RSSI with duplicate
// SSIDs (multiple BSSIDs of the same network) collapsed to their strongest
// entry. Returns the number of entries written, or -1 on failure.
int wifi_manager_scan(wifi_scan_result_t *out, int max_results);

// Sets the system wall clock from a client-supplied Unix time in
// milliseconds -- a fallback for when NTP isn't reachable (AP-only mode, or
// infra Wi-Fi with no internet uplink). No-op (returns false) if NTP has
// already synced the clock, since NTP is strictly more accurate and keeps
// itself correct via its own periodic re-sync; also rejects implausible
// values (before 2020 or after 2100) to avoid a broken client clock
// corrupting recording timestamps. Returns true if the clock was set.
bool wifi_manager_set_time_from_client(int64_t unix_ms);
