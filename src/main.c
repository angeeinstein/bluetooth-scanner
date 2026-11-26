// BTScanner firmware: Bluetooth (BLE) scanner / logger for the ESP32-S3 board
// originally built as the VibraShock vibration recorder (same hardware). Scans
// BLE continuously, logs every report to the SD card, derives a motion profile
// of the board itself from the IIS3DWB accelerometer, and serves a web
// dashboard (devices, logging, analysis, Wi-Fi, charger, firmware update).

#include <stdio.h>

#include "accel_capture.h"
#include "ble_scanner.h"
#include "motion.h"
#include "device_id.h"
#include "device_report.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "nvs_flash.h"
#include "ota.h"
#include "power_ctrl.h"
#include "recorder.h"
#include "scan_log.h"
#include "sync_trigger.h"
#include "udp_stream.h"
#include "version.h"
#include "wifi_manager.h"
#include "ws_stream.h"

#include "dashboard.h"

static void print_boot_info(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char devid[16];
    device_id_str(devid, sizeof(devid));
    printf("\n=============================================================\n");
    printf(" %s v%s\n", FIRMWARE_NAME, FIRMWARE_VERSION);
    printf("   Device ID: %s   (built %s)\n", devid, FIRMWARE_BUILD);
    printf("   Chip: ESP32-S3, %d cores @ %d MHz\n", chip.cores, esp_clk_cpu_freq() / 1000000);
    printf("   By %s\n", FIRMWARE_AUTHOR);
    printf("=============================================================\n");
}

// Internal RAM is the tight resource (Wi-Fi + BLE + task stacks): print it at
// each boot stage, so a "task create failed" can be traced to its cause.
static void heap_mark(const char *stage) {
    printf("main: [%s] internal heap free %u, largest block %u, PSRAM free %u\n", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void) {
    print_boot_info();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (!accel_capture_start()) {
        printf("main: accelerometer capture failed to start -- check wiring\n");
    }

    if (!recorder_init()) {
        printf("main: SD card mount failed -- recording will be unavailable\n");
    }
    recorder_start_writer_task();
    heap_mark("sensor + SD");

    if (!power_ctrl_start()) {
        printf("main: charger/power control failed to start -- check I2C wiring\n");
    }

    if (!wifi_manager_start()) {
        printf("main: Wi-Fi failed to start\n");
    }
    heap_mark("Wi-Fi");

    // After Wi-Fi: the radio is shared (coexistence), and Wi-Fi brings up the
    // coexistence arbiter first. Warn-and-continue like every init above.
    if (!ble_scanner_start()) {
        printf("main: BLE scanner failed to start\n");
    }
    scan_log_init(); // CSV rows, target rules, 100 % scan mode (works without BLE/SD too)
    motion_start();  // motion profile from the accelerometer (skipped if it's missing)
    heap_mark("BLE + logging + motion");

    device_report_start(); // periodic local-IP reporting to the update server (best-effort)

    sync_trigger_start(); // external TSF-latch input on IO38 (best-effort)

    if (!udp_stream_init()) {
        printf("main: UDP streaming failed to start\n");
    }

    if (!ws_stream_init()) {
        printf("main: WebSocket live-stream failed to start\n");
    }

    heap_mark("before dashboard");
    dashboard_start();
    heap_mark("dashboard");
    power_ctrl_signal_ready();
    scan_log_maybe_autostart(); // optional "log from power-on" (skipped without an SD card)

    // Boot ran to completion -- accept this firmware image, cancelling the OTA
    // rollback that would otherwise revert on the next reboot. Reaching this
    // line *is* the "firmware works" signal; only a crash/panic before here
    // triggers a rollback. IMPORTANT: every init above degrades gracefully on
    // absent hardware (a missing SD card or a detached accelerometer warn and
    // continue, they do not abort) -- keep it that way, so a missing peripheral
    // can never be mistaken for a bad update and roll a good image back.
    ota_mark_valid();
}
