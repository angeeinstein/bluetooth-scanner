#pragma once

// Scan log: turns every BLE report into one CSV row (plus TARGET / MARK /
// STATUS / INFO / START / STOP rows) and feeds them to the SD session
// (recorder_log_line) and, optionally, the USB serial port. Also owns:
//   - target rules (name prefix / address / address prefix, in NVS)
//   - logging settings: auto-start at power-on, 100 % scan mode (Wi-Fi off
//     while logging), serial mirror
//   - a 1 Hz task: STATUS rows, clock-set detection, the Wi-Fi switching.
// The CSV schema and the on-card FORMAT.md both live in scan_log.c, so they
// can't drift apart.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recorder.h"

// Registers the scanner hooks, loads settings + rules, starts the 1 Hz task.
// Call after ble_scanner_start() and recorder_init().
void scan_log_init(void);

// Starts a session if auto-start is enabled (call once, late in boot).
void scan_log_maybe_autostart(void);

// --- Called by recorder.c -------------------------------------------------
const char *scan_log_csv_header(void); // first line of every CSV part
const char *scan_log_format_doc(void); // RECORDER_DIR/FORMAT.md contents
void scan_log_session_begin(const char *base); // writes the START row
void scan_log_session_end(const char *reason); // writes the STOP row
void scan_log_marker_event(const recorder_marker_t *m, const char *action); // "add" / "note" / "delete"

// --- Settings (persisted in NVS "bts_log") -----------------------------------
typedef struct {
    bool autostart;  // start logging at power-on
    bool wifi_off;   // 100 % scan mode: Wi-Fi off while logging
    bool serial;     // mirror every row to the USB serial port
} scan_log_settings_t;

void scan_log_get_settings(scan_log_settings_t *out);
void scan_log_set_autostart(bool on);
void scan_log_set_wifi_off(bool on);
void scan_log_set_serial(bool on);

// Seconds until Wi-Fi turns off after logging started in 100 % mode (gives
// the dashboard time to show what's happening); -1 if not pending.
int scan_log_wifi_off_in_s(void);

// --- Target rules (persisted in NVS "bts_targets") -----------------------------
// One rule per line: "name:<prefix>" (device name starts with, case-sensitive),
// "addr:<AA:BB:CC:DD:EE:FF>" (exact address), "prefix:<AA:BB:CC>" (address
// starts with). Blank lines and lines starting with '#' are ignored.
#define SCAN_LOG_RULES_MAX 1024 // bytes of rule text
#define SCAN_LOG_DEFAULT_RULES "name:XXZ\n"

// Copies the rule text. Returns its length.
size_t scan_log_get_rules(char *out, size_t out_len);
// Validates, stores and applies `text`. On a bad line returns false and puts a
// message into `err` (nothing is changed).
bool scan_log_set_rules(const char *text, char *err, size_t err_len);
