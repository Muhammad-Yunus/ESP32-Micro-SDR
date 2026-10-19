/*
 * SPDX-License-Identifier: MIT
 *
 * spectrum — FFT esp-dsp 1024 titik pada stream I/Q pasca-NCO @12 kHz
 * (FR-6, docs/ARCHITECTURE.md §3/§5).
 *
 * Core 1 (dsp_chain) menyalin snapshot 1024 frame dan memanggil
 * spectrum_ingest(); Core 0 (timer UI) memanggil spectrum_get_db() yang
 * menerapkan jendela Hann, FFT, dan mengubah ke dB — urutan display:
 * index 0 = −fs/2, center = DC, index N−1 = +fs/2−bin.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "rx_adc.h"

/**
 * @brief Inisialisasi pool snapshot, LUT Hann, dan tabel FFT esp-dsp.
 *
 * Dipanggil di app_main setelah dsp_chain_init() (ARCHITECTURE.md §7).
 *
 * @return ESP_OK bila sukses.
 */
esp_err_t spectrum_init(void);

/**
 * @brief Menyerahkan snapshot 1024 frame I/Q (dipanggil dari Core 1).
 *
 * Data disalin ke slot pool bebas. Bila pool habis, satu slot terlama
 * dibuang (drop-LAMA, ARCHITECTURE.md §4). Tidak pernah memblokir.
 */
void spectrum_ingest(const iq_t *snapshot1024);

/**
 * @brief Mengambil hasil spektrum terbaru dalam urutan display (Core 0).
 *
 * Mengonversi snapshot terbaru → Hann → FFT → dB. `out_db` diisi
 * `*out_nbins` (= CONFIG_SDR_FFT_N) nilai dB: index 0 = −fs/2,
 * index N/2 = DC (frekuensi tuning), index N−1 = +fs/2−bin.
 *
 * @return true bila ada frame baru; false bila tidak ada (UI skip render).
 */
bool spectrum_get_db(float *out_db, int *out_nbins);
