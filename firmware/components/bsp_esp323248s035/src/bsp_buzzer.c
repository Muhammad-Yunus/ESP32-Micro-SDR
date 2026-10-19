/*
 * Passive buzzer / amplifier on GPIO26 for the ESP32-3248S035.
 * Port of AMP_PWM from https://github.com/ardnew/ESP32-3248S035
 *
 * The Arduino BSP uses tone()/noTone(); the IDF has no software tone API, so
 * the tone is generated with a dedicated LEDC timer whose frequency is changed
 * on the fly.
 */
#include "bsp_esp323248s035.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/ledc.h"
#include "esp_timer.h"

static const char *TAG = "bsp_buzzer";

#define BZ_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BZ_LEDC_TIMER   LEDC_TIMER_1
#define BZ_LEDC_CHANNEL LEDC_CHANNEL_4
#define BZ_LEDC_RES     LEDC_TIMER_10_BIT
#define BZ_LEDC_DUTY    512 /* 50% of 2^10 */

static bool s_ready = false;
static esp_timer_handle_t s_stop_timer = NULL;

static void buzzer_stop_cb(void *arg)
{
    (void)arg;
    bsp_buzzer_stop();
}

esp_err_t bsp_buzzer_init(void)
{
    if (s_ready) return ESP_OK;

    ledc_timer_config_t timer = {
        .speed_mode = BZ_LEDC_MODE,
        .duty_resolution = BZ_LEDC_RES,
        .timer_num = BZ_LEDC_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t chan = {
        .gpio_num = BSP_BUZZER_PIN,
        .speed_mode = BZ_LEDC_MODE,
        .channel = BZ_LEDC_CHANNEL,
        .timer_sel = BZ_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "ledc channel");

    const esp_timer_create_args_t args = {
        .callback = buzzer_stop_cb,
        .name = "buzzer_stop",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_stop_timer), TAG, "timer create");

    s_ready = true;
    return ESP_OK;
}

esp_err_t bsp_buzzer_tone(uint32_t freq_hz)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (freq_hz < 20) freq_hz = 20;

    ESP_RETURN_ON_ERROR(ledc_set_freq(BZ_LEDC_MODE, BZ_LEDC_TIMER, freq_hz), TAG, "set freq");
    ESP_RETURN_ON_ERROR(ledc_set_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL, BZ_LEDC_DUTY), TAG, "set duty");
    return ledc_update_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL);
}

esp_err_t bsp_buzzer_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    if (s_stop_timer && esp_timer_is_active(s_stop_timer)) {
        esp_timer_stop(s_stop_timer);
    }
    ESP_RETURN_ON_ERROR(ledc_set_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL, 0), TAG, "duty 0");
    return ledc_update_duty(BZ_LEDC_MODE, BZ_LEDC_CHANNEL);
}

esp_err_t bsp_buzzer_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    ESP_RETURN_ON_ERROR(bsp_buzzer_tone(freq_hz), TAG, "tone");
    ESP_RETURN_ON_FALSE(s_stop_timer != NULL, ESP_ERR_INVALID_STATE, TAG, "timer");
    if (esp_timer_is_active(s_stop_timer)) {
        esp_timer_stop(s_stop_timer);
    }
    return esp_timer_start_once(s_stop_timer, (uint64_t)duration_ms * 1000ULL);
}
