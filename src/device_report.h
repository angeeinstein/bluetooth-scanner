#pragma once

#include <stdbool.h>

// Device-address reporting. Periodically POSTs this board's current local IPv4
// address and a little status to the configured update server (the Cloudflare
// Worker, reusing the OTA endpoint URL + service-token headers -- no separate
// auth or connection code), so the public /find/<device_id> lookup can point a
// browser straight at http://<local-ip>/.
//
// Sends a report whenever the station gets a (new) IP -- first connect and any
// later DHCP/AP change -- and on a ~5-minute heartbeat while online. All of it
// runs on one low-priority background task that only reads existing state
// (wifi_manager status, battery, recording) and does a blocking TLS POST, so it
// never touches the timing-critical sampling/SD path. A no-op while no update
// server is configured; failures are logged and retried with backoff, never
// fatal. Call once at boot, after wifi_manager_start().
void device_report_start(void);

// Whether reporting is enabled. Persisted to NVS; default on. When off, the
// reporter stays idle (no POSTs at all), regardless of whether an update server
// is configured. Toggled from the dashboard (Settings -> Wi-Fi).
bool device_report_get_enabled(void);
void device_report_set_enabled(bool enabled);

// Cooperatively stops the reporter task, freeing its ~8 KB stack. The task exits
// at a safe point in its loop (holding no locks), so this is safe to call from
// another task; poll device_report_is_running() to wait until it has actually
// exited. device_report_start() brings it back. Used to free internal RAM for
// the OTA download's TLS buffers on memory-tight boards (the reporter is idle
// during an OTA anyway).
void device_report_request_stop(void);
bool device_report_is_running(void);
