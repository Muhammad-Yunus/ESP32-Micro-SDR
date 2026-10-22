/*
 * SPDX-License-Identifier: MIT
 *
 * iq_gen - implementasi generator I/Q sintetis (lihat iq_gen.h).
 */
#include "iq_gen.h"

#include <math.h>
#include <string.h>

#define IQ_GEN_TWO_PI 6.28318530717958647692f

/* LCG 32-bit (Numerical Recipes) - deterministik & cukup untuk noise uji. */
#define IQ_GEN_LCG_A 1664525u
#define IQ_GEN_LCG_C 1013904223u

void iq_gen_cfg_default(iq_gen_cfg_t *cfg, float fs_hz)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->fs_hz = fs_hz;
    cfg->tone_count = 0;
    cfg->seed = 0x12345678u;
}

void iq_gen_init(iq_gen_t *gen, const iq_gen_cfg_t *cfg)
{
    memset(gen, 0, sizeof(*gen));
    gen->cfg = *cfg;
    if (gen->cfg.tone_count > IQ_GEN_MAX_TONES) {
        gen->cfg.tone_count = IQ_GEN_MAX_TONES;
    }
    if (gen->cfg.fs_hz <= 0.0f) {
        gen->cfg.fs_hz = 48000.0f;
    }
    iq_gen_reset(gen);
}

void iq_gen_reset(iq_gen_t *gen)
{
    for (int t = 0; t < IQ_GEN_MAX_TONES; t++) {
        gen->phase[t] = 0.0f;
    }
    gen->rng = gen->cfg.seed ? gen->cfg.seed : 1u;
    gen->samples = 0;
}

size_t iq_gen_fill(iq_gen_t *gen, iq_t *out, size_t frames)
{
    const float fs = gen->cfg.fs_hz;

    for (size_t n = 0; n < frames; n++) {
        float re = gen->cfg.dc_i;
        float im = gen->cfg.dc_q;

        for (int t = 0; t < gen->cfg.tone_count && t < IQ_GEN_MAX_TONES; t++) {
            const float ph = gen->phase[t];
            const float amp = gen->cfg.tone_amp[t];
            re += amp * cosf(ph);
            im += amp * sinf(ph);

            float next = ph + IQ_GEN_TWO_PI * gen->cfg.tone_hz[t] / fs;
            while (next >= IQ_GEN_TWO_PI) {
                next -= IQ_GEN_TWO_PI;
            }
            while (next < 0.0f) {
                next += IQ_GEN_TWO_PI;
            }
            gen->phase[t] = next;
        }

        if (gen->cfg.noise_amp > 0.0f) {
            gen->rng = gen->rng * IQ_GEN_LCG_A + IQ_GEN_LCG_C;
            const float ui = ((float)(gen->rng >> 8) / 16777216.0f) * 2.0f - 1.0f;
            gen->rng = gen->rng * IQ_GEN_LCG_A + IQ_GEN_LCG_C;
            const float uq = ((float)(gen->rng >> 8) / 16777216.0f) * 2.0f - 1.0f;
            re += ui * gen->cfg.noise_amp;
            im += uq * gen->cfg.noise_amp;
        }

        out[n].i = re;
        out[n].q = im;
    }

    gen->samples += frames;
    return frames;
}
