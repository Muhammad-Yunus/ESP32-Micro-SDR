/*
 * On board RGB LED (active low) for the ESP32-3248S035.
 * Port of RGB_PWM from https://github.com/ardnew/ESP32-3248S035
 */
#include "bsp_esp323248s035.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/ledc.h"

static const char *TAG = "bsp_led";

#define LED_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define LED_LEDC_TIMER  LEDC_TIMER_0
#define LED_LEDC_FREQ_HZ 5000
#define LED_LEDC_RES    LEDC_TIMER_8_BIT
#define LED_LEDC_HRES   255

#define LED_CH_R LEDC_CHANNEL_1
#define LED_CH_G LEDC_CHANNEL_2
#define LED_CH_B LEDC_CHANNEL_3

static bool s_ready = false;

esp_err_t bsp_led_init(void)
{
    if (s_ready) return ESP_OK;

    ledc_timer_config_t timer = {
        .speed_mode = LED_LEDC_MODE,
        .duty_resolution = LED_LEDC_RES,
        .timer_num = LED_LEDC_TIMER,
        .freq_hz = LED_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        return err;
    }

    const int pins[3] = { BSP_LED_PIN_R, BSP_LED_PIN_G, BSP_LED_PIN_B };
    const ledc_channel_t chans[3] = { LED_CH_R, LED_CH_G, LED_CH_B };
    for (int i = 0; i < 3; ++i) {
        ledc_channel_config_t chan = {
            .gpio_num = pins[i],
            .speed_mode = LED_LEDC_MODE,
            .channel = chans[i],
            .timer_sel = LED_LEDC_TIMER,
            .duty = LED_LEDC_HRES, /* active low -> off */
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "ledc channel");
    }

    s_ready = true;
    return bsp_led_set_rgb(0, 0, 0);
}

esp_err_t bsp_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* active low: invert the duty */
    ledc_set_duty(LED_LEDC_MODE, LED_CH_R, LED_LEDC_HRES - r);
    ledc_update_duty(LED_LEDC_MODE, LED_CH_R);
    ledc_set_duty(LED_LEDC_MODE, LED_CH_G, LED_LEDC_HRES - g);
    ledc_update_duty(LED_LEDC_MODE, LED_CH_G);
    ledc_set_duty(LED_LEDC_MODE, LED_CH_B, LED_LEDC_HRES - b);
    ledc_update_duty(LED_LEDC_MODE, LED_CH_B);
    return ESP_OK;
}
