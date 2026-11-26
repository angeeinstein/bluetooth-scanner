#include "i2c_bus.h"

#include "esp_log.h"
#include "pins.h"

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus = NULL;
static bool s_init_attempted = false;

i2c_master_bus_handle_t i2c_bus_get(void) {
    if (!s_init_attempted) {
        s_init_attempted = true;

        i2c_master_bus_config_t bus_config = {
            .i2c_port = -1,
            .sda_io_num = PIN_CHG_SDA,
            .scl_io_num = PIN_CHG_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            s_bus = NULL;
        }
    }
    return s_bus;
}
