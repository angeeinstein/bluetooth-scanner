#include "bq25628.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool bq25628_init(i2c_master_bus_handle_t bus, bq25628_t *out_chg) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ25628_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &out_chg->dev);
    return err == ESP_OK;
}

bool bq25628_read(bq25628_t *chg, uint8_t reg, uint8_t *buf, size_t len) {
    esp_err_t err = i2c_master_transmit_receive(chg->dev, &reg, 1, buf, len, 200);
    return err == ESP_OK;
}

static bool bq25628_write_u8(bq25628_t *chg, uint8_t reg, uint8_t val) {
    uint8_t payload[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(chg->dev, payload, sizeof(payload), 200);
    return err == ESP_OK;
}

bool bq25628_adc_enable(bq25628_t *chg) {
    // ADC_EN=1, ADC_RATE=0 (continuous), ADC_SAMPLE=00 (12-bit).
    if (!bq25628_write_u8(chg, BQ25628_REG_ADC_CONTROL, 0x80)) {
        return false;
    }

    // Wait for the first conversion (ADC_DONE_STAT in CHARGER_STATUS_0),
    // up to ~1s, then proceed regardless -- reads are still valid registers,
    // just possibly the power-on-reset value if conversion hasn't run yet.
    for (int i = 0; i < 20; i++) {
        uint8_t status0 = 0;
        if (bq25628_read(chg, BQ25628_REG_CHARGER_STATUS_0, &status0, 1) &&
            (status0 & BQ25628_STATUS0_ADC_DONE_STAT)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

// Reads a 16-bit ADC register (byte 0 = LSB, byte 1 = MSB -- confirmed
// against Adafruit_BusIO_Register's LSBFIRST default, which is what the
// reference driver these registers were ported from actually uses),
// right-shifts off the low reserved/sign-extension bits, and sign-extends
// per `sig_bits`.
//
// Note the callers' `sig_bits` choices: the genuinely-signed IBAT reader
// passes the field's exact width (14) so the top data bit is treated as a
// sign bit; the always-positive voltage readers (VBUS/VBAT/VSYS) pass
// sig_bits one wider than the actual field on purpose, so `sign_bit` lands
// above the real data and the value is never misread as negative -- i.e.
// forced-unsigned. That's why e.g. VBAT (a 15-bit field) passes 16.
static bool read_adc_raw(bq25628_t *chg, uint8_t reg, int shift, int sig_bits, int *out_raw) {
    uint8_t buf[2];
    if (!bq25628_read(chg, reg, buf, 2)) {
        return false;
    }
    uint16_t raw16 = ((uint16_t)buf[1] << 8) | buf[0];
    uint16_t val = raw16 >> shift;

    uint16_t sign_bit = 1u << (sig_bits - 1);
    int32_t signed_val;
    if (val & sign_bit) {
        signed_val = (int32_t)val - (1 << sig_bits);
    } else {
        signed_val = (int32_t)val;
    }
    *out_raw = signed_val;
    return true;
}

bool bq25628_read_vbus_mv(bq25628_t *chg, int *out_mv) {
    int raw;
    // 14-bit unsigned in bits 15:2, 3.97 mV/step.
    if (!read_adc_raw(chg, BQ25628_REG_VBUS_ADC, 2, 15, &raw)) {
        return false;
    }
    *out_mv = (int)(raw * 3.97f);
    return true;
}

bool bq25628_read_vbat_mv(bq25628_t *chg, int *out_mv) {
    int raw;
    // 15-bit unsigned in bits 15:1, 1.99 mV/step.
    if (!read_adc_raw(chg, BQ25628_REG_VBAT_ADC, 1, 16, &raw)) {
        return false;
    }
    *out_mv = (int)(raw * 1.99f);
    return true;
}

bool bq25628_read_vsys_mv(bq25628_t *chg, int *out_mv) {
    int raw;
    if (!read_adc_raw(chg, BQ25628_REG_VSYS_ADC, 1, 16, &raw)) {
        return false;
    }
    *out_mv = (int)(raw * 1.99f);
    return true;
}

bool bq25628_read_ibat_ma(bq25628_t *chg, int *out_ma) {
    int raw;
    // 14-bit 2's complement in bits 15:2, 4 mA/step.
    if (!read_adc_raw(chg, BQ25628_REG_IBAT_ADC, 2, 14, &raw)) {
        return false;
    }
    *out_ma = raw * 4;
    return true;
}

bool bq25628_read_ibus_ma(bq25628_t *chg, int *out_ma) {
    int raw;
    // 15-bit 2's complement in bits 15:1, 2 mA/step. Positive = current
    // flowing from VBUS into the system (the input draw).
    if (!read_adc_raw(chg, BQ25628_REG_IBUS_ADC, 1, 15, &raw)) {
        return false;
    }
    *out_ma = raw * 2;
    return true;
}

bool bq25628_read_tdie_c(bq25628_t *chg, float *out_c) {
    int raw;
    // 12-bit 2's complement in bits 11:0 (bits 15:12 reserved, read 0),
    // 0.5 degC/step.
    if (!read_adc_raw(chg, BQ25628_REG_TDIE_ADC, 0, 12, &raw)) {
        return false;
    }
    *out_c = raw * 0.5f;
    return true;
}

bool bq25628_read_status(bq25628_t *chg, uint8_t *status0, uint8_t *status1, uint8_t *fault0) {
    bool ok = true;
    ok &= bq25628_read(chg, BQ25628_REG_CHARGER_STATUS_0, status0, 1);
    ok &= bq25628_read(chg, BQ25628_REG_CHARGER_STATUS_1, status1, 1);
    ok &= bq25628_read(chg, BQ25628_REG_FAULT_STATUS_0, fault0, 1);
    return ok;
}

bool bq25628_read_vbus_stat(bq25628_t *chg, uint8_t *out_vbus_stat) {
    uint8_t status1;
    if (!bq25628_read(chg, BQ25628_REG_CHARGER_STATUS_1, &status1, 1)) {
        return false;
    }
    *out_vbus_stat = status1 & BQ25628_STATUS1_VBUS_STAT_MASK;
    return true;
}

bool bq25628_set_charge_current_ma(bq25628_t *chg, int ma) {
    if (ma < 40) {
        ma = 40;
    } else if (ma > 2000) {
        ma = 2000;
    }
    uint8_t steps = (uint8_t)(ma / 40); // 1..50, 40mA/step
    uint8_t reg02 = (uint8_t)((steps & 0x07) << 5);
    uint8_t reg03 = (uint8_t)((steps >> 3) & 0x07);
    return bq25628_write_u8(chg, BQ25628_REG_CHARGE_CURRENT_LIMIT_0, reg02) &&
           bq25628_write_u8(chg, BQ25628_REG_CHARGE_CURRENT_LIMIT_1, reg03);
}

bool bq25628_set_batfet_ctrl(bq25628_t *chg, bq25628_batfet_ctrl_t ctrl) {
    uint8_t val;
    if (!bq25628_read(chg, BQ25628_REG_CHARGER_CONTROL_2, &val, 1)) {
        return false;
    }
    // Set BATFET_CTRL (bits 1:0) and clear BATFET_DLY (bit 2) atomically in
    // one write, dropping the ship/shutdown delay from 12.5s to ~25ms. Doing
    // it in the same write as the command means the shortened delay is
    // guaranteed in effect the instant the command is latched, regardless of
    // the register's prior state.
    val = (uint8_t)((val & ~(0x03 | BQ25628_CTRL2_BATFET_DLY)) | ((uint8_t)ctrl & 0x03));
    return bq25628_write_u8(chg, BQ25628_REG_CHARGER_CONTROL_2, val);
}

bool bq25628_set_ts_ignore(bq25628_t *chg, bool ignore) {
    uint8_t val;
    if (!bq25628_read(chg, BQ25628_REG_NTC_CONTROL_0, &val, 1)) {
        return false;
    }
    if (ignore) {
        val |= BQ25628_NTC_CTRL0_TS_IGNORE;
    } else {
        val &= (uint8_t)~BQ25628_NTC_CTRL0_TS_IGNORE;
    }
    return bq25628_write_u8(chg, BQ25628_REG_NTC_CONTROL_0, val);
}

bool bq25628_get_ts_ignore(bq25628_t *chg, bool *out_ignore) {
    uint8_t val;
    if (!bq25628_read(chg, BQ25628_REG_NTC_CONTROL_0, &val, 1)) {
        return false;
    }
    *out_ignore = (val & BQ25628_NTC_CTRL0_TS_IGNORE) != 0;
    return true;
}

const char *bq25628_chg_stat_str(uint8_t status1) {
    switch ((status1 & BQ25628_STATUS1_CHG_STAT_MASK) >> BQ25628_STATUS1_CHG_STAT_SHIFT) {
        case 0: return "Not charging";
        case 1: return "Charging (CC)";
        case 2: return "Charging (CV)";
        case 3: return "Top-off";
        default: return "Unknown";
    }
}

bool bq25628_reset_watchdog(bq25628_t *chg) {
    uint8_t val;
    if (!bq25628_read(chg, BQ25628_REG_CHARGER_CONTROL_0, &val, 1)) {
        return false;
    }
    val |= BQ25628_CTRL0_WD_RST;
    return bq25628_write_u8(chg, BQ25628_REG_CHARGER_CONTROL_0, val);
}

const char *bq25628_ts_status_str(uint8_t fault0) {
    switch (fault0 & BQ25628_FAULT_TS_STAT_MASK) {
        case 0x00: return "Normal";
        case 0x01: return "Cold";
        case 0x02: return "Hot";
        case 0x03: return "Cool";
        case 0x04: return "Warm";
        case 0x05: return "Pre-cool";
        case 0x06: return "Pre-warm";
        case 0x07: return "Bias fault (check TS pads / 10k resistor)";
        default: return "Unknown";
    }
}
