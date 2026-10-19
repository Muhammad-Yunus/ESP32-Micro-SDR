/*
 * SPDX-License-Identifier: MIT
 *
 * ESP-IDF board support package for the Sunton ESP32-3248S035(C)
 * ("Cheap Yellow Display" style 3.5" 480x320 TFT with resistive... no,
 * capacitive GT911 touch).
 *
 * This component is a direct C port of the proven Arduino/PlatformIO BSP
 * https://github.com/ardnew/ESP32-3248S035 (header only, C++). All pin
 * assignments, the ST7796 initialization sequence, the GT911 raw register
 * protocol and the coordinate mapping are kept identical to that BSP.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/spi_master.h"
#include "driver/i2c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Board pin map (identical to ardnew/ESP32-3248S035)                  */
/* ------------------------------------------------------------------ */

/* Display (ST7796) on HSPI / SPI2 */
#define BSP_TFT_SPI_HOST        SPI2_HOST
#define BSP_TFT_PIN_MISO        12
#define BSP_TFT_PIN_MOSI        13
#define BSP_TFT_PIN_SCLK        14
#define BSP_TFT_PIN_CS          15
#define BSP_TFT_PIN_DC          2
#define BSP_TFT_PIN_BL          27   /* backlight, PWM */
/* no dedicated hardware reset pin on this board */

/* Touch (GT911) on I2C0 */
#define BSP_TOUCH_I2C_PORT      I2C_NUM_0
#define BSP_TOUCH_PIN_SCL       32
#define BSP_TOUCH_PIN_SDA       33
#define BSP_TOUCH_PIN_INT       21
#define BSP_TOUCH_PIN_RST       25
#define BSP_TOUCH_I2C_ADDR      0x5D

/* On board RGB LED (active low) */
#define BSP_LED_PIN_R           4
#define BSP_LED_PIN_G           16
#define BSP_LED_PIN_B           17

/* Passive buzzer / amplifier on GPIO26 (DAC1 / LEDC) */
#define BSP_BUZZER_PIN          26

/* LDR (light sensor) on ADC1 channel 6 */
#define BSP_LDR_ADC_UNIT        ADC_UNIT_1
#define BSP_LDR_ADC_CHANNEL     ADC_CHANNEL_6

/* microSD on VSPI / SPI3 */
#define BSP_SD_SPI_HOST         SPI3_HOST
#define BSP_SD_PIN_MISO         19
#define BSP_SD_PIN_MOSI         23
#define BSP_SD_PIN_SCLK         18
#define BSP_SD_PIN_CS           5

/* Native (portrait) panel geometry */
#define BSP_TFT_H_RES           320
#define BSP_TFT_V_RES           480

#if CONFIG_BSP_TFT_ROTATION_0
#define BSP_TFT_DEFAULT_ROTATION 0
#elif CONFIG_BSP_TFT_ROTATION_90
#define BSP_TFT_DEFAULT_ROTATION 90
#elif CONFIG_BSP_TFT_ROTATION_180
#define BSP_TFT_DEFAULT_ROTATION 180
#else
#define BSP_TFT_DEFAULT_ROTATION 270
#endif

/* ------------------------------------------------------------------ */
/* Global init                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize the whole board (display, touch, LED, buzzer, LDR and,
 *        when enabled, the LVGL port). Safe to call more than once.
 */
esp_err_t bsp_init(void);

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize the ST7796 display (SPI bus, panel IO, panel driver,
 *        backlight).
 */
esp_err_t bsp_tft_init(void);

/** @brief Get the esp_lcd panel handle. */
esp_lcd_panel_handle_t bsp_tft_get_panel(void);

/** @brief Get the esp_lcd panel IO handle. */
esp_lcd_panel_io_handle_t bsp_tft_get_io(void);

/**
 * @brief Set the display rotation.
 * @param rotation 0, 90, 180 or 270 degrees (only multiples of 90 supported).
 */
esp_err_t bsp_tft_set_rotation(int rotation);

/** @brief Get the current rotation in degrees (0/90/180/270). */
int bsp_tft_get_rotation(void);

/** @brief Rotation index (0..3) derived from MADCTL, used for touch mapping. */
uint8_t bsp_tft_rotation_index(void);

/** @brief Current (rotated) display width in pixels. */
uint16_t bsp_tft_width(void);

/** @brief Current (rotated) display height in pixels. */
uint16_t bsp_tft_height(void);

/**
 * @brief Set backlight brightness.
 * @param percent 0 (off) .. 100 (full).
 */
esp_err_t bsp_tft_set_backlight(int percent);

/* ------------------------------------------------------------------ */
/* Touch (GT911)                                                       */
/* ------------------------------------------------------------------ */

/** @brief Initialize the GT911 capacitive touch controller. */
esp_err_t bsp_touch_init(void);

/**
 * @brief Read the current touch point (already mapped to display coordinates).
 * @param[out] x  X coordinate in the current rotation (may be NULL).
 * @param[out] y  Y coordinate in the current rotation (may be NULL).
 * @return true if the screen is currently touched, false otherwise.
 */
bool bsp_touch_read(uint16_t *x, uint16_t *y);

/* ------------------------------------------------------------------ */
/* RGB LED                                                             */
/* ------------------------------------------------------------------ */

/** @brief Initialize the on board RGB LED (active low). */
esp_err_t bsp_led_init(void);

/** @brief Set the RGB LED color (0..255 per channel). */
esp_err_t bsp_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/* ------------------------------------------------------------------ */
/* Buzzer / amplifier (GPIO26)                                         */
/*                                                                     */
/* GPIO26 is DAC channel 1 and drives the on-board FM8002A class-AB    */
/* audio amplifier. Only one of the two drivers below is compiled,     */
/* selected by CONFIG_BSP_AMP_DRIVER_*.                                */
/* ------------------------------------------------------------------ */

#if CONFIG_BSP_AMP_DRIVER_BUZZER

/** @brief Initialize the buzzer PWM. */
esp_err_t bsp_buzzer_init(void);

/** @brief Start a continuous tone at @p freq_hz. */
esp_err_t bsp_buzzer_tone(uint32_t freq_hz);

/** @brief Stop the tone. */
esp_err_t bsp_buzzer_stop(void);

/** @brief Emit a tone for @p duration_ms milliseconds (non blocking). */
esp_err_t bsp_buzzer_beep(uint32_t freq_hz, uint32_t duration_ms);

#endif /* CONFIG_BSP_AMP_DRIVER_BUZZER */

#if CONFIG_BSP_AMP_DRIVER_DAC

  /** Lowest/highest tone frequency the UI should offer. */
  #define BSP_AUDIO_FREQ_MIN      20
  #define BSP_AUDIO_FREQ_MAX      1500

  /** Default output volume in percent of full scale (0..100). */
  #define BSP_AUDIO_VOLUME_DEFAULT 38

/**
 * @brief Initialize the DAC in continuous (DMA) mode on GPIO26 (DAC1) and
 *        start the sine generation task. Output starts silenced.
 */
esp_err_t bsp_audio_init(void);

/**
 * @brief Set the generated sine frequency (clamped to BSP_AUDIO_FREQ_MIN..MAX).
 */
esp_err_t bsp_audio_set_frequency(uint32_t freq_hz);

/** @brief Get the configured sine frequency in Hz. */
uint32_t bsp_audio_get_frequency(void);

  /** @brief Enable or disable the tone output (disabled = silent mid-rail). */
  esp_err_t bsp_audio_set_enabled(bool enabled);

  /** @brief Whether the tone output is currently enabled. */
  bool bsp_audio_is_enabled(void);

  /** @brief Set the output volume (percent, clamped to 0..100). */
  esp_err_t bsp_audio_set_volume(uint32_t percent);

  /** @brief Get the configured output volume in percent. */
  uint32_t bsp_audio_get_volume(void);

  /**
   * @brief Start PCM streaming through the DAC, replacing the generated tone.
   *
   * Mono 16-bit samples written with bsp_audio_pcm_write() are resampled
   * (linear interpolation) to the DAC sample rate and output through GPIO26.
   * While a stream is active bsp_audio_set_enabled() is ignored; the tone
   * resumes automatically once bsp_audio_pcm_stop() is called.
   *
   * @param sample_rate_hz Source sample rate in Hz (8000..48000).
   */
  esp_err_t bsp_audio_pcm_start(uint32_t sample_rate_hz);

  /**
   * @brief Queue mono 16-bit PCM samples for playback.
   *
   * Blocks until all @p count frames are queued, so the caller naturally
   * paces itself. Returns ESP_ERR_INVALID_STATE once the stream is stopped.
   */
  esp_err_t bsp_audio_pcm_write(const int16_t *frames, size_t count);

  /** @brief Stop PCM streaming and return to tone/silence mode. */
  esp_err_t bsp_audio_pcm_stop(void);

  /** @brief Whether a PCM stream is currently active. */
  bool bsp_audio_pcm_active(void);

#endif /* CONFIG_BSP_AMP_DRIVER_DAC */

/* ------------------------------------------------------------------ */
/* LDR (light sensor)                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize the LDR using the one-shot ADC driver.
 *
 * A dedicated task (pinned to CONFIG_BSP_LDR_TASK_CORE) polls ADC1 channel 6
 * at CONFIG_BSP_LDR_POLL_HZ and keeps a running average, published every 5 ms.
 * One-shot mode is used because the DAC continuous audio engine owns I2S0 on
 * this chip. Call bsp_ldr_read() to fetch the value.
 */
esp_err_t bsp_ldr_init(void);

/** @brief Read the latest averaged ADC value (0..4095). */
esp_err_t bsp_ldr_read(int *raw);

/** @brief Measured ADC sample rate in samples per second (0 until measured). */
uint32_t bsp_ldr_sample_rate(void);

/* ------------------------------------------------------------------ */
/* microSD card                                                        */
/* ------------------------------------------------------------------ */

/** @brief Mount the microSD card as FATFS at @p mount_point. */
esp_err_t bsp_sd_mount(const char *mount_point);

/** @brief Unmount the microSD card. */
esp_err_t bsp_sd_unmount(void);

/* ------------------------------------------------------------------ */
/* LVGL                                                                */
/* ------------------------------------------------------------------ */
#if CONFIG_BSP_LVGL

#include "lvgl.h"

/**
 * @brief Initialize the LVGL port: display, input device, tick timer and
 *        the LVGL handler task.
 */
esp_err_t bsp_lvgl_init(void);

/** @brief Take the LVGL mutex before touching any LVGL object. */
bool bsp_lvgl_lock(uint32_t timeout_ms);

/** @brief Release the LVGL mutex. */
void bsp_lvgl_unlock(void);

/** @brief Get the LVGL display created by bsp_lvgl_init(). */
lv_display_t *bsp_lvgl_display(void);

/** @brief Get the LVGL touch input device created by bsp_lvgl_init(). */
lv_indev_t *bsp_lvgl_indev(void);

#endif /* CONFIG_BSP_LVGL */

#ifdef __cplusplus
}
#endif
