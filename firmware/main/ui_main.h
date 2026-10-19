/*
 * SPDX-License-Identifier: MIT
 *
 * UI SDR — layout & objek LVGL (Core 0).
 *
 * Batas modul (docs/ARCHITECTURE.md §3): UI hanya menyentuh LVGL dan
 * sdr_config_set_*; tidak pernah memanggil ADC/DSP secara langsung.
 */
#pragma once

#include "esp_err.h"

/**
 * @brief Membuat seluruh objek UI awal (di bawah bsp_lvgl_lock).
 *
 * @return ESP_OK bila sukses.
 */
esp_err_t ui_init(void);
