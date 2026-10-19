/*
 * Global initializer for the ESP32-3248S035 BSP.
 */
#include "bsp_esp323248s035.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp";

esp_err_t bsp_init(void)
{
    ESP_LOGI(TAG, "initializing ESP32-3248S035 BSP");

    ESP_RETURN_ON_ERROR(bsp_tft_init(), TAG, "tft");
    ESP_RETURN_ON_ERROR(bsp_touch_init(), TAG, "touch");
    ESP_RETURN_ON_ERROR(bsp_led_init(), TAG, "led");
#if CONFIG_BSP_AMP_DRIVER_BUZZER
    ESP_RETURN_ON_ERROR(bsp_buzzer_init(), TAG, "buzzer");
#elif CONFIG_BSP_AMP_DRIVER_DAC
    ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "audio");
#endif
    ESP_RETURN_ON_ERROR(bsp_ldr_init(), TAG, "ldr");

#if CONFIG_BSP_LVGL
    ESP_RETURN_ON_ERROR(bsp_lvgl_init(), TAG, "lvgl");
#endif

    ESP_LOGI(TAG, "BSP ready (%ux%u, rotation %d)",
             bsp_tft_width(), bsp_tft_height(), bsp_tft_get_rotation());
    return ESP_OK;
}
