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

#include "bsp_esp323248s035.h"
#include "audio_out.h"
#include "dsp_chain.h"
#include "rx_adc.h"
#include "sdr_config.h"
#include "ui/ui_main.h"

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
     */
    if (rx_adc_init() != ESP_OK) {
        ESP_LOGE(TAG, "rx_adc_init gagal — UI jalan tanpa sampling I/Q");
    } else if (xTaskCreatePinnedToCore(sdr_rx_task, "sdr_rx", 16384, NULL, 12,
                                       NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "gagal membuat sdr_rx_task");
    }

    ESP_LOGI(TAG, "boot complete — free internal heap %u bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
