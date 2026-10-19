/*
 * SPDX-License-Identifier: MIT
 *
 * Runtime SDR configuration — the single struct shared across cores.
 *
 * Writers: UI (Core 0). Readers: DSP (Core 1, snapshot per block).
 * Guarded by a short critical section; never passed around as a pointer
 * (docs/ARCHITECTURE.md §3, §9).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDR_MODE_USB 0
#define SDR_MODE_LSB 1

typedef struct {
    int32_t lo_label_hz;    /* visual label only; real LO is external      */
    int32_t nco_offset_hz;  /* -CONFIG_SDR_NCO_LIMIT_HZ .. +limit, tuning  */
    int32_t nco_step_hz;    /* 10/50/100/500/1k/10k/100k/1M                */
    uint8_t mode;           /* SDR_MODE_USB / SDR_MODE_LSB                 */
    uint8_t volume_pct;     /* 0..100                                      */
    uint8_t agc_target;     /* AGC target level (used from F5)             */
    int16_t lo_i, lo_q;     /* I/Q calibration slots (reserved, future)    */
} sdr_config_t;

/** Load defaults from Kconfig. Call once from app_main before ui_init(). */
void sdr_config_init(void);

/** Atomic snapshot for the DSP task. */
void sdr_config_get(sdr_config_t *out);

void sdr_config_set_offset(int32_t hz);      /* clamped to +/- limit      */
void sdr_config_set_lo_label(int32_t hz);    /* clamped 0..CONFIG_SDR_LO_MAX_HZ */
void sdr_config_set_step(int32_t hz);        /* 10/50/100/500/1k/10k/100k/1M */

/** Read-only list of allowed steps (for the step picker UI). */
const int32_t *sdr_config_step_options(size_t *count);
void sdr_config_set_mode(uint8_t mode);
void sdr_config_toggle_mode(void);
void sdr_config_set_volume(uint8_t pct);     /* clamped 0..100            */
void sdr_config_add_volume(int8_t delta_pct);
void sdr_config_set_agc_target(uint8_t target);

#ifdef __cplusplus
}
#endif
