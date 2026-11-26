#pragma once

// Motion profile of the device itself, from the IIS3DWB accelerometer only
// (no gyroscope, no magnetometer -- so no absolute heading, and integrating
// acceleration would drift within seconds). Best effort, built to answer
// "did it stand still or move around, and roughly how":
//
//   - gravity estimate (1 s low-pass) -> tilt, and the dynamic acceleration
//   - activity (RMS of dynamic acceleration per second) -> still / moving
//   - walking: a steady rhythm in the vertical component (autocorrelation
//     over the last 4 s) -> cadence; steps and distance accrue from the
//     cadence while walking; step length (Weinberg) -> walking speed
//   - main horizontal axis of the movement in the device frame (180 deg
//     ambiguous) and how clear it is
//
// Input: accel_capture's ~100 Hz feed. Output: a 1 Hz state (dashboard,
// MOTION rows in the scan log), a 10-minute history, and -- while logging --
// every 100 Hz sample in <base>.motion.csv.

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MOTION_UNKNOWN = 0, // not enough data yet / sensor missing
    MOTION_STILL = 1,
    MOTION_MOVING = 2,  // moving, but not a walking rhythm (handling, vehicle, ...)
    MOTION_WALKING = 3,
} motion_state_t;

typedef struct {
    bool ok;               // receiving samples
    motion_state_t state;
    float rate_hz;         // sample rate of the 100 Hz feed
    float activity_g;      // RMS of the dynamic acceleration over the last second
    float peak_g;          // largest dynamic acceleration in the last second
    uint32_t steps;        // since boot
    float cadence_spm;     // steps per minute (0 = no rhythm)
    float step_m;          // average estimated step length of the last steps
    float speed_mps;       // walking speed; 0 when still; -1 = moving but unknown
    float dist_m;          // walked distance since boot (from steps)
    float rhythm;          // 0..1, how strongly the vertical motion repeats over the last 4 s
                           // at a step/stride period (walking ~0.6-0.95, vehicle/handling ~0.2)
    float axis_deg;        // main horizontal movement axis, 0..180 deg from the device's
                           // X axis projected onto the horizontal plane; -1 = none
    float axis_conf;       // 0..1, how clearly the movement follows that axis
    float tilt_deg;        // angle between the device's +Z axis and straight up
    float gx, gy, gz;      // gravity direction (unit vector, device frame)
    uint32_t dropped;      // samples lost before processing
} motion_status_t;

// Applies the motion settings to the accelerometer (range, filter off) and
// starts the processing task. Call after accel_capture_start().
void motion_start(void);

void motion_get_status(motion_status_t *out);
const char *motion_state_name(motion_state_t s);

// 1 Hz history, oldest first, up to MOTION_HIST_SECONDS: activity in mg,
// speed in cm/s (-1 unknown), state, cadence in steps/min. Returns the count.
#define MOTION_HIST_SECONDS 600
int motion_get_history(uint16_t *activity_mg, int16_t *speed_cms, uint8_t *state, uint8_t *cadence,
                       int max);

// Settings (NVS "bts_motion" -- separate from VibraShock's "accel_cfg").
bool motion_log_enabled(void);       // write <base>.motion.csv during sessions (default on)
void motion_set_log_enabled(bool on);
int motion_get_range_g(void);        // 2 / 4 / 8 / 16 (default 4)
bool motion_set_range_g(int g);
