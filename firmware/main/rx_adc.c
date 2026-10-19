/*
 * SPDX-License-Identifier: MIT
 *
 * rx_adc — ADC1 continuous DMA pada pola CH0 (I, GPIO36) + CH7 (Q, GPIO35).
 *
 * Catatan hardware (ESP32 klasik, IDF v6.1):
 *  - `adc_continuous` memakai I2S0 sebagai FIFO DMA; I2S0 karena itu TIDAK
 *    boleh dipakai `dac_continuous` (audio BSP) — lihat docs/TECH_STACK.md §6.
 *  - Hasil konversi berformat TYPE1 (2 byte/konversi): bit[11:0] data 12-bit,
 *    bit[15:12] indeks channel. Nilai dinormalkan `(raw − 2048) / 2048`.
 *  - `sample_freq_hz` adalah frekuensi AGREGAT semua channel pada pola, jadi
 *    rate per kanal = agregat / jumlah pola (SDR_ADC_FS_HZ × 2).
 *  - `CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y`: callback ISR wajib IRAM_ATTR.
 */
#include "rx_adc.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "esp_adc/adc_continuous.h"

static const char *TAG = "rx_adc";

#define RX_ADC_UNIT             ADC_UNIT_1
#define RX_ADC_CH_I             ADC_CHANNEL_0   /* GPIO36 */
#define RX_ADC_CH_Q             ADC_CHANNEL_7   /* GPIO35 */
#define RX_ADC_PATTERN_NUM      2
#define RX_ADC_AGG_FS_HZ        (CONFIG_SDR_ADC_FS_HZ * RX_ADC_PATTERN_NUM)
#define RX_ADC_CONV_FRAME_BYTES CONFIG_SDR_ADC_CONV_FRAME_BYTES
#define RX_ADC_POOL_BYTES       CONFIG_SDR_ADC_POOL_BYTES
#define RX_ADC_READ_TIMEOUT_MS  100
#define RX_ADC_RATE_WINDOW_US   500000          /* 0,5 s — jendela ukur rate   */

/* Kconfig menyimpan nilai dB; driver meminta nilai enum `adc_atten_t`. */
#if CONFIG_SDR_ADC_ATTEN_DB == 0
#define RX_ADC_ATTEN            ADC_ATTEN_DB_0
#elif CONFIG_SDR_ADC_ATTEN_DB == 3
#define RX_ADC_ATTEN            ADC_ATTEN_DB_2_5
#elif CONFIG_SDR_ADC_ATTEN_DB == 6
#define RX_ADC_ATTEN            ADC_ATTEN_DB_6
#elif CONFIG_SDR_ADC_ATTEN_DB == 11 || CONFIG_SDR_ADC_ATTEN_DB == 12
#define RX_ADC_ATTEN            ADC_ATTEN_DB_12
#else
#error "SDR_ADC_ATTEN_DB harus 0, 3, 6, atau 11 (lihat help Kconfig)"
#endif

_Static_assert((RX_ADC_CONV_FRAME_BYTES % SOC_ADC_DIGI_DATA_BYTES_PER_CONV) == 0,
               "conv_frame_size harus kelipatan SOC_ADC_DIGI_DATA_BYTES_PER_CONV");
_Static_assert(RX_ADC_AGG_FS_HZ >= SOC_ADC_SAMPLE_FREQ_THRES_LOW &&
               RX_ADC_AGG_FS_HZ <= SOC_ADC_SAMPLE_FREQ_THRES_HIGH,
               "sample_freq_hz di luar rentang yang didukung SoC");

static adc_continuous_handle_t s_handle;
static uint8_t s_raw[RX_ADC_CONV_FRAME_BYTES];

static volatile uint32_t s_pool_ovf;
static volatile uint32_t s_fs_hz = CONFIG_SDR_ADC_FS_HZ;
static uint32_t s_rate_frames;
static int64_t s_rate_t0_us;

/* Satu sampel I yang belum berpasangan dengan Q (batas frame DMA). */
static bool s_pending_i;
static float s_pending_i_val;

static IRAM_ATTR bool rx_adc_pool_ovf_cb(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data)
{
    (void)handle;
    (void)edata;
    (void)user_data;
    s_pool_ovf++;
    return false;
}

static bool rx_adc_ready(void)
{
    if (s_handle != NULL) {
        return true;
    }
    ESP_LOGE(TAG, "rx_adc_read() sebelum rx_adc_init()");
    return false;
}

esp_err_t rx_adc_init(void)
{
    ESP_RETURN_ON_FALSE(s_handle == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "sudah diinisialisasi");

    adc_continuous_handle_cfg_t hdl_cfg = {
        .max_store_buf_size = RX_ADC_POOL_BYTES,
        .conv_frame_size = RX_ADC_CONV_FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&hdl_cfg, &s_handle), TAG,
                        "new_handle (I2S0 sudah dipakai peripheral lain?)");

    /* Bit width harus eksplisit: driver menolak ADC_BITWIDTH_DEFAULT (0). */
    const adc_digi_pattern_config_t pattern[RX_ADC_PATTERN_NUM] = {
        { .atten = RX_ADC_ATTEN, .channel = RX_ADC_CH_I,
          .unit = RX_ADC_UNIT, .bit_width = ADC_BITWIDTH_12 },
        { .atten = RX_ADC_ATTEN, .channel = RX_ADC_CH_Q,
          .unit = RX_ADC_UNIT, .bit_width = ADC_BITWIDTH_12 },
    };
    const adc_continuous_config_t cfg = {
        .pattern_num = RX_ADC_PATTERN_NUM,
        .adc_pattern = (adc_digi_pattern_config_t *)pattern,
        .sample_freq_hz = RX_ADC_AGG_FS_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_config(s_handle, &cfg), TAG, "config");

    const adc_continuous_evt_cbs_t cbs = {
        .on_pool_ovf = rx_adc_pool_ovf_cb,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_register_event_callbacks(s_handle, &cbs,
                                                                NULL),
                        TAG, "register_event_callbacks");
    ESP_RETURN_ON_ERROR(adc_continuous_start(s_handle), TAG, "start");

    s_rate_t0_us = esp_timer_get_time();
    ESP_LOGI(TAG, "I/Q DMA: CH0=GPIO36, CH7=GPIO35, %d Hz/kanal (%d Hz agregat), "
             "atten %d dB, pool %d B, frame %d B",
             CONFIG_SDR_ADC_FS_HZ, RX_ADC_AGG_FS_HZ, CONFIG_SDR_ADC_ATTEN_DB,
             RX_ADC_POOL_BYTES, RX_ADC_CONV_FRAME_BYTES);
    return ESP_OK;
}

void rx_adc_deinit(void)
{
    if (s_handle == NULL) {
        return;
    }
    adc_continuous_stop(s_handle);
    adc_continuous_deinit(s_handle);
    s_handle = NULL;
}

size_t rx_adc_read(iq_t *out, size_t max_frames)
{
    if (!rx_adc_ready() || out == NULL || max_frames == 0) {
        return 0;
    }

    size_t frames = 0;
    while (frames < max_frames) {
        uint32_t got_bytes = 0;
        if (adc_continuous_read(s_handle, s_raw, sizeof(s_raw), &got_bytes,
                                RX_ADC_READ_TIMEOUT_MS) != ESP_OK) {
            break;      /* timeout / error: serahkan apa yang sudah terkumpul */
        }

        for (uint32_t off = 0;
             off + SOC_ADC_DIGI_RESULT_BYTES <= got_bytes;
             off += SOC_ADC_DIGI_RESULT_BYTES) {
            const uint16_t word = (uint16_t)s_raw[off] |
                                  ((uint16_t)s_raw[off + 1] << 8);
            const uint8_t ch = (uint8_t)((word >> 12) & 0x0Fu);
            const float val = ((float)(word & 0x0FFFu) - 2048.0f) / 2048.0f;

            if (ch == RX_ADC_CH_I) {
                s_pending_i_val = val;
                s_pending_i = true;
            } else if (ch == RX_ADC_CH_Q && s_pending_i) {
                if (frames >= max_frames) {
                    break;
                }
                out[frames].i = s_pending_i_val;
                out[frames].q = val;
                frames++;
                s_pending_i = false;
            }
        }
    }

    if (frames > 0) {
        s_rate_frames += frames;
        const int64_t now_us = esp_timer_get_time();
        const int64_t span_us = now_us - s_rate_t0_us;
        if (span_us >= RX_ADC_RATE_WINDOW_US) {
            s_fs_hz = (uint32_t)(((int64_t)s_rate_frames * 1000000) / span_us);
            s_rate_frames = 0;
            s_rate_t0_us = now_us;
        }
    }
    return frames;
}

uint32_t rx_adc_measured_fs_hz(void)
{
    return s_fs_hz;
}

uint32_t rx_adc_overrun_count(void)
{
    return s_pool_ovf;
}
