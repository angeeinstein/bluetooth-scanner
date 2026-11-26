#include "led_rgb.h"

#include <stdio.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

// 10 MHz RMT resolution -> 0.1 us per tick, which divides the WS2812 bit
// timings cleanly.
#define LED_RGB_RES_HZ 10000000

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

bool led_rgb_init(int gpio_num) {
    if (s_chan != NULL) {
        return true; // already initialised
    }

    rmt_tx_channel_config_t chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz = LED_RGB_RES_HZ,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&chan_cfg, &s_chan) != ESP_OK) {
        printf("led_rgb: rmt_new_tx_channel failed\n");
        s_chan = NULL;
        return false;
    }

    // WS2812C bit cells at 0.1 us/tick (~1.25 us total per bit, 800 kHz):
    //   '0' = 0.3 us high + 0.9 us low   (3 + 9 ticks)
    //   '1' = 0.6 us high + 0.6 us low   (6 + 6 ticks)
    // Bits are sent MSB first.
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 6, .level1 = 0, .duration1 = 6},
        .flags = {.msb_first = 1},
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_encoder) != ESP_OK) {
        printf("led_rgb: rmt_new_bytes_encoder failed\n");
        return false;
    }

    if (rmt_enable(s_chan) != ESP_OK) {
        printf("led_rgb: rmt_enable failed\n");
        return false;
    }
    return true;
}

void led_rgb_set(uint8_t r, uint8_t g, uint8_t b) {
    if (s_chan == NULL || s_encoder == NULL) {
        return;
    }
    uint8_t grb[3] = {g, r, b}; // WS2812 wire order is green, red, blue
    rmt_transmit_config_t tx_cfg = {.loop_count = 0};
    if (rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx_cfg) == ESP_OK) {
        // Block until sent; the line then idles low, which latches the pixel
        // (the WS2812 "reset" gap). One pixel is ~30 us, so this is quick.
        rmt_tx_wait_all_done(s_chan, 50);
    }
}
