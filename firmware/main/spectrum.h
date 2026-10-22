/*
 * SPDX-License-Identifier: MIT
 *
 * spectrum — FFT esp-dsp 2048 titik pada stream I/Q pasca-NCO @12 kHz
 * (FR-6, docs/ARCHITECTURE.md §3/§5).
 *
 * Core 1 (dsp_chain) menyalin snapshot 1024 frame dan memanggil
 * spectrum_ingest(); Core 0 (timer UI) memanggil spectrum_get_db() yang
 * menerapkan jendela Hann, zero-pad ke SPECTRUM_FFT_N, FFT, dan mengubah
 * ke dB.
 *
 * Zero-pad menambah kepadatan sampel frekuensi tampilan (5,86 Hz/bin
 * @12 kHz untuk N=2048); resolusi spektrum asli (RBW Hann) tetap
 * 1024-titik (~11,7 Hz). Keluaran = band penuh terpusat DC: jumlah bin
 * mengikuti mode runtime (sdr_config.fft_bins = 1024 atau 2048), index
 * tengah = DC, index 0 = tepi kiri (-FS/2), urutan display frekuensi naik.
 * Line chart memakai nilai bin asli TANPA interpolasi.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "sdkconfig.h"

#include "rx_adc.h"

/** NFFT zero-pad: pangkat 2, >= CONFIG_SDR_FFT_N, <= CONFIG_DSP_MAX_FFT_SIZE. */
#define SPECTRUM_FFT_N     2048

/** Jumlah bin keluaran maksimum (mode 2048 = band penuh +/-FS/2). */
#define SPECTRUM_MAX_OUT_NBINS 2048

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
 * @brief Kunci konsumsi snapshot (self-test).
 *
 * Saat terkunci, spectrum_get_db() selalu mengembalikan false sehingga
 * snapshot tidak diambil konsumen lain (task UI Core 0). Dipakai self-test
 * agar snapshot yang diukur benar-benar data yang baru saja di-ingest.
 * Production: biarkan terbuka (default false).
 */
void spectrum_set_consume_locked(bool locked);

/**
 * @brief Mengambil hasil spektrum terbaru dalam urutan display (Core 0).
 *
 * Mengonversi snapshot terbaru → Hann → zero-pad SPECTRUM_FFT_N → FFT →
 * dB. `out_db` diisi `*out_nbins` nilai dB band penuh terpusat DC:
 * index 0 = tepi kiri (-FS/2), index `*out_nbins`/2 = DC (frekuensi
 * tuning). Jumlah bin = mode runtime sdr_config.fft_bins (1024 atau
 * 2048); lebar bin = CONFIG_SDR_AUDIO_FS_HZ / jumlah bin.
 *
 * @param out_db buffer dB (kapasitas SPECTRUM_MAX_OUT_NBINS float).
 * @param out_nbins diisi jumlah bin efektif yang dihasilkan.
 * @return true bila ada frame baru; false bila tidak ada (UI skip render).
 */
bool spectrum_get_db(float *out_db, int *out_nbins);

/**
 * @brief Sama seperti spectrum_get_db(), tetapi dapat melewati kunci konsumsi.
 *
 * `ignore_lock = true` dipakai pemilik kunci (self-test) untuk mengambil
 * snapshot yang terkunci; konsumen biasa (task UI) memakai
 * spectrum_get_db() yang honor lock.
 */
bool spectrum_get_db_ex(float *out_db, int *out_nbins, bool ignore_lock);
