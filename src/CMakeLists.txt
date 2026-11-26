idf_component_register(
    SRC_DIRS "." "drivers" "web"
    INCLUDE_DIRS "." "drivers" "web"
    REQUIRES
        app_update # esp_ota_ops -- OTA firmware update (see ota.c)
        driver     # gpio, i2c, spi, and the RMT TX driver (led_rgb.c / WS2812)

        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        fatfs
        esp_timer
        esp_http_server
        esp_http_client # internet firmware updates (ota_cloud.c) -- HTTPS GET
        mbedtls         # esp_crt_bundle_attach() for TLS server-cert verification
        esp_psram
        heap
        lwip
        mdns
        wpa_supplicant # WPA2/WPA3-Enterprise EAP client (esp_eap_client.h) --
                       # only a PRIV_REQUIRES of esp_wifi, so not transitively
                       # exposed; must be required directly here.
)
