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

#include <stdbool.h>

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
 * `db` berisi `nbins` nilai dB band penuh terpusat DC (index tengah = DC,
 * index 0 = tepi kiri -FS/2); jumlah bin runtime 1024 atau 2048 mengikuti
 * mode FFT dari Settings modal. Lebar bin = CONFIG_SDR_AUDIO_FS_HZ/nbins,
 * tampilan di-zoom ke ±span efektif (sdr_config.span_half_hz, dijepit ke
 * FS/2); line chart menampilkan titik per bin asli tanpa interpolasi
 * (atau envelope max per piksel bila bin melebihi piksel), marker merah +
 * segitiga kuning + chip menandai puncak, dan satu baris waterfall
 * didorong ke bawah.
 */
void ui_scope_push(const float *db, int nbins);

/**
 * @brief Terapkan ulang skala runtime (span/FFT bin) dan bersihkan waterfall.
 *
 * Dipanggil setelah Settings modal mengubah span atau jumlah bin FFT:
 * baris waterfall lama memakai skala berbeda sehingga dihapus, dan chart
 * digambar ulang dengan skala baru.
 */
void ui_scope_refresh(void);

/**
 * @brief Aktifkan / nonaktifkan timer spectrum (ON/OFF Scope).
 *
 * `on = false` → timer dipause: tidak ada drain spectrum_get_db(), tidak
 * ada render line chart / waterfall / peak. DSP task tetap berjalan
 * (audio normal); spectrum_ingest() tetap mengisi pool (ditimpa
 * otomatis). `on = true` → timer diresume.
 */
void ui_scope_set_enabled(bool on);
