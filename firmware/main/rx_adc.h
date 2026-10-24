/*
 * SPDX-License-Identifier: MIT
 *
 * rx_adc — sampling I/Q simultan via ADC1 continuous DMA (FR-1).
 *
 * Modul ini hanya boleh menyentuh ADC (docs/ARCHITECTURE.md §3): tidak
 * mengetahui DAC/audio, LVGL, maupun modul lain. Hasil dibaca sebagai blok
 * frame I/Q ternormalisasi oleh sdr_rx_task (dsp_chain).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Satu pasang sampel kompleks; I dan Q diambil pada konversi berurutan. */
typedef struct {
    float i;
    float q;
} iq_t;

/** Handle + config + start DMA. Dipanggil terakhir (ARCHITECTURE.md §7). */
esp_err_t rx_adc_init(void);
void rx_adc_deinit(void);

/**
 * Blocking read satu atau lebih frame I/Q (bukan byte raw).
 * Mengembalikan jumlah frame yang benar-benar terisi di `out`; 0 = timeout.
 * Channel selain CH0/CH7 (I/Q) diabaikan.
 */
size_t rx_adc_read(iq_t *out, size_t max_frames);

/** Rate per kanal hasil pengukuran runtime (dipakai NCO/FFT), dalam Hz. */
uint32_t rx_adc_measured_fs_hz(void);

/** Hitungan event pool DMA penuh (overrun); 0 pada operasi normal. */
uint32_t rx_adc_overrun_count(void);

/** Diagnostik raw-12-bit per kanal (jendela 2 s, di-reset otomatis). */
typedef struct {
    uint16_t min;            /* raw minimum (0..4095) */
    uint16_t max;            /* raw maksimum (0..4095) */
    uint32_t avg;            /* rata-rata raw (0..4095) */
    uint32_t rms;            /* akar rata-rata kuadrat raw (0..4095) */
    uint32_t p2p;            /* max − min (0..4095) */
} rx_adc_diag_chan_t;

typedef struct {
    rx_adc_diag_chan_t i;    /* CH0 = GPIO36 */
    rx_adc_diag_chan_t q;    /* CH7 = GPIO35 */
    uint32_t frames;         /* jumlah sampel raw per kanal pada jendela */
} rx_adc_diag_t;

/**
 * Isi `out` dengan statistik raw-12-bit I/Q pada jendela 2 s terakhir.
 * Dipanggil dari sdr_rx_task (Core 1) saat log diagnostik.
 */
void rx_adc_diag_get(rx_adc_diag_t *out);
