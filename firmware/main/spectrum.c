/*
 * SPDX-License-Identifier: MIT
 *
 * spectrum — pool snapshot + FFT esp-dsp (FR-6).
 *
 * Alur (ARCHITECTURE.md §5 langkah 6–7):
 *   Core 1: dsp_chain mengakumulasi 1024 frame pasca-NCO @12 kHz
 *           → spectrum_ingest() menyalin ke slot pool (drop-LAMA).
 *   Core 0: timer UI memanggil spectrum_get_db() → drain slot terbaru →
 *           Hann window → FFT kompleks 1024 → magnitude → dB → display.
 *
 * Input I/Q kompleks: TIDAK memakai dsps_cplx2reC (itu untuk trik FFT
 * data real); magnitude per bin dihitung langsung dari interleaved.
 */
#include "spectrum.h"

#include <math.h>
#include <string.h>

#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

static const char *TAG = "spectrum";

#define SP_N        CONFIG_SDR_FFT_N          /* 1024 titik               */
#define SP_POOL     3                         /* slot snapshot (§4)       */
#define SP_EPS      1e-12f                    /* floor log                */

/* Pool: 3 slot heap internal (24 KB total) + dua antrean pointer. */
static QueueHandle_t s_free_q;                /* iq_t* kosong             */
static QueueHandle_t s_snap_q;                /* iq_t* berisi data        */
static iq_t *s_pool[SP_POOL];

/* Buffer FFT (interleaved 2×N float, aligned 16 untuk assembly esp-dsp). */
static float *s_fft;

/* LUT Hann + hasil dB (display order, index tengah = DC). */
static float s_hann[SP_N];
static float s_db[SP_N];

esp_err_t spectrum_init(void)
{
    ESP_RETURN_ON_FALSE(dsps_fft2r_init_fc32(NULL, SP_N) == ESP_OK,
                        ESP_FAIL, TAG, "dsps_fft2r_init_fc32(%d) gagal", SP_N);

    dsps_wind_hann_f32(s_hann, SP_N);

    s_fft = heap_caps_aligned_alloc(16, 2 * SP_N * sizeof(float),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_fft != NULL, ESP_ERR_NO_MEM, TAG,
                        "buffer FFT %u byte gagal", (unsigned)(2 * SP_N * sizeof(float)));

    s_free_q = xQueueCreate(SP_POOL, sizeof(iq_t *));
    s_snap_q = xQueueCreate(SP_POOL, sizeof(iq_t *));
    ESP_RETURN_ON_FALSE(s_free_q != NULL && s_snap_q != NULL, ESP_ERR_NO_MEM,
                        TAG, "pool queue gagal");

    for (int i = 0; i < SP_POOL; i++) {
        s_pool[i] = heap_caps_malloc(SP_N * sizeof(iq_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_pool[i] != NULL, ESP_ERR_NO_MEM, TAG,
                            "slot pool %d gagal", i);
        xQueueSend(s_free_q, &s_pool[i], 0);
    }

    ESP_LOGI(TAG,
             "F6: FFT %d (%.1f Hz/bin @%d Hz) | pool %d x %d frame | span +/-%d Hz",
             SP_N, (float)CONFIG_SDR_AUDIO_FS_HZ / (float)SP_N,
             CONFIG_SDR_AUDIO_FS_HZ, SP_POOL, SP_N,
             CONFIG_SDR_DISPLAY_SPAN_HZ);
    return ESP_OK;
}

void spectrum_ingest(const iq_t *snapshot1024)
{
    if (s_free_q == NULL || s_snap_q == NULL) {
        return;                                 /* belum init              */
    }
    iq_t *slot = NULL;

    if (xQueueReceive(s_free_q, &slot, 0) != pdTRUE) {
        /* Pool habis: buang 1 slot terlama (drop-LAMA, §4). */
        if (xQueueReceive(s_snap_q, &slot, 0) != pdTRUE) {
            return;                             /* defensif; tidak terjadi */
        }
    }

    memcpy(slot, snapshot1024, SP_N * sizeof(iq_t));

    if (xQueueSend(s_snap_q, &slot, 0) != pdTRUE) {
        /* Snap penuh meski sudah drop-oldest di atas: buang satu lagi. */
        iq_t *old = NULL;
        if (xQueueReceive(s_snap_q, &old, 0) == pdTRUE) {
            xQueueSend(s_free_q, &old, 0);
        }
        xQueueSend(s_snap_q, &slot, 0);
    }
}

bool spectrum_get_db(float *out_db, int *out_nbins)
{
    if (s_snap_q == NULL) {
        return false;                           /* belum init (ui timer duluan) */
    }
    iq_t *slot = NULL;
    if (xQueueReceive(s_snap_q, &slot, 0) != pdTRUE) {
        return false;                           /* belum ada frame baru    */
    }

    /* Hann window: salin interleaved + window per kanal. */
    for (int k = 0; k < SP_N; k++) {
        s_fft[2 * k]     = slot->i * s_hann[k];
        s_fft[2 * k + 1] = slot->q * s_hann[k];
    }
    xQueueSend(s_free_q, &slot, 0);             /* kembalikan slot pool    */

    dsps_fft2r_fc32(s_fft, SP_N);
    dsps_bit_rev_fc32(s_fft, SP_N);

    /* dB per bin dalam urutan display: j=0 → bin −N/2, j=N/2 → DC. */
    for (int j = 0; j < SP_N; j++) {
        const int k = (j < SP_N / 2) ? (j + SP_N / 2) : (j - SP_N / 2);
        const float re = s_fft[2 * k];
        const float im = s_fft[2 * k + 1];
        out_db[j] = 10.0f * log10f(re * re + im * im + SP_EPS);
    }

    *out_nbins = SP_N;
    return true;
}
