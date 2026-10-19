/*
 * SPDX-License-Identifier: MIT
 *
 * dsp_chain — pemilik sdr_rx_task (Core 1): baca blok I/Q dari rx_adc lalu
 * jalankan rantai DSP (docs/ARCHITECTURE.md §6) dan serahkan audio ke
 * audio_out.
 *
 * Status F3: hanya penarik blok + diagnostik rate/DC/overrun. Tahap DSP §6
 * (DC block, NCO, LPF/decim, BFO, AGC) ditambahkan pada F5.
 */
#pragma once

#include "esp_err.h"

/** Siapkan state DSP (LUT NCO, koefisien FIR). Panggil sebelum task dibuat. */
esp_err_t dsp_chain_init(void);

/** Reset state filter/akumulator sehingga blok berikutnya mulai bersih. */
void dsp_chain_reset(void);

/** Entry point sdr_rx_task: Core 1, prio 12, stack 16 KB. */
void sdr_rx_task(void *arg);
