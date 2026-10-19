/*
 * SPDX-License-Identifier: MIT
 *
 * dsp_chain — sdr_rx_task: penarik blok I/Q dari ADC DMA + (F5) rantai DSP.
 *
 * Loop dipacing oleh blocking `rx_adc_read()` — tanpa vTaskDelay, tanpa
 * logging per blok (docs/ARCHITECTURE.md §2 dan §9).
 */
#include "dsp_chain.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "audio_out.h"
#include "rx_adc.h"

static const char *TAG = "sdr_rx";

#define DSP_BLOCK_FRAMES      CONFIG_SDR_ADC_BLOCK_FRAMES
#define DSP_DIAG_PERIOD_US    2000000           /* 2 s — diagnostik F3        */

static iq_t s_block[DSP_BLOCK_FRAMES];

static uint32_t s_blocks;
static uint64_t s_frames;
static double s_sum_i;
static double s_sum_q;
static float s_peak_i;
static float s_peak_q;
static int64_t s_diag_t0_us;
static uint32_t s_aud_prev;

esp_err_t dsp_chain_init(void)
{
    dsp_chain_reset();
    s_aud_prev = audio_out_update_count();
    ESP_LOGI(TAG, "blok %d frame (%.1f ms @ %d Hz), DSP §6 menyusul di F5",
             DSP_BLOCK_FRAMES, 1000.0f * DSP_BLOCK_FRAMES / CONFIG_SDR_ADC_FS_HZ,
             CONFIG_SDR_ADC_FS_HZ);
    return ESP_OK;
}

void dsp_chain_reset(void)
{
    s_blocks = 0;
    s_frames = 0;
    s_sum_i = 0.0;
    s_sum_q = 0.0;
    s_peak_i = 0.0f;
    s_peak_q = 0.0f;
    s_diag_t0_us = esp_timer_get_time();
}

void sdr_rx_task(void *arg)
{
    (void)arg;

    for (;;) {
        const size_t frames = rx_adc_read(s_block, DSP_BLOCK_FRAMES);
        if (frames == 0) {
            continue;       /* hanya timeout DMA; loop tetap dipacing driver */
        }

        for (size_t n = 0; n < frames; n++) {
            const float i = s_block[n].i;
            const float q = s_block[n].q;
            s_sum_i += (double)i;
            s_sum_q += (double)q;
            const float ai = (i < 0.0f) ? -i : i;
            const float aq = (q < 0.0f) ? -q : q;
            if (ai > s_peak_i) {
                s_peak_i = ai;
            }
            if (aq > s_peak_q) {
                s_peak_q = aq;
            }
        }
        s_blocks++;
        s_frames += frames;

        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_diag_t0_us >= DSP_DIAG_PERIOD_US) {
            const double n = (s_frames > 0) ? (double)s_frames : 1.0;
            const uint32_t aud = audio_out_update_count();
            ESP_LOGI(TAG,
                     "I/Q %" PRIu32 " Hz (target %d) | mean I %+.3f Q %+.3f | "
                     "peak I %.3f Q %.3f | blk %" PRIu32 " | ovf %" PRIu32
                     " | xrun %" PRIu32 " | aud %" PRIu32 " Hz | heap %u",
                     rx_adc_measured_fs_hz(), CONFIG_SDR_ADC_FS_HZ,
                     s_sum_i / n, s_sum_q / n, s_peak_i, s_peak_q, s_blocks,
                     rx_adc_overrun_count(),
                     audio_out_xrun_count(),
                     (aud - s_aud_prev) / (DSP_DIAG_PERIOD_US / 1000000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_aud_prev = aud;
            s_sum_i = 0.0;
            s_sum_q = 0.0;
            s_peak_i = 0.0f;
            s_peak_q = 0.0f;
            s_blocks = 0;
            s_frames = 0;
            s_diag_t0_us = now_us;
        }
    }
}
