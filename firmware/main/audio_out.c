/*
 * SPDX-License-Identifier: MIT
 *
 * audio_out — SDM (sigma-delta) di GPIO26 (docs/ARCHITECTURE.md §5 langkah
 * 4-5, §2 tabel task).
 *
 * Dua mode pemutar di audio_feed_task:
 *  - CONFIG_SDR_AUDIO_TONE_HZ > 0: sin test digenerate langsung pada rate
 *    density (bring-up/diagnostik jalur SDM-amp; amplitudo = 90 x volume).
 *  - CONFIG_SDR_AUDIO_TONE_HZ = 0 (produksi): blok dari audio_q diputar
 *    dengan interpolasi linear x4 (update density 48 kHz, citra ZOH pindah
 *    ke ~47/49 kHz di luar band audio).
 *
 * Pool = 8 slot antrean + 1 blok perakitan + 1 blok yang sedang diputar;
 * drop policy mengikuti ARCHITECTURE.md §4 (audio tidak boleh drop, NFR-3):
 * antrean penuh → blok BARU dibuang + xrun++.
 *
 * Pacing memakai deadline absolut dari esp_timer (bukan ISR/timer hardware):
 * sdm_channel_set_pulse_density() hanya menulis register, jadi task pemutar
 * harus menjaga periode sampel sendiri; sisa <= ~1-2 ms per update di-spin
 * agar jitter tetap kecil.
 */
#include "audio_out.h"

#include <math.h>
#include <string.h>

#include "driver/sdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include "sdr_config.h"

static const char *TAG = "audio_out";

#define AUDIO_BLOCK_SAMPLES CONFIG_SDR_AUDIO_BLOCK_SAMPLES
#define AUDIO_QUEUE_LEN     8                    /* ARCHITECTURE.md §4     */
#define AUDIO_POOL_BLOCKS   (AUDIO_QUEUE_LEN + 2) /* antrean+perakitan+pemutar */

/* Interpolasi linear x4 pada jalur antrean: update density 48 kHz, citra
   ZOH berpindah ke 47/49 kHz (di luar band audio, docs/ARCHITECTURE.md §5). */
#define AUDIO_DENS_MULT 4
#define AUDIO_UPD_HZ    (CONFIG_SDR_AUDIO_FS_HZ * AUDIO_DENS_MULT)
#define AUDIO_SUB_BLOCK (AUDIO_BLOCK_SAMPLES * AUDIO_DENS_MULT)

/* Amplitudo tone: 90 = batas "lebih baik randomness" (driver sdm.h); skala
   volume_pct dari sdr_config. Dibagi 10 dari skala penuh agar level di
   osiloskop/speaker tidak terlalu kuat (bring-up). */
#define AUDIO_TONE_BASE_AMP 9.0f
#define AUDIO_TONE_RATE_MASK 4095        /* penyesuaian step tiap 4096 write */

static sdm_channel_handle_t s_chan;
static QueueHandle_t s_audio_q;   /* int8_t* — blok penuh siap diputar    */
static QueueHandle_t s_free_q;    /* int8_t* — blok kosong milik pool      */
static int8_t *s_asm;             /* blok perakitan (milik pengirim)       */
static size_t s_asm_len;
static volatile bool s_ready;
static volatile uint32_t s_xrun;
static volatile uint32_t s_updates;   /* total update density (diagnostik) */

static int8_t *audio_pool_pop(void)
{
    int8_t *blk = NULL;
    if (xQueueReceive(s_free_q, &blk, 0) != pdTRUE) {
        return NULL;
    }
    return blk;
}

esp_err_t audio_out_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ready, ESP_ERR_INVALID_STATE, TAG,
                        "sudah diinisialisasi");

    sdm_config_t sdm_cfg = {
        .clk_src = SDM_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_SDR_SDM_GPIO,
        .sample_rate_hz = CONFIG_SDR_SDM_CARRIER_HZ,
    };
    ESP_RETURN_ON_ERROR(sdm_new_channel(&sdm_cfg, &s_chan), TAG,
                        "sdm_new_channel (GPIO %d sudah dipakai?)",
                        CONFIG_SDR_SDM_GPIO);

    s_audio_q = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(int8_t *));
    s_free_q = xQueueCreate(AUDIO_POOL_BLOCKS, sizeof(int8_t *));
    ESP_RETURN_ON_FALSE(s_audio_q != NULL && s_free_q != NULL, ESP_ERR_NO_MEM,
                        TAG, "queue create");

    for (int i = 0; i < AUDIO_POOL_BLOCKS; i++) {
        int8_t *blk = heap_caps_malloc(AUDIO_BLOCK_SAMPLES,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(blk != NULL, ESP_ERR_NO_MEM, TAG,
                            "pool %d/%d", i + 1, AUDIO_POOL_BLOCKS);
        xQueueSend(s_free_q, &blk, 0);
    }

    s_asm = audio_pool_pop();
    s_asm_len = 0;
    s_xrun = 0;
    s_updates = 0;

    ESP_RETURN_ON_ERROR(sdm_channel_enable(s_chan), TAG, "enable");

    s_ready = true;
    ESP_LOGI(TAG,
             "SDM GPIO%d carrier %d Hz | audio %d Hz, blok %d sampel x%d, "
             "pool %d | free heap %u",
             CONFIG_SDR_SDM_GPIO, CONFIG_SDR_SDM_CARRIER_HZ,
             CONFIG_SDR_AUDIO_FS_HZ, AUDIO_BLOCK_SAMPLES, AUDIO_QUEUE_LEN,
             AUDIO_POOL_BLOCKS,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (CONFIG_SDR_AUDIO_TONE_HZ > 0) {
        ESP_LOGW(TAG, "tone test %d Hz aktif — bukan demodulasi (produksi = 0)",
                 CONFIG_SDR_AUDIO_TONE_HZ);
    }
    return ESP_OK;
}

bool audio_out_push(const int8_t *pcm, size_t n)
{
    if (!s_ready) {
        return false;
    }

    while (n > 0) {
        if (s_asm == NULL) {
            s_asm = audio_pool_pop();
            s_asm_len = 0;
            if (s_asm == NULL) {
                s_xrun++;
                return false;
            }
        }

        const size_t space = AUDIO_BLOCK_SAMPLES - s_asm_len;
        const size_t take = (n < space) ? n : space;
        memcpy(&s_asm[s_asm_len], pcm, take);
        s_asm_len += take;
        pcm += take;
        n -= take;

        if (s_asm_len == AUDIO_BLOCK_SAMPLES) {
            if (xQueueSend(s_audio_q, &s_asm, 0) != pdTRUE) {
                s_xrun++;                     /* drop BARU — §4 */
                xQueueSend(s_free_q, &s_asm, 0);
            }
            s_asm = NULL;
            s_asm_len = 0;
        }
    }
    return true;
}

uint32_t audio_out_xrun_count(void)
{
    return s_xrun;
}

uint32_t audio_out_update_count(void)
{
    return s_updates;
}

/* Tunggu sampai deadline absolut; sisa panjang di-delay, sisanya di-spin. */
static inline void audio_wait_until(int64_t deadline_us)
{
    const int64_t rem_us = deadline_us - esp_timer_get_time();
    if (rem_us > 2000) {
        vTaskDelay(pdMS_TO_TICKS((rem_us - 1000) / 1000));
    }
    while (esp_timer_get_time() < deadline_us) {
        /* spin: jitter jauh di bawah periode update */
    }
}

/* Mode test: sinus kontinu-fasa tanpa pacing (free-run). Rate write aktual
   diukur tiap 4096 write dan step disetel ulang, sehingga frekuensi output
   = target meski CPU dibebani rx task — untuk tone tinggi (mis. 100 kHz)
   pacing berbasis task tidak sanggup (butuh periode ~2 us). */
static void audio_tone_loop(void)
{
    const float target = (float)CONFIG_SDR_AUDIO_TONE_HZ;
    const float two_pi = 2.0f * (float)M_PI;

    sdr_config_t cfg;
    sdr_config_get(&cfg);
    float amp = AUDIO_TONE_BASE_AMP * (float)cfg.volume_pct / 100.0f;

    float phase = 0.0f;
    float step = two_pi * target / (float)AUDIO_UPD_HZ;   /* tebakan awal */
    uint32_t n = 0;
    int64_t tmark_us = esp_timer_get_time();

    for (;;) {
        sdm_channel_set_pulse_density(s_chan, (int8_t)(sinf(phase) * amp));
        s_updates++;

        phase += step;
        while (phase >= two_pi) {
            phase -= two_pi;
        }

        if ((++n & AUDIO_TONE_RATE_MASK) == 0) {
            const int64_t dt = esp_timer_get_time() - tmark_us;
            if (dt > 0) {
                step = two_pi * target * (float)dt / 4096.0e6f;
            }
            tmark_us = esp_timer_get_time();

            sdr_config_get(&cfg);
            amp = AUDIO_TONE_BASE_AMP * (float)cfg.volume_pct / 100.0f;
        }
    }
}

/* Mode produksi: putar blok dari audio_q; density = interpolasi linear x4
   antara sampel berurutan (dibawa antar blok agar fase mulus). */
static void audio_block_loop(void)
{
    int64_t sub_idx = 0;
    int64_t epoch_us = esp_timer_get_time();
    bool started = false;
    int8_t prev_density = 0;

    for (;;) {
        int8_t *blk = NULL;
        if (xQueueReceive(s_audio_q, &blk, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const int64_t block_us =
            (int64_t)AUDIO_SUB_BLOCK * 1000000 / AUDIO_UPD_HZ;
        const int64_t now = esp_timer_get_time();

        /* Antrean sempat kosong lebih lama dari satu blok (atau baru mulai)
           → mulai epoch dari "now"; jangan mengejar ketinggalan. */
        if (!started ||
            now - (epoch_us + sub_idx * 1000000 / AUDIO_UPD_HZ) > block_us) {
            epoch_us = now;
            sub_idx = 0;
            started = true;
        }

        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            const int8_t cur = blk[i];
            for (int k = 1; k <= AUDIO_DENS_MULT; k++, sub_idx++) {
                audio_wait_until(
                    epoch_us + (sub_idx + 1) * 1000000 / AUDIO_UPD_HZ);
                const int8_t dens =
                    prev_density +
                    (int16_t)((cur - prev_density) * k) / AUDIO_DENS_MULT;
                sdm_channel_set_pulse_density(s_chan, dens);
                s_updates++;
            }
            prev_density = cur;
        }

        if (xQueueSend(s_free_q, &blk, 0) != pdTRUE) {
            s_xrun++; /* pool tidak konsisten — tidak boleh terjadi */
        }
    }
}

void audio_feed_task(void *arg)
{
    (void)arg;

    if (CONFIG_SDR_AUDIO_TONE_HZ > 0) {
        audio_tone_loop();
    }
    audio_block_loop();
}
