#pragma once

// Firmware identity, shown in the dashboard's About card and printed to the
// boot serial log. Single source of truth -- bump FIRMWARE_VERSION per release.
// FIRMWARE_BUILD is the compiler's date/time for the translation unit that
// references it (dashboard.c / main.c), so it refreshes on a clean build or
// whenever this header is touched (e.g. a version bump).
#define FIRMWARE_NAME "VibraShock Recorder"
#define FIRMWARE_VERSION "1.6.0"
#define FIRMWARE_AUTHOR "Angelo Popovic"
#define FIRMWARE_BUILD (__DATE__ " " __TIME__)
