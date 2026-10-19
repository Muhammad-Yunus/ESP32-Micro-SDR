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

#include "bsp_esp323248s035.h"
#include "ui_main.h"

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

    /* Audio: DAC1/GPIO26 via dac_continuous (mengklaim I2S0 internal) */
    ESP_ERROR_CHECK(bsp_audio_init());

    /* LVGL port: task handler di Core 0 (CONFIG_BSP_LVGL_TASK_CORE) */
    ESP_ERROR_CHECK(bsp_lvgl_init());

    /* UI dasar (label frekuensi) — objek LVGL dibuat di bawah lock */
    ESP_ERROR_CHECK(ui_init());

    ESP_LOGI(TAG, "boot complete — free internal heap %u bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
