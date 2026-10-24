/*
 * SPDX-License-Identifier: MIT
 *
 * spectrum — pool snapshot + FFT esp-dsp (FR-6).
 *
 * Alur (ARCHITECTURE.md §5 langkah 6–7):
 *   Core 1: dsp_chain mengakumulasi 1024 frame pasca-NCO @12 kHz
 *           → spectrum_ingest() menyalin ke slot pool (drop-LAMA).
 *   Core 0: timer UI memanggil spectrum_get_db() → drain slot terbaru →
 *           Hann window 1024 → zero-pad 2048 → FFT kompleks → magnitude →
 *           dB band penuh terpusat DC → display.
 *
 * Input I/Q kompleks: TIDAK memakai dsps_cplx2reC (itu untuk trik FFT
 * data real); magnitude per bin dihitung langsung dari interleaved.
 *
 * Skala: dBFS relatif penuh (0 dB = tone full-scale I/Q ±1.0 pasca-NCO,
 * pra-BFO). UI menampilkan angka dBFS apa adanya (tanpa offset kalibrasi).
 */
#include "spectrum.h"

#include <math.h>
#include <string.h>

#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "sdr_config.h"

static const char *TAG = "spectrum";

#define SP_N        CONFIG_SDR_FFT_N          /* jendela/capture 1024     */
#define SP_FFT_N    SPECTRUM_FFT_N            /* zero-pad FFT 2048        */
#define SP_MAX_OUT  SPECTRUM_MAX_OUT_NBINS   /* keluaran maks 2048 bin   */
#define SP_POOL     3                         /* slot snapshot (§4)       */
/* Floor log: bin sunyi (re²+im²=0) membaca ≈ 10·log10(SP_EPS) − SP_FS_REFS
 * ≈ −150 dBFS, jauh di bawah noise floor riil (~−110 dBFS). Nilai lebih
 * dalam adalah artifact log, bukan sinyal. */
#define SP_EPS      2.6e-10f


/* Referensi full-scale: tone I/Q = ±1.0 (ADC penuh pasca-NCO). */
#define SP_FS_REFS   (20.0f * log10f((2.0f * (float)SP_N) / 4.0f))

/* Pool: 3 slot heap internal (24 KB total) + dua antrean pointer. */
static QueueHandle_t s_free_q;                /* iq_t* kosong             */
static QueueHandle_t s_snap_q;                /* iq_t* berisi data        */
static iq_t *s_pool[SP_POOL];

/* Buffer FFT (interleaved 2×N float, aligned 16 untuk assembly esp-dsp). */
static float *s_fft;

/* Kunci konsumsi: true = snapshot tidak boleh diambil (self-test). */
static volatile bool s_consume_locked;

/* LUT Hann (jendela window 1024 titik). */
static float s_hann[SP_N];

esp_err_t spectrum_init(void)
{
    ESP_RETURN_ON_FALSE(dsps_fft2r_init_fc32(NULL, SP_FFT_N) == ESP_OK,
                        ESP_FAIL, TAG, "dsps_fft2r_init_fc32(%d) gagal", SP_FFT_N);

    dsps_wind_hann_f32(s_hann, SP_N);

    s_fft = heap_caps_aligned_alloc(16, 2 * SP_FFT_N * sizeof(float),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_fft != NULL, ESP_ERR_NO_MEM, TAG,
                        "buffer FFT %u byte gagal", (unsigned)(2 * SP_FFT_N * sizeof(float)));

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
             "F6: window %d zero-pad FFT %d (%.2f Hz/bin @%d Hz) | "
             "keluaran %d/%d bin (runtime) | pool %d x %d frame | "
             "span +/-%ld Hz | free heap %u",
             SP_N, SP_FFT_N,
             (float)CONFIG_SDR_AUDIO_FS_HZ / (float)SP_FFT_N,
             CONFIG_SDR_AUDIO_FS_HZ, SP_MAX_OUT, SP_MAX_OUT, SP_POOL, SP_N,
             (long)sdr_config_span_half_effective(),
             (unsigned)esp_get_free_heap_size());
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

void spectrum_set_consume_locked(bool locked)
{
    s_consume_locked = locked;
}

bool spectrum_get_db(float *out_db, int *out_nbins)
{
    return spectrum_get_db_ex(out_db, out_nbins, false);
}

bool spectrum_get_db_ex(float *out_db, int *out_nbins, bool ignore_lock)
{
    if (s_snap_q == NULL) {
        return false;                           /* belum init (ui timer duluan) */
    }
    if (s_consume_locked && !ignore_lock) {
        return false;                           /* terkunci (self-test aktif) */
    }
    iq_t *slot = NULL;
    if (xQueueReceive(s_snap_q, &slot, 0) != pdTRUE) {
        return false;                           /* belum ada frame baru    */
    }

    /* Hann window pada jendela 1024 sampel, sisanya nol (zero-pad). */
    for (int k = 0; k < SP_N; k++) {
        s_fft[2 * k]     = slot[k].i * s_hann[k];
        s_fft[2 * k + 1] = slot[k].q * s_hann[k];
    }
    memset(&s_fft[2 * SP_N], 0,
           2 * (SP_FFT_N - SP_N) * sizeof(float));
    xQueueSend(s_free_q, &slot, 0);             /* kembalikan slot pool    */

    dsps_fft2r_fc32(s_fft, SP_FFT_N);
    dsps_bit_rev_fc32(s_fft, SP_FFT_N);

    /* Band penuh terpusat DC (urutan display: frekuensi naik; index
     * out_n/2 = DC = tuned, index 0 = tepi kiri -FS/2).
     *
     * Jumlah bin mengikuti mode runtime sdr_config.fft_bins:
     *   out_n = 2048 → stride 1 → bin FFT natural k (5,86 Hz/bin)
     *   out_n = 1024 → stride 2 → bin FFT natural 2m (11,72 Hz/bin)
     *   out_n = 512  → stride 4 → bin FFT natural 4m (23,44 Hz/bin)
     * Karena DFT-512/1024 dari jendela yang sama == sub-set bin DFT-2048
     * zero-pad (X_2048[j*stride] == X_j[m], magnitudo identik), mode 512
     * cukup decimasi keluaran — tabel esp-dsp & pool tetap sama.
     *
     * Skala dBFS tetap (SP_FS_REFS dari panjang jendela 1024; zero-pad
     * tidak mengubah magnitudo DTFT tone penuh-skala → 0 dB).*/
    uint16_t bins = sdr_config_fft_bins_get();
    if (bins != 512 && bins != 1024 && bins != 2048) {
        bins = 2048;
    }
    const int out_n = (int)bins;
    const int stride = SP_FFT_N / out_n;         /* 1 / 2 / 4            */
    const int center = out_n / 2;

    for (int j = 0; j < out_n; j++) {
        const int m = (j + center) & (out_n - 1);   /* indeks DFT out_n  */
        const int k = (stride * m) & (SP_FFT_N - 1);/* indeks FFT natural*/
        const float re = s_fft[2 * k];
        const float im = s_fft[2 * k + 1];
        out_db[j] = 10.0f * log10f(re * re + im * im + SP_EPS) - SP_FS_REFS;
    }

    *out_nbins = out_n;
    return true;
}
