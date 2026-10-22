/*
 * SPDX-License-Identifier: MIT
 *
 * sdr_config implementation — critical-section guarded, no allocation,
 * safe to call from Core 0 (UI) while Core 1 (DSP) takes snapshots.
 */
#include "sdr_config.h"

#include <sdkconfig.h>

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static sdr_config_t s_cfg;

/* Allowed tuning steps (docs/PRD.md FR-3, extended by user review). */
static const int32_t s_steps[] = {
    10, 50, 100, 500, 1000, 10000, 100000, 1000000,
};
#define SDR_STEP_COUNT (sizeof(s_steps) / sizeof(s_steps[0]))

static const int32_t s_span_opts[] = {
    250, 500, 1000, 2000, 5000, 10000,
};
#define SDR_SPAN_COUNT (sizeof(s_span_opts) / sizeof(s_span_opts[0]))

void sdr_config_init(void)
{
    portENTER_CRITICAL(&s_lock);
    s_cfg = (sdr_config_t) {
        .lo_label_hz   = CONFIG_SDR_DEFAULT_LO_HZ,
        .nco_offset_hz = 0,
        .nco_step_hz   = 100,
        .mode          = SDR_MODE_USB,
        .volume_pct    = CONFIG_SDR_DEFAULT_VOLUME_PCT,
        .agc_target    = CONFIG_SDR_AGC_TARGET,
        .lo_i          = 0,
        .lo_q          = 0,
        .fft_bins      = 2048,
        .span_half_hz  = CONFIG_SDR_DISPLAY_SPAN_HZ,
    };
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_get(sdr_config_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_set_offset(int32_t hz)
{
    const int32_t limit = CONFIG_SDR_NCO_LIMIT_HZ;
    if (hz > limit) {
        hz = limit;
    } else if (hz < -limit) {
        hz = -limit;
    }
    portENTER_CRITICAL(&s_lock);
    s_cfg.nco_offset_hz = hz;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_set_lo_label(int32_t hz)
{
    if (hz < 0) {
        hz = 0;
    } else if (hz > CONFIG_SDR_LO_MAX_HZ) {
        hz = CONFIG_SDR_LO_MAX_HZ;
    }
    portENTER_CRITICAL(&s_lock);
    s_cfg.lo_label_hz = hz;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_set_step(int32_t hz)
{
    for (size_t i = 0; i < SDR_STEP_COUNT; i++) {
        if (s_steps[i] == hz) {
            portENTER_CRITICAL(&s_lock);
            s_cfg.nco_step_hz = hz;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
    }
}

const int32_t *sdr_config_step_options(size_t *count)
{
    *count = SDR_STEP_COUNT;
    return s_steps;
}

void sdr_config_set_mode(uint8_t mode)
{
    portENTER_CRITICAL(&s_lock);
    s_cfg.mode = (mode == SDR_MODE_LSB) ? SDR_MODE_LSB : SDR_MODE_USB;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_toggle_mode(void)
{
    portENTER_CRITICAL(&s_lock);
    s_cfg.mode = (s_cfg.mode == SDR_MODE_USB) ? SDR_MODE_LSB : SDR_MODE_USB;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_set_volume(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    portENTER_CRITICAL(&s_lock);
    s_cfg.volume_pct = pct;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_add_volume(int8_t delta_pct)
{
    portENTER_CRITICAL(&s_lock);
    int32_t v = (int32_t)s_cfg.volume_pct + delta_pct;
    if (v < 0) {
        v = 0;
    } else if (v > 100) {
        v = 100;
    }
    s_cfg.volume_pct = (uint8_t)v;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_set_agc_target(uint8_t target)
{
    portENTER_CRITICAL(&s_lock);
    s_cfg.agc_target = target;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_set_fft_bins(uint16_t bins)
{
    if (bins != 1024 && bins != 2048) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_cfg.fft_bins = bins;
    portEXIT_CRITICAL(&s_lock);
}

void sdr_config_set_span_half(int32_t hz)
{
    for (size_t i = 0; i < SDR_SPAN_COUNT; i++) {
        if (s_span_opts[i] == hz) {
            portENTER_CRITICAL(&s_lock);
            s_cfg.span_half_hz = hz;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
    }
}

const int32_t *sdr_config_span_options(size_t *count)
{
    *count = SDR_SPAN_COUNT;
    return s_span_opts;
}

uint16_t sdr_config_fft_bins_get(void)
{
    uint16_t v;
    portENTER_CRITICAL(&s_lock);
    v = s_cfg.fft_bins;
    portEXIT_CRITICAL(&s_lock);
    return v;
}

int32_t sdr_config_span_half_get(void)
{
    int32_t v;
    portENTER_CRITICAL(&s_lock);
    v = s_cfg.span_half_hz;
    portEXIT_CRITICAL(&s_lock);
    return v;
}

int32_t sdr_config_span_half_effective(void)
{
    const int32_t lim = CONFIG_SDR_AUDIO_FS_HZ / 2;   /* Nyquist 12 kHz */
    int32_t v = sdr_config_span_half_get();
    if (v > lim) {
        v = lim;
    } else if (v < 1) {
        v = 1;
    }
    return v;
}
