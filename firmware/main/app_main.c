/*
 * SPDX-License-Identifier: MIT
 *
 * ESP32-Micro-SDR — entry point.
 *
 * Urutan inisialisasi WAJIB mengikuti docs/ARCHITECTURE.md §7.
 * DILARANG memanggil bsp_init() (menginisialisasi LDR pada ADC1_CH6 yang
 * akan bentrok dengan ADC1 DMA untuk I/Q — lihat bsp.c:23 BSP).
 */
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "bsp_esp323248s035.h"
#include "audio_out.h"
#include "dsp_chain.h"
#include "rx_adc.h"
#include "sdr_config.h"
#include "spectrum.h"
#include "ui/ui_main.h"

#if CONFIG_SDR_SELFTEST
#include "selftest.h"
#endif

static const char *TAG = "app";

void app_main(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "ESP32-Micro-SDR boot — %d cores, free internal heap %u bytes",
             info.cores, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Display & touch */
    ESP_ERROR_CHECK(bsp_tft_init());
    ESP_ERROR_CHECK(bsp_touch_init());

    /* LED opsional (tanpa efek samping peripheral lain) */
    ESP_ERROR_CHECK(bsp_led_init());

    /* LVGL port: task handler di Core 0 (CONFIG_BSP_LVGL_TASK_CORE) */
    ESP_ERROR_CHECK(bsp_lvgl_init());

    /* Konfigurasi runtime (defaults dari Kconfig) — sebelum UI */
    sdr_config_init();

    /* UI dasar (label frekuensi) — objek LVGL dibuat di bawah lock */
    ESP_ERROR_CHECK(ui_init());

    /* Rantai DSP (state; tahap §6 menyusul di F5) */
    ESP_ERROR_CHECK(dsp_chain_init());

    /* Spectrum FFT (F6) — pool snapshot + tabel FFT esp-dsp */
    ESP_ERROR_CHECK(spectrum_init());

    /*
     * Audio out: SDM (sigma-delta) 1-bit di GPIO26 — BUKAN
     * bsp_audio_init(): dac_continuous mengklaim I2S0, sama dengan ADC DMA
     * (I/Q) — lihat docs/ARCHITECTURE.md §7 dan docs/TECH_STACK.md §6.
     * Kegagalan tidak fatal: UI tetap hidup tanpa output audio.
     */
    if (audio_out_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio_out_init gagal — UI jalan tanpa output audio");
    } else if (xTaskCreatePinnedToCore(audio_feed_task, "audio_feed", 6144,
                                       NULL, 11, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "gagal membuat audio_feed_task");
    }

    /*
     * ADC DMA start — TERAKHIR, agar konsumen data sudah siap.
     * Kegagalan di sini tidak fatal: UI tetap hidup (tanpa data I/Q).
     *
     * Mode self-test (CONFIG_SDR_SELFTEST=y): ADC TIDAK diinisialisasi
     * sama sekali (GPIO35/GPIO36 tidak dipakai); data I/Q disuplai
     * generator sintetis (CONVENTIONS.md §5).
     */
#if CONFIG_SDR_SELFTEST
    /*
     * Task self-test: stack & TCB statis (8 KB) — heap internal setelah
     * spectrum_init() sudah terfragmentasi sehingga alokasi stack dinamis
     * tidak reliable, dan stack 4 KB memicu overflow.
     */
    ESP_LOGI(TAG, "heap internal: free %u byte, blok terbesar %u byte",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!selftest_start()) {
        ESP_LOGE(TAG, "gagal membuat selftest_task");
    } else {
        ESP_LOGI(TAG, "selftest_task dibuat (stack statis %u byte, Core 1)",
                 (unsigned)8192);
    }
#else
    if (rx_adc_init() != ESP_OK) {
        ESP_LOGE(TAG, "rx_adc_init gagal — UI jalan tanpa sampling I/Q");
    } else if (xTaskCreatePinnedToCore(sdr_rx_task, "sdr_rx", 16384, NULL, 12,
                                       NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "gagal membuat sdr_rx_task");
    }
#endif

    ESP_LOGI(TAG, "boot complete — free internal heap %u bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
