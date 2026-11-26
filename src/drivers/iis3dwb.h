// Register-level driver for the ST IIS3DWB SPI accelerometer, including
// FIFO-based high-rate capture.
//
// Register addresses and bitfield layout are taken from ST's own reference
// driver (iis3dwb-pid, https://github.com/STMicroelectronics/iis3dwb-pid,
// files iis3dwb_reg.h / iis3dwb_reg.c) -- verified against that source
// rather than assumed from familiarity with similar parts. Notably:
//   - CTRL1_XL packs [xl_en:3][rsvd:1][fs_xl:2][lpf2_xl_en:1][rsvd:1] from
//     MSB to LSB (NOT the [odr:3][fs:2][lpf2:1][rsvd:2] layout used by most
//     other ST accelerometers).
//   - FS_XL is NOT a monotonic 2/4/8/16g encoding: 2g=0, 16g=1, 4g=2, 8g=3.
//   - The FIFO is 512 entries deep, tag-based, 7 bytes/entry (1 tag byte +
//     X_L,X_H,Y_L,Y_H,Z_L,Z_H), burst-readable from FIFO_DATA_OUT_TAG with
//     auto-increment/auto-pop.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"

#define IIS3DWB_REG_FIFO_CTRL1     0x07
#define IIS3DWB_REG_FIFO_CTRL2     0x08
#define IIS3DWB_REG_FIFO_CTRL3     0x09
#define IIS3DWB_REG_FIFO_CTRL4     0x0A
#define IIS3DWB_REG_INT1_CTRL      0x0D
#define IIS3DWB_REG_WHO_AM_I       0x0F
#define IIS3DWB_REG_CTRL1_XL       0x10
#define IIS3DWB_REG_CTRL3_C        0x12
#define IIS3DWB_REG_CTRL5_C        0x14 // ST[1:0]_XL self-test bits: 00 off, 01 positive, 10 negative
#define IIS3DWB_REG_CTRL8_XL       0x17 // HPCF_XL[2:0] (bits7:5) cutoff, HP_SLOPE_XL_EN (bit2) LP/HP select
#define IIS3DWB_REG_CTRL10_C       0x19 // bit5 TIMESTAMP_EN -- enables the timestamp counter
#define IIS3DWB_REG_OUT_TEMP_L     0x20
#define IIS3DWB_REG_OUT_TEMP_H     0x21 // OUT_TEMP: 16-bit signed, 256 LSB/degC, 0 LSB at 25 degC
#define IIS3DWB_REG_STATUS_REG     0x1E
#define IIS3DWB_REG_FIFO_STATUS1   0x3A
#define IIS3DWB_REG_FIFO_STATUS2   0x3B
#define IIS3DWB_REG_OUTX_L_A       0x28
#define IIS3DWB_REG_TIMESTAMP0     0x40 // 32-bit free-running timestamp, LSB first (0x40..0x43)
#define IIS3DWB_REG_FIFO_DATA_OUT_TAG 0x78

#define IIS3DWB_CTRL10_TIMESTAMP_EN (1 << 5)
#define IIS3DWB_CTRL8_HP_SLOPE_EN   (1 << 2) // 1 = second stage is high-pass, 0 = low-pass

// CTRL5_C self-test mode selection (ST[1:0]_XL). Self-test applies a known
// internal electrostatic force; the datasheet-specified valid output change
// (FS = +-4g) is 800..3200 mg per axis. Must run stationary and at +-4g.
#define IIS3DWB_ST_XL_OFF      0x00
#define IIS3DWB_ST_XL_POSITIVE 0x01
#define IIS3DWB_ST_XL_NEGATIVE 0x02

#define IIS3DWB_WHO_AM_I_VAL 0x7B

#define IIS3DWB_INT1_FIFO_TH  (1 << 3) // watermark reached
#define IIS3DWB_INT1_FIFO_OVR (1 << 4) // overrun

#define IIS3DWB_FIFO_STATUS2_OVR_IA (1 << 3)

#define IIS3DWB_FIFO_DEPTH       512
#define IIS3DWB_FIFO_ENTRY_BYTES 7 // 1 tag byte + 6 data bytes (X/Y/Z, 16-bit each)

// FS_XL encoding (CTRL1_XL bits 3:2) -- NOT monotonic, see file header.
typedef enum {
    IIS3DWB_FS_2G = 0,
    IIS3DWB_FS_16G = 1,
    IIS3DWB_FS_4G = 2,
    IIS3DWB_FS_8G = 3,
} iis3dwb_fs_t;

// Second-stage digital filter mode. OFF bypasses LPF2 -- the output is then the
// first-stage LPF1 only (~6.3 kHz, linear phase). LOWPASS routes the data
// through LPF2 as a low-pass; HIGHPASS routes it through the high-pass path
// (removes the DC / gravity offset). LOWPASS/HIGHPASS use the cutoff selected by
// iis3dwb_hpcf_t. Maps to CTRL1_XL.LPF2_XL_EN (on when != OFF) + CTRL8_XL.
typedef enum {
    IIS3DWB_FILTER_OFF = 0,
    IIS3DWB_FILTER_LOWPASS,
    IIS3DWB_FILTER_HIGHPASS,
} iis3dwb_filter_mode_t;

// HPCF_XL[2:0] second-stage cutoff = ODR / {4,10,20,45,100,200,400,800}. At the
// fixed 26.7 kHz ODR the approximate -3 dB points are noted per value.
typedef enum {
    IIS3DWB_HPCF_ODR_4 = 0,   // ~6.3 kHz
    IIS3DWB_HPCF_ODR_10 = 1,  // ~2.7 kHz
    IIS3DWB_HPCF_ODR_20 = 2,  // ~1.3 kHz
    IIS3DWB_HPCF_ODR_45 = 3,  // ~590 Hz
    IIS3DWB_HPCF_ODR_100 = 4, // ~270 Hz
    IIS3DWB_HPCF_ODR_200 = 5, // ~133 Hz
    IIS3DWB_HPCF_ODR_400 = 6, // ~67 Hz
    IIS3DWB_HPCF_ODR_800 = 7, // ~33 Hz
} iis3dwb_hpcf_t;

// Approximate -3 dB cutoff, in Hz, for a given HPCF setting -- for UI labels and
// the recording log. Returns 0 for an out-of-range value.
int iis3dwb_hpcf_cutoff_hz(iis3dwb_hpcf_t hpcf);

typedef struct {
    spi_device_handle_t spi;
} iis3dwb_t;

// Adds the accelerometer as a device on `host` using PIN_ACCEL_CS, at a
// clock speed suitable for FIFO burst reads (8MHz -- still under the
// sensor's 10MHz hard limit). Does not touch the SPI bus itself; the caller
// must initialize the bus first (with DMA enabled: SPI_DMA_CH_AUTO, and
// max_transfer_sz large enough for a full FIFO drain -- see accel_capture.c).
bool iis3dwb_init(spi_host_device_t host, iis3dwb_t *out_dev);

bool iis3dwb_read_reg(iis3dwb_t *dev, uint8_t reg, uint8_t *val);
bool iis3dwb_write_reg(iis3dwb_t *dev, uint8_t reg, uint8_t val);

// Enables the accelerometer at its fixed 26.7 kHz ODR with the given full
// scale and second-stage filter (mode + cutoff), block-data-update and auto
// address increment. `cutoff` is ignored when `filter` is OFF.
bool iis3dwb_configure(iis3dwb_t *dev, iis3dwb_fs_t fs, iis3dwb_filter_mode_t filter,
                       iis3dwb_hpcf_t cutoff);

// Sensitivity in mg/LSB for a given FS_XL setting (standard ST per-range
// doubling from the 0.061 mg/LSB base at +-2g).
float iis3dwb_sensitivity_mg_per_lsb(iis3dwb_fs_t fs);

// Routes the data-ready signal to INT1 (IO15). Used only by the low-rate
// single-shot path; FIFO capture uses iis3dwb_fifo_enable_watermark_int()
// instead.
bool iis3dwb_enable_drdy_interrupt(iis3dwb_t *dev);

bool iis3dwb_data_ready(iis3dwb_t *dev, bool *ready);

// Reads raw 16-bit X/Y/Z and converts to g using the given FS sensitivity.
// Low-rate, single-shot -- not used by the FIFO capture path.
bool iis3dwb_read_xyz_g(iis3dwb_t *dev, iis3dwb_fs_t fs, float *x, float *y, float *z);

// Reads the on-chip temperature sensor (OUT_TEMP) in degrees C. Not the
// measured medium's temperature -- the die temperature of the sensor itself.
bool iis3dwb_read_temp_c(iis3dwb_t *dev, float *out_c);

// Enables/disables the sensor's 32-bit free-running timestamp counter
// (CTRL10_C.TIMESTAMP_EN). The counter is clocked from the same internal
// oscillator as the accelerometer ODR, so timestamp-ticks-per-sample is a
// near-constant that tracks ODR drift -- read it back with
// iis3dwb_read_timestamp() to measure the real elapsed time of a sample run.
bool iis3dwb_enable_timestamp(iis3dwb_t *dev, bool enable);

// Reads the current 32-bit timestamp counter (TIMESTAMP0..3, LSB first). The
// tick period is nominally ~21.7us but part-to-part variable; callers that
// need real seconds should cross-calibrate the tick against a known clock
// (e.g. esp_timer) rather than assume a fixed constant. Wraps ~every 26 hours
// at the nominal tick -- fine for elapsed-delta use (unsigned subtraction).
bool iis3dwb_read_timestamp(iis3dwb_t *dev, uint32_t *out_ticks);

// --- FIFO capture ---------------------------------------------------------

// Configures the FIFO for continuous capture: batches every accel sample
// (bdr_xl = 26.7kHz), STREAM mode (overwrites oldest entries on overflow
// rather than stopping), and sets the watermark threshold (entries, up to
// IIS3DWB_FIFO_DEPTH - 1).
bool iis3dwb_fifo_configure(iis3dwb_t *dev, uint16_t watermark_entries);

// Routes the FIFO watermark-reached flag to INT1 (IO15).
bool iis3dwb_fifo_enable_watermark_int(iis3dwb_t *dev);

// Reads FIFO_STATUS1/2: number of unread entries currently in the FIFO, and
// whether an overrun has occurred since the last read (the overrun flag is
// cleared by this read).
bool iis3dwb_fifo_status(iis3dwb_t *dev, uint16_t *out_num_entries, bool *out_overrun);

// Burst-reads `num_entries` FIFO entries (num_entries * IIS3DWB_FIFO_ENTRY_BYTES
// bytes) starting at FIFO_DATA_OUT_TAG into `out_buf` (caller-allocated,
// must be DMA-capable if the SPI bus was initialized with DMA). This also
// pops those entries from the sensor's FIFO.
bool iis3dwb_fifo_burst_read(iis3dwb_t *dev, uint16_t num_entries, uint8_t *out_buf);
