/*
 * SPDX-License-Identifier: MIT
 *
 * audio_out — sigma-delta (SDM) 1-bit output di GPIO26 + pool blok audio
 * + antrean `audio_q` (FR-5 bagian Audio Out).
 *
 * Pengirim adalah dsp_chain (sdr_rx_task, Core 1); pemutar adalah
 * audio_feed_task (Core 1, prio 11, dibuat di app_main — ARCHITECTURE.md §7).
 * `bsp_audio_*`/`dac_continuous` TIDAK dipakai: mengklaim I2S0, sama dengan
 * adc_continuous (docs/TECH_STACK.md §6).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Kanal SDM + pool + antrean. Task dibuat terpisah di app_main (§7). */
esp_err_t audio_out_init(void);

/**
 * Salin `n` sampel ke blok perakitan; blok penuh
 * (CONFIG_SDR_AUDIO_BLOCK_SAMPLES) dikirim ke `audio_q` dengan timeout 0.
 * false = drop + xrun++ (ARCHITECTURE.md §4). Aman dipanggil dari
 * sdr_rx_task; tidak memblokir.
 */
bool audio_out_push(const int8_t *pcm, size_t n);

/** Hitungan drop blok audio; 0 pada operasi normal (NFR-3). */
uint32_t audio_out_xrun_count(void);

/** Total update density yang ditulis ke SDM (diagnostik laju pacing). */
uint32_t audio_out_update_count(void);

/** Entry point audio_feed_task: Core 1, prio 11, stack 6 KB (§2). */
void audio_feed_task(void *arg);
