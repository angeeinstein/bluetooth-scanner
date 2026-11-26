#pragma once

#include "driver/i2c_master.h"

// Lazily initializes (once) and returns the shared I2C master bus on
// SCL=IO8 / SDA=IO9 (the charger bus). Returns NULL on failure.
i2c_master_bus_handle_t i2c_bus_get(void);
