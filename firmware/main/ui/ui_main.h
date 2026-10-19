/*
 * SPDX-License-Identifier: MIT
 *
 * UI SDR — layout 480x320 & controls (Core 0).
 *
 * Module boundary (docs/ARCHITECTURE.md §3): this file only touches LVGL
 * and sdr_config_set_* — never ADC, DSP or bsp_audio_pcm_write().
 *
 * F2 scope: top frequency bar (FR-8), bottom control bar + numeric keypad
 * modal (FR-3 sources a/b, FR-9).
 * F6 scope: spectrum bar + waterfall via ui_scope_init() (FR-6).
 */
#pragma once

#include "esp_err.h"

/**
 * @brief Membuat seluruh objek UI awal (di bawah bsp_lvgl_lock).
 *
 * @return ESP_OK bila sukses.
 */
esp_err_t ui_init(void);

/**
 * @brief Memperbarui label frekuensi/mode/volume (dipanggil dari ui_scope
 *        saat drag waterfall mengubah nco_offset_hz).
 */
void ui_main_refresh_freq(void);
