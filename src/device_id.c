#include "device_id.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_mac.h"

void device_id_str(char *out, size_t len) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, len, "BTS-%02X%02X%02X", mac[3], mac[4], mac[5]);
}
