/*
 * SPDX-License-Identifier: MIT
 *
 * selftest - uji fungsional rantai DSP memakai I/Q sintetis (tanpa ADC).
 *
 * Build khusus (CONFIG_SDR_SELFTEST=y): ADC DMA tidak pernah diinisialisasi
 * sehingga GPIO35/GPIO36 tidak tersentuh. Frame I/Q dibangkitkan oleh
 * iq_gen dan disuntikkan ke dsp_chain, sehingga seluruh rantai produksi
 * (DC block, NCO, LPF+decimasi, BFO, AGC, spektrum) diuji apa adanya.
 *
 * Laporan ditulis ke log serial (tag "selftest") dengan status:
 *   PASS = perilaku sesuai spesifikasi
 *   WARN = fungsi berjalan, tetapi nilai terukur belum memenuhi ambang
 *         acceptance di PRD.md (dicetak lengkap untuk ditindaklanjuti)
 *   FAIL = fungsi tidak berjalan / hasil di luar toleransi
 */
#pragma once

#include <stdbool.h>

/** Jalankan seluruh kasus uji lalu masuk mode demo (sinyal tetap mengalir). */
void selftest_task(void *arg);

/**
 * @brief Buat task self-test (Core 1, prio 12) dengan stack statis 8 KB.
 *
 * Stack & TCB statis: alokasi heap internal setelah spectrum_init() sudah
 * terfragmentasi sehingga stack dinamis 8 KB pernah gagal dibuat, dan stack
 * 4 KB memicu overflow (panic StoreProhibited). Dipanggil dari app_main.
 *
 * @return true bila task berhasil dibuat.
 */
bool selftest_start(void);
