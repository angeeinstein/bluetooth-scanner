#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Internet firmware updates: pulls the latest build from a user-configured HTTPS
// endpoint (a Cloudflare Worker fronting the project's GitHub releases) and
// installs it through the shared ota.c core (ota_begin/write/end). The endpoint
// URL and the Cloudflare Access service-token headers are stored in NVS (set via
// the dashboard), never compiled in -- so they stay out of git and the .bin.
//
// The endpoint contract (see the worker):
//   GET <url>/version  -> {"version":"v1.2.3","filename":"...","size":1234,...}
//   GET <url>/firmware -> the raw firmware.bin (Content-Length set)
// Both requests carry: CF-Access-Client-Id / CF-Access-Client-Secret headers.

#define OTA_CLOUD_URL_MAX 128
#define OTA_CLOUD_ID_MAX 96
#define OTA_CLOUD_SECRET_MAX 128
#define OTA_CLOUD_VER_MAX 24

typedef struct {
    bool configured;                 // an endpoint URL is set in NVS
    bool update_available;           // the server offers a strictly newer version
    char current[OTA_CLOUD_VER_MAX]; // running firmware version (FIRMWARE_VERSION)
    char latest[OTA_CLOUD_VER_MAX];  // version the server reports
    uint32_t size;                   // bytes of the offered image
    char error[80];                  // empty on success, else a human reason
} ota_cloud_check_t;

// Reads the stored config for the dashboard's settings form. Returns true if an
// endpoint URL is configured. `has_secret` reports whether a secret is stored
// (the secret itself is never handed back out).
bool ota_cloud_get_config(char *url, size_t url_sz, char *client_id, size_t id_sz,
                          bool *has_secret);

// Persists the config to NVS. An empty/NULL `client_secret` leaves the stored
// secret unchanged (so re-saving the form without re-typing it is safe).
bool ota_cloud_set_config(const char *url, const char *client_id, const char *client_secret);

// Synchronously queries <url>/version and compares it (numeric semver) with the
// running firmware. Fills `out`; never blocks longer than a few seconds.
void ota_cloud_check(ota_cloud_check_t *out);

// Spawns a background task that downloads <url>/firmware into the inactive OTA
// slot and reboots on success. Byte progress is reported via ota_get_status();
// the connect phase and any error via ota_cloud_get_status(). Returns false
// immediately (reason in `err`) if it can't start.
bool ota_cloud_start_install(char *err, size_t err_sz);

// Live install state for the dashboard poll: `active` while a cloud install task
// is running, and the last install error (empty if none).
void ota_cloud_get_status(bool *active, char *err, size_t err_sz);

// True if an update-server URL is configured. Both the OTA check and the
// device-address reporting need one; a cheap NVS read.
bool ota_cloud_is_configured(void);

// POSTs `body` (a JSON string) to <configured base URL><path>, carrying the same
// Cloudflare Access service-token headers as the OTA calls (reuses the stored
// config + TLS bundle -- no separate auth or connection code). Retries a few
// times on transient transport failures (socket briefly unavailable, DNS hiccup,
// TLS reset); a non-200 HTTP status is a definitive answer and is returned as an
// error without retrying. Returns true only on HTTP 200. Performs a blocking TLS
// request, so call it only from a low-priority background task, never from a
// timing-critical path. `err` (may be NULL) receives a short reason on failure.
bool ota_cloud_post_json(const char *path, const char *body, char *err, size_t err_sz);
