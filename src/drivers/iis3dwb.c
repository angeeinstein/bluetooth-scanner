#include "iis3dwb.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "pins.h"

// Scratch TX/RX buffers for FIFO burst reads, sized for a full-FIFO drain
// (worst case): 1 address byte + up to 512 entries * 7 bytes. DMA-capable
// (internal RAM, not PSRAM) and shared across calls -- allocated once,
// lazily, in iis3dwb_init(). TX byte 0 is fixed (FIFO_DATA_OUT_TAG|0x80);
// the rest of TX is dummy zero bytes clocked out while we read the
// response. RX byte 0 is a throwaway echo; the payload starts at byte 1.
#define FIFO_SCRATCH_SIZE (1 + IIS3DWB_FIFO_DEPTH * IIS3DWB_FIFO_ENTRY_BYTES)
static uint8_t *s_fifo_tx_scratch = NULL;
static uint8_t *s_fifo_rx_scratch = NULL;

bool iis3dwb_init(spi_host_device_t host, iis3dwb_t *out_dev) {
    spi_device_interface_config_t devcfg = {
        .mode = 0, // CPOL=0, CPHA=0 (mode 3 also acceptable per datasheet)
        .clock_speed_hz = 8000000, // 8 MHz -- under the sensor's 10 MHz hard limit,
                                    // fast enough that a full FIFO-watermark burst
                                    // takes well under 2ms.
        .spics_io_num = PIN_ACCEL_CS,
        .queue_size = 2,
    };
    esp_err_t err = spi_bus_add_device(host, &devcfg, &out_dev->spi);
    if (err != ESP_OK) {
        return false;
    }

    if (s_fifo_tx_scratch == NULL) {
        s_fifo_tx_scratch = heap_caps_calloc(1, FIFO_SCRATCH_SIZE, MALLOC_CAP_DMA);
        s_fifo_rx_scratch = heap_caps_calloc(1, FIFO_SCRATCH_SIZE, MALLOC_CAP_DMA);
        if (s_fifo_tx_scratch == NULL || s_fifo_rx_scratch == NULL) {
            return false;
        }
        s_fifo_tx_scratch[0] = (uint8_t)(IIS3DWB_REG_FIFO_DATA_OUT_TAG | 0x80);
    }
    return true;
}

bool iis3dwb_read_reg(iis3dwb_t *dev, uint8_t reg, uint8_t *val) {
    uint8_t tx[2] = {(uint8_t)(reg | 0x80), 0x00};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 8 * 2,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(dev->spi, &t);
    if (err != ESP_OK) {
        return false;
    }
    *val = rx[1];
    return true;
}

bool iis3dwb_write_reg(iis3dwb_t *dev, uint8_t reg, uint8_t val) {
    uint8_t tx[2] = {(uint8_t)(reg & 0x7F), val};
    spi_transaction_t t = {
        .length = 8 * 2,
        .tx_buffer = tx,
        .rx_buffer = NULL,
    };
    esp_err_t err = spi_device_polling_transmit(dev->spi, &t);
    return err == ESP_OK;
}

float iis3dwb_sensitivity_mg_per_lsb(iis3dwb_fs_t fs) {
    switch (fs) {
        case IIS3DWB_FS_2G: return 0.061f;
        case IIS3DWB_FS_4G: return 0.122f;
        case IIS3DWB_FS_8G: return 0.244f;
        case IIS3DWB_FS_16G: return 0.488f;
        default: return 0.061f;
    }
}

int iis3dwb_hpcf_cutoff_hz(iis3dwb_hpcf_t hpcf) {
    // Approximate -3 dB points at the fixed 26.7 kHz ODR (ODR/4 is analog-limited
    // to the sensor's ~6.3 kHz bandwidth rather than the literal 6.67 kHz).
    static const int hz[] = {6300, 2700, 1300, 590, 270, 133, 67, 33};
    if ((unsigned)hpcf < sizeof(hz) / sizeof(hz[0])) {
        return hz[hpcf];
    }
    return 0;
}

bool iis3dwb_configure(iis3dwb_t *dev, iis3dwb_fs_t fs, iis3dwb_filter_mode_t filter,
                       iis3dwb_hpcf_t cutoff) {
    bool ok = true;
    // CTRL3_C: BDU=1 (bit6), IF_INC=1 (bit2) -- freeze registers during
    // multi-byte reads, auto-increment address on burst reads.
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_CTRL3_C, 0x44);
    // CTRL8_XL: HPCF_XL[2:0] (bits7:5) sets the second-stage cutoff; HP_SLOPE_XL_EN
    // (bit2) picks high-pass vs low-pass. Only meaningful when LPF2 is enabled
    // below, but always writing it keeps the register deterministic.
    uint8_t ctrl8_xl = ((uint8_t)cutoff << 5) |
                       (filter == IIS3DWB_FILTER_HIGHPASS ? IIS3DWB_CTRL8_HP_SLOPE_EN : 0x00);
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_CTRL8_XL, ctrl8_xl);
    // CTRL1_XL: xl_en=101 (bits7:5, 26.7kHz -- the only ODR this sensor
    // supports), fs_xl (bits3:2), lpf2_xl_en (bit1 -- routes data through the
    // second stage; off => LPF1 only).
    uint8_t ctrl1_xl =
        0xA0 | ((uint8_t)fs << 2) | (filter != IIS3DWB_FILTER_OFF ? 0x02 : 0x00);
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_CTRL1_XL, ctrl1_xl);
    return ok;
}

bool iis3dwb_enable_drdy_interrupt(iis3dwb_t *dev) {
    return iis3dwb_write_reg(dev, IIS3DWB_REG_INT1_CTRL, 0x01);
}

bool iis3dwb_data_ready(iis3dwb_t *dev, bool *ready) {
    uint8_t status;
    if (!iis3dwb_read_reg(dev, IIS3DWB_REG_STATUS_REG, &status)) {
        return false;
    }
    *ready = (status & 0x01) != 0;
    return true;
}

bool iis3dwb_read_xyz_g(iis3dwb_t *dev, iis3dwb_fs_t fs, float *x, float *y, float *z) {
    uint8_t tx[7] = {(uint8_t)(IIS3DWB_REG_OUTX_L_A | 0x80), 0, 0, 0, 0, 0, 0};
    uint8_t rx[7] = {0};
    spi_transaction_t t = {
        .length = 8 * 7,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(dev->spi, &t);
    if (err != ESP_OK) {
        return false;
    }

    int16_t raw_x = (int16_t)((rx[2] << 8) | rx[1]);
    int16_t raw_y = (int16_t)((rx[4] << 8) | rx[3]);
    int16_t raw_z = (int16_t)((rx[6] << 8) | rx[5]);

    float sens = iis3dwb_sensitivity_mg_per_lsb(fs);
    *x = raw_x * sens / 1000.0f;
    *y = raw_y * sens / 1000.0f;
    *z = raw_z * sens / 1000.0f;
    return true;
}

bool iis3dwb_read_temp_c(iis3dwb_t *dev, float *out_c) {
    uint8_t lo, hi;
    if (!iis3dwb_read_reg(dev, IIS3DWB_REG_OUT_TEMP_L, &lo) ||
        !iis3dwb_read_reg(dev, IIS3DWB_REG_OUT_TEMP_H, &hi)) {
        return false;
    }
    int16_t raw = (int16_t)(((uint16_t)hi << 8) | lo);
    // 256 LSB/degC, 0 LSB at 25 degC (per the datasheet temperature-sensor
    // characteristics).
    *out_c = 25.0f + (float)raw / 256.0f;
    return true;
}

bool iis3dwb_enable_timestamp(iis3dwb_t *dev, bool enable) {
    uint8_t ctrl10;
    if (!iis3dwb_read_reg(dev, IIS3DWB_REG_CTRL10_C, &ctrl10)) {
        return false;
    }
    if (enable) {
        ctrl10 |= IIS3DWB_CTRL10_TIMESTAMP_EN;
    } else {
        ctrl10 &= (uint8_t)~IIS3DWB_CTRL10_TIMESTAMP_EN;
    }
    return iis3dwb_write_reg(dev, IIS3DWB_REG_CTRL10_C, ctrl10);
}

bool iis3dwb_read_timestamp(iis3dwb_t *dev, uint32_t *out_ticks) {
    // Burst-read the 4 timestamp bytes in one transaction (IF_INC auto-increment
    // set in iis3dwb_configure) so all four come from the same counter value.
    uint8_t tx[5] = {(uint8_t)(IIS3DWB_REG_TIMESTAMP0 | 0x80), 0, 0, 0, 0};
    uint8_t rx[5] = {0};
    spi_transaction_t t = {
        .length = 8 * 5,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_polling_transmit(dev->spi, &t) != ESP_OK) {
        return false;
    }
    *out_ticks = (uint32_t)rx[1] | ((uint32_t)rx[2] << 8) | ((uint32_t)rx[3] << 16) |
                 ((uint32_t)rx[4] << 24);
    return true;
}

// --- FIFO capture ---------------------------------------------------------

bool iis3dwb_fifo_configure(iis3dwb_t *dev, uint16_t watermark_entries) {
    if (watermark_entries >= IIS3DWB_FIFO_DEPTH) {
        watermark_entries = IIS3DWB_FIFO_DEPTH - 1;
    }
    bool ok = true;
    // FIFO_CTRL4: fifo_mode = 000 (BYPASS) first -- this is the sensor's own
    // FIFO-flush mechanism. The accelerometer has its own power rail and
    // isn't reset by the ESP32 reflashing/rebooting, so on every new boot
    // its FIFO can already be full/overrun from a previous run that nobody
    // ever drained. Flushing here guarantees a clean, empty FIFO before we
    // arm the watermark, regardless of whatever state the chip was already
    // in when this boot started.
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_FIFO_CTRL4, 0x00);
    // FIFO_CTRL1: WTM[7:0].
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_FIFO_CTRL1, (uint8_t)(watermark_entries & 0xFF));
    // FIFO_CTRL2: bit0 = WTM[8], bit7 = stop_on_wtm (0 = keep streaming past
    // the watermark rather than stopping, we drain continuously).
    uint8_t wtm_msb = (uint8_t)((watermark_entries >> 8) & 0x01);
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_FIFO_CTRL2, wtm_msb);
    // FIFO_CTRL3: bdr_xl = 1010 (batch every sample at 26.7kHz into the FIFO).
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_FIFO_CTRL3, 0x0A);
    // FIFO_CTRL4: fifo_mode = 110 (STREAM_MODE -- continuous, overwrites
    // oldest entries on overflow rather than latching stopped).
    ok &= iis3dwb_write_reg(dev, IIS3DWB_REG_FIFO_CTRL4, 0x06);
    return ok;
}

bool iis3dwb_fifo_enable_watermark_int(iis3dwb_t *dev) {
    return iis3dwb_write_reg(dev, IIS3DWB_REG_INT1_CTRL, IIS3DWB_INT1_FIFO_TH);
}

bool iis3dwb_fifo_status(iis3dwb_t *dev, uint16_t *out_num_entries, bool *out_overrun) {
    uint8_t status1, status2;
    if (!iis3dwb_read_reg(dev, IIS3DWB_REG_FIFO_STATUS1, &status1)) {
        return false;
    }
    if (!iis3dwb_read_reg(dev, IIS3DWB_REG_FIFO_STATUS2, &status2)) {
        return false;
    }
    *out_num_entries = (uint16_t)status1 | (((uint16_t)(status2 & 0x03)) << 8);
    *out_overrun = (status2 & IIS3DWB_FIFO_STATUS2_OVR_IA) != 0;
    return true;
}

bool iis3dwb_fifo_burst_read(iis3dwb_t *dev, uint16_t num_entries, uint8_t *out_buf) {
    if (s_fifo_tx_scratch == NULL || s_fifo_rx_scratch == NULL || num_entries == 0) {
        return false;
    }
    if (num_entries > IIS3DWB_FIFO_DEPTH) {
        num_entries = IIS3DWB_FIFO_DEPTH;
    }

    size_t payload_bytes = (size_t)num_entries * IIS3DWB_FIFO_ENTRY_BYTES;
    size_t total_bytes = 1 + payload_bytes;

    spi_transaction_t t = {
        .length = 8 * total_bytes,
        .tx_buffer = s_fifo_tx_scratch,
        .rx_buffer = s_fifo_rx_scratch,
    };
    // Blocking but interrupt-driven (not busy-polling): this transfer can
    // take up to ~1.8ms at the full watermark, and this task shares its
    // core with the SD writer task -- letting the scheduler run other
    // tasks while the DMA completes is preferable to a tight poll loop.
    esp_err_t err = spi_device_transmit(dev->spi, &t);
    if (err != ESP_OK) {
        return false;
    }
    memcpy(out_buf, s_fifo_rx_scratch + 1, payload_bytes);
    return true;
}
