/*
 * SPDX-License-Identifier: MIT
 *
 * iq_gen - generator I/Q kompleks sintetis untuk self-test rantai DSP.
 *
 * Dipakai HANYA pada build self-test (CONFIG_SDR_SELFTEST=y): menyuplai frame
 * I/Q ke dsp_chain tanpa ADC sama sekali, sehingga rantai DSP (DC block,
 * NCO, LPF+decimasi, BFO, AGC) dan spektrum dapat diuji tanpa sinyal RF
 * dan tanpa menyentuh GPIO35/GPIO36.
 *
 * Sinyal yang dapat dibangkitkan:
 *   - DC konstan (I/Q)                 -> uji DC removal (FR-2)
 *   - hingga 2 tone kompleks           -> uji NCO/BFO/spektrum (FR-3/4/6)
 *     (I = A*cos, Q = A*sin sehingga tone berada pada satu sisi frekuensi)
 *   - noise putih (LCG, reproducible)  -> uji floor noise
 *
 * Fase kumulatif antar panggilan (tanpa discontinuity) sehingga blok dapat
 * diumpankan seperti streaming ADC. Noise memakai LCG ber-seed supaya hasil
 * self-test deterministik dan dapat direproduksi.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rx_adc.h"

#define IQ_GEN_MAX_TONES 2

/** Konfigurasi sumber sinyal sintetis. */
typedef struct {
    float    fs_hz;                     /* laju sampel stream (Hz)        */
    int      tone_count;                /* 0..IQ_GEN_MAX_TONES             */
    float    tone_hz[IQ_GEN_MAX_TONES];  /* frekuensi tone (Hz, boleh < 0) */
    float    tone_amp[IQ_GEN_MAX_TONES]; /* amplitudo kompleks tone        */
    float    dc_i;                      /* komponen DC kanal I             */
    float    dc_q;                      /* komponen DC kanal Q             */
    float    noise_amp;                 /* amplitudo noise (0 = mati)      */
    uint32_t seed;                      /* seed LCG noise                  */
} iq_gen_cfg_t;

/** State generator (fase kumulatif + LCG). */
typedef struct {
    iq_gen_cfg_t cfg;
    float    phase[IQ_GEN_MAX_TONES];   /* fase kumulatif (rad)            */
    uint32_t rng;
    uint64_t samples;
} iq_gen_t;

/** Isi konfigurasi default: satu tone, tanpa DC, tanpa noise. */
void iq_gen_cfg_default(iq_gen_cfg_t *cfg, float fs_hz);

/** Inisialisasi state generator dari konfigurasi (fase & LCG di-reset). */
void iq_gen_init(iq_gen_t *gen, const iq_gen_cfg_t *cfg);

/** Reset fase & LCG (stream baru dari titik yang sama). */
void iq_gen_reset(iq_gen_t *gen);

/**
 * Bangkit `frames` frame I/Q ke `out`.
 *
 * Fase tone dilanjutkan dari panggilan sebelumnya, sehingga blok dapat
 * diumpankan tanpa discontinuity.
 */
size_t iq_gen_fill(iq_gen_t *gen, iq_t *out, size_t frames);
