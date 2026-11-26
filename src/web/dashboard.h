#pragma once

// Starts the HTTP dashboard (status polling API + WebSocket live-graph push
// + recording/config/charger/Wi-Fi control endpoints). Call once at boot,
// after wifi_manager_start() (needs the AP/STA up) and after
// accel_capture_start()/recorder_init()/power_ctrl_start() (status queries
// read from all of them).
void dashboard_start(void);
