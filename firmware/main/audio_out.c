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
/* Tabel sinus int8 skala penuh (256 titik) untuk tone sintetis: kualitas
 * jauh lebih baik dari sinf() per-sampel (akurat setengah-precision di
 * ESP32 klasik) dan tanpa pembagian floating di loop. */
#define AUDIO_SIN_LOG2   8
#define AUDIO_SIN_LUT_N  (1u << AUDIO_SIN_LOG2)

static sdm_channel_handle_t s_chan;
static QueueHandle_t s_audio_q;   /* int8_t* — blok penuh siap diputar    */
static QueueHandle_t s_free_q;    /* int8_t* — blok kosong milik pool      */
static int8_t *s_asm;             /* blok perakitan (milik pengirim)       */
static size_t s_asm_len;
static volatile bool s_ready;
static volatile uint32_t s_xrun;
static volatile uint32_t s_updates;   /* total update density (diagnostik) */

static int8_t s_sin_lut[AUDIO_SIN_LUT_N];  /* -127..127 skala penuh         */

/* State tone sintetis (persist antar-dispatch; fase mulus saat tone<->blok). */
static uint32_t s_tone_phase;      /* akumulator fase: 1 siklus = 2^32    */
static int64_t  s_tone_prev_us;    /* waktu riil write densitas terakhir   */
static int64_t  s_tone_epoch_us;   /* titik acuan pacing absolut          */
static int64_t  s_tone_upd_idx;    /* indeks sampel pacing                */
static bool     s_tone_started;

/* State pemutaran blok dibawa antar-dispatch (fase mulus saat tone<->blok). */
static int64_t s_blk_sub_idx;
static int64_t s_blk_epoch_us;
static bool    s_blk_started;
static int8_t  s_blk_prev_dens;

static int8_t *audio_pool_pop(void)
{
    int8_t *blk = NULL;
    if (xQueueReceive(s_free_q, &blk, 0) != pdTRUE) {
        return NULL;
    }
    return blk;
}

/* Bangun tabel sinus int8 skala penuh (256 titik, -127..127) SEKALI saat
 * init — memakai sinf() float hanya di sini, bukan di loop pemutar. */
static void audio_build_sin_lut(void)
{
    const float two_pi = 2.0f * (float)M_PI;
    for (uint32_t i = 0; i < AUDIO_SIN_LUT_N; i++) {
        s_sin_lut[i] = (int8_t)(lroundf(127.0f *
                                        sinf(two_pi * (float)i /
                                              (float)AUDIO_SIN_LUT_N)));
    }
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

    audio_build_sin_lut();
    s_tone_phase = 0;
    s_tone_prev_us = 0;
    s_tone_epoch_us = 0;
    s_tone_upd_idx = 0;
    s_tone_started = false;

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

/* Mode test: tone sintetis DDS (akumulator fase int32, 1 siklus = 2^32) +
 * LUT 256 titik dengan interpolasi linear. Fase maju dari WAKTU NYATA
 * (esp_timer) x hz, bukan dari jumlah write — jadi frekuensi EXAK di preset
 * mana pun (250 Hz..100 kHz) & tetap akurat meski sdr_rx_task (prio 12)
 * menunda audio task sesaat. Pacing deadline memakai rumus (idx*1e6)/rate
 * (bukan 1e6/rate yang ter-truncate → dulu melar 48k jadi 50k). */
#define AUDIO_TONE_BATCH   64
/* Tone diagnostik mengikuti VOLUME (Vol+/Vol−): amp = ±127*vol/100
 * (vol 0 = senyap). */
#define AUDIO_TONE_FULL    127   /* skala penuh int8 saat volume 100 %    */
#define AUDIO_TONE_LUT_FS  127   /* skala penuh tabel sinus int8          */

static void audio_tone_batch(uint32_t hz, uint8_t vol_pct)
{
    const int32_t amp = (AUDIO_TONE_FULL * (int)vol_pct) / 100;
    const int64_t now = esp_timer_get_time();
    const int64_t block_us =
        (int64_t)AUDIO_TONE_BATCH * 1000000 / AUDIO_UPD_HZ;
    /* Re-epoch bila task sempat tertunda > satu batch (toggle on/off sering). */
    if (!s_tone_started ||
        now > s_tone_epoch_us + s_tone_upd_idx * (1000000 / AUDIO_UPD_HZ)
            + block_us) {
        s_tone_epoch_us = now;
        s_tone_upd_idx = 0;
        s_tone_prev_us = now;      /* hindari lompatan fase setelah preempsi */
        s_tone_started = true;
    }

    for (int i = 0; i < AUDIO_TONE_BATCH; i++) {
        /* Deadline absolut per update tanpa truncation: (idx*1e6)/rate. */
        const int64_t dl =
            s_tone_epoch_us + (s_tone_upd_idx + 1) * 1000000 / AUDIO_UPD_HZ;
        audio_wait_until(dl);

        /* Fase maju dari WAKTU NYATA (robust terhadap preempsi Core-1):
         * inc = 2^32 * hz/1e6 * dt_us (mod 2^32, wrap aman utk uint32). */
        const int64_t now_w = esp_timer_get_time();
        const int64_t dt_us = now_w - s_tone_prev_us;
        s_tone_prev_us = now_w;
        if (dt_us > 0) {
            /* inc = 2^32 * hz * dt_us / 1e6 (seluruh dalam 64-bit, lalu mod 2^32
             * via wrap uint32). Frekuensi EXAK meski task sempat tertunda. */
            const uint64_t num =
                (uint64_t)4294967296ULL * (uint64_t)hz * (uint64_t)dt_us;
            s_tone_phase += (uint32_t)(num / 1000000ULL);
        }

        /* Indeks LUT = 8 bit atas fase; fraksi dalam sel = 16 bit bawah
         * (dipakai utk interpolasi linear) → waveform sinus bersih. */
        const uint32_t ph = s_tone_phase;
        const uint32_t idx = ph >> 24;
        const uint32_t f16 = (ph & 0xFFFFFFu) >> 8;
        const int32_t a = s_sin_lut[idx & (AUDIO_SIN_LUT_N - 1)];
        const int32_t b = s_sin_lut[(idx + 1) & (AUDIO_SIN_LUT_N - 1)];
        const int32_t lut = a + (((b - a) * (int32_t)f16 + 32767) >> 16);

        sdm_channel_set_pulse_density(
            s_chan, (int8_t)(lut * amp / AUDIO_TONE_LUT_FS));
        s_updates++;
        s_tone_upd_idx++;
    }
}

/* Mode produksi: putar SATU blok dari audio_q; density = interpolasi linear x4
 * antar sampel berurutan (state dibawa antar blok untuk fase mulus). */
static void audio_block_one(void)
{
    int8_t *blk = NULL;
    if (xQueueReceive(s_audio_q, &blk, portMAX_DELAY) != pdTRUE) {
        return;
    }

    const int64_t block_us =
        (int64_t)AUDIO_SUB_BLOCK * 1000000 / AUDIO_UPD_HZ;
    const int64_t now = esp_timer_get_time();

    /* Antrean sempat kosong lebih lama dari satu blok (atau baru mulai)
       → mulai epoch dari "now"; jangan mengejar ketinggalan. */
    if (!s_blk_started ||
        now - (s_blk_epoch_us + s_blk_sub_idx * 1000000 / AUDIO_UPD_HZ) > block_us) {
        s_blk_epoch_us = now;
        s_blk_sub_idx = 0;
        s_blk_started = true;
    }

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        const int8_t cur = blk[i];
        for (int k = 1; k <= AUDIO_DENS_MULT; k++, s_blk_sub_idx++) {
            audio_wait_until(
                s_blk_epoch_us + (s_blk_sub_idx + 1) * 1000000 / AUDIO_UPD_HZ);
            const int8_t dens =
                s_blk_prev_dens +
                (int16_t)((cur - s_blk_prev_dens) * k) / AUDIO_DENS_MULT;
            sdm_channel_set_pulse_density(
                s_chan, dens / CONFIG_SDR_AUDIO_SCALE_DIV);
            s_updates++;
        }
        s_blk_prev_dens = cur;
    }

    if (xQueueSend(s_free_q, &blk, 0) != pdTRUE) {
        s_xrun++; /* pool tidak konsisten — tidak boleh terjadi */
    }
}

/* Dispatcher: pilih tone-sintetis vs blok-antrean per-runtime dari sdr_config
 * (toggle Test di gear). Tone menyisihkan jalur antrean (blok tetap di-push
 * DSP tapi tidak di-drain → speaker hanya mendengar tone). */
void audio_feed_task(void *arg)
{
    (void)arg;

    sdr_config_t cfg;
    sdr_config_get(&cfg);

    for (;;) {
        uint16_t tone_hz = cfg.test_tone_hz;
        if (tone_hz == 0) {
            tone_hz = CONFIG_SDR_AUDIO_TONE_HZ; /* fallback bring-up build */
        }

        if (tone_hz > 0) {
            audio_tone_batch(tone_hz, cfg.volume_pct);
        } else {
            audio_block_one();
        }

        sdr_config_get(&cfg);
    }
}
