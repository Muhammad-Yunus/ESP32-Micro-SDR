/*
 * SPDX-License-Identifier: MIT
 *
 * dsp_chain — pemilik sdr_rx_task (Core 1): baca blok I/Q dari rx_adc lalu
 * jalankan rantai DSP (docs/ARCHITECTURE.md §6): DC block → NCO offset →
 * LPF/decimasi 48→24→12 → BFO → audio LPF → AGC+volume → audio_out_push.
 *
 * Hook self-test (CONVENTIONS.md §5 "tone-injection internal bypass ADC"):
 * `dsp_chain_process_block()` dapat dipanggil dengan I/Q sintetis dan
 * `dsp_chain_set_pcm_sink()` dapat mencegat PCM hasil demodulasi. Pada
 * build produksi (`CONFIG_SDR_SELFTEST=n`) hook tidak pernah diaktifkan:
 * sumber blok tetap rx_adc, PCM tetap dikirim ke audio_out.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "rx_adc.h"

/** Callback penampung PCM hasil tahap 7 (dipanggil di dalam proses blok). */
typedef void (*dsp_pcm_sink_t)(const int8_t *pcm, size_t n);

/**
 * @brief Batas gain AGC (tahap 7): -36 dB .. +36 dB.
 *
 * Karena gain maksimum hanya +36 dB, target AGC (CONFIG_SDR_AGC_TARGET)
 * hanya tercapai untuk level input > target/64. Self-test mengukur
 * perilaku ini (kasus T09/T10).
 */
#define DSP_AGC_GAIN_MIN  (1.0f / 64.0f)
#define DSP_AGC_GAIN_MAX  64.0f

/** Siapkan state DSP (LUT NCO, koefisien FIR, alpha AGC). Panggil sebelum task dibuat. */
esp_err_t dsp_chain_init(void);

/** Reset state filter/akumulator sehingga blok berikutnya mulai bersih. */
void dsp_chain_reset(void);

/**
 * Proses satu blok I/Q melalui rantai DSP lengkap.
 *
 * Dipanggil sdr_rx_task (produksi) maupun self-test (I/Q sintetis).
 * Mengembalikan jumlah frame yang diproses (0 bila < 4 frame).
 */
size_t dsp_chain_process_block(const iq_t *in, size_t frames);

/**
 * Pasang penampung PCM (self-test). NULL = kembali ke audio_out_push.
 *
 * PERINGATAN: hook ini hanya boleh dipakai pada build self-test; pada
 * build produksi penampung harus NULL agar audio tidak terputus.
 */
void dsp_chain_set_pcm_sink(dsp_pcm_sink_t sink);

/**
 * Paksa laju sampel (Hz) yang dipakai untuk perhitungan NCO/BFO.
 *
 * 0 (default) = pakai laju terukur dari ADC. Self-test memakai nilai
 * nominal Kconfig karena tidak ada ADC.
 */
void dsp_chain_set_fs_hz(uint32_t fs_hz);

/** Entry point sdr_rx_task: Core 1, prio 12, stack 16 KB. */
void sdr_rx_task(void *arg);
