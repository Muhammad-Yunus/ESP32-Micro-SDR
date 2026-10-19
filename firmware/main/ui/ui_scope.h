/*
 * SPDX-License-Identifier: MIT
 *
 * ui_scope — widget spectrum line chart + waterfall (F6, FR-6).
 *
 * Membuat objek LVGL di area tengah layar (di bawah top bar, di atas
 * bottom bar), timer 30 ms yang menarik spectrum_get_db() (Core 0), dan
 * drag horizontal pada waterfall untuk mengubah nco_offset (§8).
 */
#pragma once

#include "esp_err.h"

#include "lvgl.h"

/**
 * @brief Membuat widget spektrum + waterfall di parent (layar aktif).
 *
 * Juga membuat lv_timer 30 ms yang drain spectrum_get_db() →
 * ui_scope_push() → render line chart + waterfall (ARCHITECTURE.md §5).
 *
 * @param parent objek induk (lv_screen_active()).
 * @return ESP_OK bila sukses.
 */
esp_err_t ui_scope_init(lv_obj_t *parent);

/**
 * @brief Menerima satu frame spektrum (dB, urutan display) untuk render.
 *
 * `db` berisi `nbins` nilai dB dengan index tengah = DC; line chart
 * menampilkan zoom ±SDR_DISPLAY_SPAN_HZ (~1 bin/piksel), marker merah +
 * chip menandai puncak, dan satu baris waterfall didorong ke bawah.
 */
void ui_scope_push(const float *db, int nbins);
