/*
 * LDR (light dependent resistor) on ADC1 channel 6 (GPIO34) for the
 * ESP32-3248S035.
 *
 * Uses the ordinary one-shot ADC driver and polls it from a dedicated task
 * pinned to CONFIG_BSP_LDR_TASK_CORE. The continuous (DMA) driver cannot be
 * used while the DAC continuous audio driver runs: on the ESP32 both DMA
 * engines are hardwired to the same I2S0 peripheral, so only one of them can
 * occupy it at a time. One-shot reads leave I2S0 free for the sine generator.
 *
 * Samples are averaged over CONFIG_BSP_LDR_POLL_HZ polls and published every
 * LDR_PUBLISH_US, so the effective resolution improves with oversampling.
 */
#include "bsp_esp323248s035.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_ldr";

#define LDR_PUBLISH_US      5000

static adc_oneshot_unit_handle_t s_adc = NULL;
static TaskHandle_t s_task = NULL;
static volatile int s_value = 0;
static volatile uint32_t s_rate = 0;

static void ldr_task(void *arg)
{
    (void)arg;
    uint32_t interval_us = 1000000UL / CONFIG_BSP_LDR_POLL_HZ;

    uint64_t sum = 0;
    uint32_t cnt = 0;
    uint32_t window_samples = 0;
    int64_t window_start = esp_timer_get_time();
    int64_t last_publish = window_start;
    int64_t next_poll = window_start;
    int raw = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now < next_poll) {
            vTaskDelay(pdMS_TO_TICKS((next_poll - now + 999) / 1000));
        }
        next_poll += interval_us;

        if (adc_oneshot_read(s_adc, BSP_LDR_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            cnt++;
            window_samples++;
        }

        now = esp_timer_get_time();
        if (cnt && now - last_publish >= LDR_PUBLISH_US) {
            s_value = (int)(sum / cnt);
            sum = 0;
            cnt = 0;
            last_publish = now;
        }

        if (now - window_start >= 1000000) {
            s_rate = (uint32_t)((uint64_t)window_samples * 1000000ULL / (uint64_t)(now - window_start));
            window_samples = 0;
            window_start = now;
        }
    }
}

esp_err_t bsp_ldr_init(void)
{
    if (s_adc) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "new unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BSP_LDR_ADC_CHANNEL, &chan_cfg),
                        TAG, "config channel");

    if (xTaskCreatePinnedToCore(ldr_task, "ldr", 4096, NULL, 5, &s_task,
                                CONFIG_BSP_LDR_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ADC one-shot polling, GPIO%d ch%d, %d Hz (I2S0 free for DAC)",
             BSP_LDR_ADC_CHANNEL == ADC_CHANNEL_6 ? 34 : BSP_LDR_ADC_CHANNEL,
             BSP_LDR_ADC_CHANNEL & 0x7, CONFIG_BSP_LDR_POLL_HZ);
    return ESP_OK;
}

esp_err_t bsp_ldr_read(int *raw)
{
    ESP_RETURN_ON_FALSE(raw != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_ERROR(bsp_ldr_init(), TAG, "init");
    *raw = s_value;
    return ESP_OK;
}

uint32_t bsp_ldr_sample_rate(void)
{
    return s_rate;
}
