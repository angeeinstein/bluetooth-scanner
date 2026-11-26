// Minimal register-level driver for the TI BQ25628 1-cell LiPo charger.
//
// Register map, ADC bit layout, and LSB scale factors are taken from
// Texas Instruments' own reference driver (Adafruit_BQ25628E, MIT licensed,
// https://github.com/adafruit/Adafruit_bq25628e) -- the BQ25628E shares the
// same I2C register map as the BQ25628 used on this board.
//
// SELFTEST_SPEC.md states the address as 0x6B, but the part actually
// populated on this board answers at 0x6A (confirmed by the I2C scan on
// real hardware) -- the same default as the Adafruit_BQ25628E breakout.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#define BQ25628_I2C_ADDR 0x6A

#define BQ25628_REG_CHARGE_CURRENT_LIMIT_0 0x02 // low byte: ICHG[2:0] in bits 7:5
#define BQ25628_REG_CHARGE_CURRENT_LIMIT_1 0x03 // high byte: ICHG[5:3] in bits 2:0
#define BQ25628_REG_CHARGER_CONTROL_0 0x16
#define BQ25628_REG_CHARGER_CONTROL_2 0x18
#define BQ25628_REG_NTC_CONTROL_0    0x1A
#define BQ25628_REG_CHARGER_STATUS_0 0x1D
#define BQ25628_REG_CHARGER_STATUS_1 0x1E
#define BQ25628_REG_FAULT_STATUS_0   0x1F

// NTC_CONTROL_0 bit 7: TS_IGNORE. Set to bypass TS (thermistor) monitoring
// entirely when no NTC is populated -- without it, a floating/open TS pin
// reads as a bias fault and the charger blocks charging. Verified against
// Adafruit_BQ25628E's setIgnoreThermistor()/getIgnoreThermistor(), which
// manipulate this exact register/bit.
#define BQ25628_NTC_CTRL0_TS_IGNORE (1 << 7)

// CHARGER_CONTROL_0 bit 2: WD_RST. Per the BQ25628/BQ25629 datasheet
// (Sec 8.4.1), the first I2C write after POR starts a 50s watchdog
// (CHARGER_CONTROL_0 bits 1:0, POR default 01b = 50s); if it isn't reset by
// writing 1 to WD_RST before it expires, the charger reverts to "default
// mode" and several registers snap back to their POR values -- including
// NTC_CONTROL_0's TS_IGNORE and ADC_CONTROL's ADC_EN. This bit self-clears
// back to 0 once the reset takes effect.
#define BQ25628_CTRL0_WD_RST (1 << 2)
#define BQ25628_REG_ADC_CONTROL      0x26
#define BQ25628_REG_IBUS_ADC         0x28
#define BQ25628_REG_IBAT_ADC         0x2A
#define BQ25628_REG_VBUS_ADC         0x2C
#define BQ25628_REG_VBAT_ADC         0x30
#define BQ25628_REG_VSYS_ADC         0x32
#define BQ25628_REG_TDIE_ADC         0x36 // die/junction temperature, 12-bit signed, 0.5 degC/LSB
#define BQ25628_REG_PART_INFORMATION 0x38

#define BQ25628_STATUS0_ADC_DONE_STAT (1 << 6)
// CHARGER_STATUS_0 (already fetched by bq25628_read_status() as *status0),
// the three "why isn't it charging at full current" regulation flags:
//   bit 5 TREG    = in thermal regulation (die too hot, reducing charge current)
//   bit 3 IINDPM  = input current limit / ILIM pin regulation active
//   bit 2 VINDPM  = input VOLTAGE regulation active (source sagging -- the
//                   charger throttles input to keep VBUS above its floor;
//                   the usual reason a weak USB port/cable caps charge speed)
#define BQ25628_STATUS0_TREG_STAT     (1 << 5)
#define BQ25628_STATUS0_IINDPM_STAT   (1 << 3)
#define BQ25628_STATUS0_VINDPM_STAT   (1 << 2)

#define BQ25628_STATUS1_VBUS_STAT_MASK   0x07
#define BQ25628_STATUS1_CHG_STAT_MASK    0x18
#define BQ25628_STATUS1_CHG_STAT_SHIFT   3

// VBUS_STAT (CHARGER_STATUS_1, masked by BQ25628_STATUS1_VBUS_STAT_MASK):
// 0x00 = no input source detected -- i.e. no USB/DC present, running on
// battery (if any). Other values indicate a detected adapter type; we only
// need to distinguish "not powered" from "powered" for the on-battery
// indicator, not the specific adapter type.
#define BQ25628_VBUS_STAT_NOT_POWERED 0x00

// BATFET_CTRL (CHARGER_CONTROL_2, register 0x18, bits 1:0). Verified
// against the Adafruit_BQ25628E reference driver (same register map as the
// plain BQ25628 used here).
typedef enum {
    BQ25628_BATFET_NORMAL = 0,
    BQ25628_BATFET_SHUTDOWN = 1,
    BQ25628_BATFET_SHIP = 2,
    BQ25628_BATFET_RESET = 3,
} bq25628_batfet_ctrl_t;

// CHARGER_CONTROL_2 bit 2: BATFET_DLY. Per the datasheet (tBATFET_DLY),
// this is the delay between writing a BATFET_CTRL ship/shutdown command and
// the BATFET actually opening: 1 = 12.5s (POR default), 0 = 25ms.
// bq25628_set_batfet_ctrl() clears it so a commanded ship mode powers off
// promptly -- the 12.5s window exists to allow aborting the shutdown, which
// we never need since ship mode is only ever triggered deliberately.
#define BQ25628_CTRL2_BATFET_DLY (1 << 2)

#define BQ25628_FAULT_VBUS_FAULT_STAT (1 << 7)
#define BQ25628_FAULT_BAT_FAULT_STAT  (1 << 6)
#define BQ25628_FAULT_SYS_FAULT_STAT  (1 << 5)
#define BQ25628_FAULT_TSHUT_STAT      (1 << 3)
#define BQ25628_FAULT_TS_STAT_MASK    0x07

typedef struct {
    i2c_master_dev_handle_t dev;
} bq25628_t;

// Adds the charger as a device on `bus` (does not touch the bus itself).
// Returns true on success.
bool bq25628_init(i2c_master_bus_handle_t bus, bq25628_t *out_chg);

// Reads `len` bytes starting at `reg`. Returns true on success.
bool bq25628_read(bq25628_t *chg, uint8_t reg, uint8_t *buf, size_t len);

// Enables the ADC in continuous-conversion, 12-bit mode and waits briefly
// for the first conversion to complete.
bool bq25628_adc_enable(bq25628_t *chg);

// ADC result readers. Each returns true on successful I2C transaction;
// the value is always written to *out even if the ADC hasn't converted yet.
bool bq25628_read_vbus_mv(bq25628_t *chg, int *out_mv);
bool bq25628_read_vbat_mv(bq25628_t *chg, int *out_mv);
bool bq25628_read_vsys_mv(bq25628_t *chg, int *out_mv);
bool bq25628_read_ibat_ma(bq25628_t *chg, int *out_ma);
// Input current drawn from VBUS (positive = flowing into the system), 2mA/LSB.
bool bq25628_read_ibus_ma(bq25628_t *chg, int *out_ma);
// Charger die/junction temperature in degrees C (0.5 degC/LSB, -40..140 range).
bool bq25628_read_tdie_c(bq25628_t *chg, float *out_c);

// Charger/fault status. Returns true on successful I2C transaction.
bool bq25628_read_status(bq25628_t *chg, uint8_t *status0, uint8_t *status1, uint8_t *fault0);

// Human-readable TS (thermistor) status string from a FAULT_STATUS_0 value.
const char *bq25628_ts_status_str(uint8_t fault0);

// Human-readable charge-phase string from a CHARGER_STATUS_1 value (the
// same byte bq25628_read_status() already fetches into *status1).
const char *bq25628_chg_stat_str(uint8_t status1);

// Enables/disables TS (thermistor) pin monitoring. Pass true when no NTC is
// populated, so a floating TS pin doesn't block charging with a bias fault.
bool bq25628_set_ts_ignore(bq25628_t *chg, bool ignore);
bool bq25628_get_ts_ignore(bq25628_t *chg, bool *out_ignore);

// Pets the I2C watchdog (writes WD_RST=1). Must be called at least once
// within the configured WATCHDOG timeout (POR default 50s) or the charger
// reverts several registers -- notably TS_IGNORE -- to their defaults.
bool bq25628_reset_watchdog(bq25628_t *chg);

// VBUS_STAT bits from CHARGER_STATUS_1 (already masked). Compare against
// BQ25628_VBUS_STAT_NOT_POWERED to detect "running on battery".
bool bq25628_read_vbus_stat(bq25628_t *chg, uint8_t *out_vbus_stat);

// Read-modify-writes BATFET_CTRL (bits 1:0 of CHARGER_CONTROL_2) and clears
// BATFET_DLY (bit 2) in the same write, so ship/shutdown takes effect after
// ~25ms instead of the 12.5s POR default (see BQ25628_CTRL2_BATFET_DLY).
// All other bits are preserved. BQ25628_BATFET_SHIP cuts the battery from
// the system (ship mode) -- see power_ctrl.c for the safe
// stop-recording-first sequence this should always be called through.
bool bq25628_set_batfet_ctrl(bq25628_t *chg, bq25628_batfet_ctrl_t ctrl);

// Sets the fast-charge current regulation limit (ICHG, REG0x02/REG0x03).
// Clamped to the chip's supported range, 40mA-2000mA, rounded down to the
// nearest 40mA step. See CHARGE_CURRENT_MA in power_ctrl.c for where the
// actual target value is chosen (that's a battery-safety decision, not a
// hardware one -- keep it there, not hardcoded in this driver).
bool bq25628_set_charge_current_ma(bq25628_t *chg, int ma);
