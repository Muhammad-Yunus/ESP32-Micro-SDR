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
#define SDR_MODE_AM  2
#define SDR_MODE_FM  3

#define SDR_SRC_IQ   0
#define SDR_SRC_Q    1
#define SDR_SRC_I    2

#define SDR_MAX_GAIN_DB      54   /* +54 dB = x512, selaras DSP_AGC_GAIN_MAX */
#define SDR_MIN_GAIN_DB     (-24)
#define SDR_MAX_AUDIO_LPF_HZ 3000
#define SDR_MIN_AUDIO_LPF_HZ 100
#define SDR_MAX_ADC_ATTEN_DB 11

typedef struct {
    int32_t lo_label_hz;    /* visual label only; real LO is external      */
    int32_t nco_offset_hz;  /* -CONFIG_SDR_NCO_LIMIT_HZ .. +limit, tuning  */
    int32_t nco_step_hz;    /* 10/50/100/500/1k/10k/100k/1M                */
    uint8_t mode;           /* SDR_MODE_USB / LSB / AM / FM */
    uint8_t volume_pct;     /* 0..100                                      */
    uint8_t agc_target;     /* AGC target level (used from F5)             */
    bool agc_enabled;       /* AGC on/off; when off, manual gain is used   */
    int16_t manual_gain_db;/* -SDR_MAX_GAIN_DB..+SDR_MAX_GAIN_DB          */
    uint8_t src;           /* SDR_SRC_IQ / SDR_SRC_Q / SDR_SRC_I          */
    uint16_t lpf_hz;        /* audio LPF cutoff 100..6000 Hz               */
    uint8_t adc_atten_db;   /* 0..11, maps to ADC attenuation preset       */
    int16_t lo_i, lo_q;     /* I/Q calibration slots (reserved, future)    */
    uint16_t test_tone_hz;  /* 0 = off; >0 = synth sinus direct ke SDM     */
    uint16_t fft_bins;      /* 512 / 1024 / 2048                          */
    int32_t  span_half_hz;  /* ± Hz dari LO: 250/500/1k/2k/5k/10k Hz      */
    bool     scope_enabled; /* ON = spectrum+waterfall timer aktif        */
    bool     dsp_bypass;    /* ON = jalur ADC→speaker mentah (skip DSP)   */
} sdr_config_t;

/** Default mode modulation dari Kconfig (CONFIG_SDR_DEFAULT_MODE_*). */
uint8_t sdr_config_default_mode(void);

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
void sdr_config_set_agc_enabled(bool enabled);
void sdr_config_set_manual_gain_db(int16_t db);
void sdr_config_set_src(uint8_t src);
void sdr_config_set_audio_lpf_hz(uint16_t hz);
void sdr_config_set_adc_atten_db(uint8_t atten_db);
void sdr_config_set_test_tone_hz(uint16_t hz); /* 0 = off, >0 = synth direct */
void sdr_config_set_fft_bins(uint16_t bins);
void sdr_config_set_span_half(int32_t hz);
const int32_t *sdr_config_span_options(size_t *count);
uint16_t sdr_config_fft_bins_get(void);
int32_t sdr_config_span_half_get(void);

/**
 * @brief Half-span efektif (Hz) yang benar-benar dapat ditampilkan.
 *
 * Nilai preset (sdr_config_span_half_get) di-clamp ke FS/2 karena sampler
 * 12 kHz hanya mencakup +/-6 kHz sekitar DC. Preset 10 kHz tetap terpilih di
 * UI, namun readout & plot memakai nilai efektif hasil fungsi ini.
 */
int32_t sdr_config_span_half_effective(void);
void   sdr_config_set_scope_enabled(bool on);
void   sdr_config_set_dsp_bypass(bool on);
#ifdef __cplusplus
}
#endif
