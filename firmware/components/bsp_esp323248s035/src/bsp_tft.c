/*
 * ST7796 display driver for the ESP32-3248S035.
 *
 * Direct C port of the Arduino BSP:
 *   https://github.com/ardnew/ESP32-3248S035
 * The command sequence, MADCTL handling and geometry are kept identical.
 */
#include "bsp_esp323248s035.h"

#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_compiler.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_tft";

/* ---------------- ST7796 command codes ---------------- */
#define ST7796_SWRESET  0x01
#define ST7796_SLPIN    0x10
#define ST7796_SLPOUT   0x11
#define ST7796_NORON    0x13
#define ST7796_INVOFF   0x20
#define ST7796_INVON    0x21
#define ST7796_DISPOFF  0x28
#define ST7796_DISPON   0x29
#define ST7796_CASET    0x2A
#define ST7796_RASET    0x2B
#define ST7796_RAMWR    0x2C
#define ST7796_MADCTL   0x36
#define ST7796_COLMOD   0x3A
#define ST7796_PGC      0xE0
#define ST7796_NGC      0xE1
#define ST7796_CSCON    0xF0

/* MADCTL bits */
#define MADCTL_MY       0x80
#define MADCTL_MX       0x40
#define MADCTL_MV       0x20
#define MADCTL_BGR      0x08

#define ST7796_COLMOD_RGB565 0x55

/* ---------------- backlight ---------------- */
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ 5000
#define BL_LEDC_RES     LEDC_TIMER_8_BIT

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    bool inverted;
} st7796_panel_t;

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static bool s_backlight_ready = false;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static uint8_t st7796_madctl_value(st7796_panel_t *p)
{
    uint8_t madctl = 0;
    if (p->mirror_y) madctl |= MADCTL_MY;
    if (p->mirror_x) madctl |= MADCTL_MX;
    if (p->swap_xy)  madctl |= MADCTL_MV;
    /* This panel's subpixel order is BGR, so the BGR bit must be set for
     * plain RGB565 data to show with the correct channel order. */
    madctl |= MADCTL_BGR;
    return madctl;
}

static esp_err_t st7796_tx_param(st7796_panel_t *p, int cmd, const void *param, size_t len)
{
    return esp_lcd_panel_io_tx_param(p->io, cmd, param, len);
}

static esp_err_t st7796_apply_madctl(st7796_panel_t *p)
{
    uint8_t madctl = st7796_madctl_value(p);
    return st7796_tx_param(p, ST7796_MADCTL, &madctl, 1);
}

/* ------------------------------------------------------------------ */
/* panel interface implementation                                      */
/* ------------------------------------------------------------------ */

static esp_err_t st7796_reset(esp_lcd_panel_t *panel)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(p->reset_gpio_num, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t st7796_init(esp_lcd_panel_t *panel)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    /* software reset */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_SWRESET, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Command2 BK0 (extension command set unlock) */
    uint8_t csc3[] = {0xC3};
    uint8_t csc96[] = {0x96};
    ESP_RETURN_ON_ERROR(st7796_tx_param(p, ST7796_CSCON, csc3, sizeof(csc3)), TAG, "bk0 c3");
    ESP_RETURN_ON_ERROR(st7796_tx_param(p, ST7796_CSCON, csc96, sizeof(csc96)), TAG, "bk0 96");

    /* pixel format: 16 bpp */
    uint8_t colmod = ST7796_COLMOD_RGB565;
    ESP_RETURN_ON_ERROR(st7796_tx_param(p, ST7796_COLMOD, &colmod, 1), TAG, "colmod");

    /* memory access control */
    ESP_RETURN_ON_ERROR(st7796_apply_madctl(p), TAG, "madctl");

    /* positive gamma */
    uint8_t pgc[] = {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54,
                     0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B};
    /* negative gamma */
    uint8_t ngc[] = {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43,
                     0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B};
    ESP_RETURN_ON_ERROR(st7796_tx_param(p, ST7796_PGC, pgc, sizeof(pgc)), TAG, "pgc");
    ESP_RETURN_ON_ERROR(st7796_tx_param(p, ST7796_NGC, ngc, sizeof(ngc)), TAG, "ngc");

    /* Command2 BK1 */
    uint8_t bk1_3c[] = {0x3C};
    uint8_t bk1_69[] = {0x69};
    ESP_RETURN_ON_ERROR(st7796_tx_param(p, ST7796_CSCON, bk1_3c, sizeof(bk1_3c)), TAG, "bk1 3c");
    ESP_RETURN_ON_ERROR(st7796_tx_param(p, ST7796_CSCON, bk1_69, sizeof(bk1_69)), TAG, "bk1 69");

    /* display inversion off, normal mode on, sleep out, display on */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_INVOFF, NULL, 0), TAG, "invoff");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_NORON, NULL, 0), TAG, "noron");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_SLPOUT, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_DISPON, NULL, 0), TAG, "dispon");
    vTaskDelay(pdMS_TO_TICKS(20));

    p->inverted = false;
    return ESP_OK;
}

static esp_err_t st7796_del(esp_lcd_panel_t *panel)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t st7796_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    /* The MADCTL MV bit performs the row/column exchange in hardware (same
     * behaviour as the ESP-IDF built-in drivers and the original Arduino BSP),
     * so the coordinates are passed through unchanged. */
    x_start += p->x_gap;
    x_end   += p->x_gap;
    y_start += p->y_gap;
    y_end   += p->y_gap;

    uint8_t caset[4] = {
        (uint8_t)(x_start >> 8), (uint8_t)(x_start & 0xFF),
        (uint8_t)((x_end - 1) >> 8), (uint8_t)((x_end - 1) & 0xFF)
    };
    uint8_t raset[4] = {
        (uint8_t)(y_start >> 8), (uint8_t)(y_start & 0xFF),
        (uint8_t)((y_end - 1) >> 8), (uint8_t)((y_end - 1) & 0xFF)
    };

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CASET, caset, sizeof(caset)), TAG, "caset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_RASET, raset, sizeof(raset)), TAG, "raset");

    size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * 2;
    return esp_lcd_panel_io_tx_color(io, ST7796_RAMWR, color_data, len);
}

static esp_err_t st7796_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    p->mirror_x = mirror_x;
    p->mirror_y = mirror_y;
    return st7796_apply_madctl(p);
}

static esp_err_t st7796_swap_xy(esp_lcd_panel_t *panel, bool swap_xy)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    p->swap_xy = swap_xy;
    return st7796_apply_madctl(p);
}

static esp_err_t st7796_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t st7796_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    int cmd = invert_color_data ? ST7796_INVON : ST7796_INVOFF;
    p->inverted = invert_color_data;
    return esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
}

static esp_err_t st7796_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    int cmd = on_off ? ST7796_DISPON : ST7796_DISPOFF;
    return esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
}

static esp_err_t st7796_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7796_panel_t *p = __containerof(panel, st7796_panel_t, base);
    int cmd = sleep ? ST7796_SLPIN : ST7796_SLPOUT;
    return esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
}

static esp_err_t st7796_set_brightness(esp_lcd_panel_t *panel, int brightness)
{
    (void)panel;
    return bsp_tft_set_backlight(brightness);
}

static esp_err_t st7796_draw_bitmap_2d(esp_lcd_panel_t *panel, int x_start, int y_start,
                                       int x_end, int y_end, const void *src_data,
                                       size_t src_x_size, size_t src_y_size,
                                       int src_x_start, int src_y_start,
                                       int src_x_end, int src_y_end)
{
    /* Not needed by the BSP: not supported, mirror to a plain draw. */
    (void)panel; (void)x_start; (void)y_start; (void)x_end; (void)y_end;
    (void)src_data; (void)src_x_size; (void)src_y_size;
    (void)src_x_start; (void)src_y_start; (void)src_x_end; (void)src_y_end;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t st7796_new_panel(esp_lcd_panel_io_handle_t io, int reset_gpio_num,
                                  esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st7796_panel_t *p = NULL;
    ESP_GOTO_ON_FALSE(io && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid arg");
    p = calloc(1, sizeof(st7796_panel_t));
    ESP_GOTO_ON_FALSE(p, ESP_ERR_NO_MEM, err, TAG, "no mem for panel");

    p->reset_gpio_num = reset_gpio_num;
    p->io = io;
    p->base.reset = st7796_reset;
    p->base.init = st7796_init;
    p->base.del = st7796_del;
    p->base.draw_bitmap = st7796_draw_bitmap;
    p->base.draw_bitmap_2d = st7796_draw_bitmap_2d;
    p->base.mirror = st7796_mirror;
    p->base.swap_xy = st7796_swap_xy;
    p->base.set_gap = st7796_set_gap;
    p->base.invert_color = st7796_invert_color;
    p->base.disp_on_off = st7796_disp_on_off;
    p->base.disp_sleep = st7796_disp_sleep;
    p->base.set_brightness = st7796_set_brightness;

    if (reset_gpio_num >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << reset_gpio_num,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&cfg), err, TAG, "reset gpio");
    }

    *ret_panel = &p->base;
    return ESP_OK;

err:
    if (p) free(p);
    return ret;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t backlight_init(void)
{
    if (s_backlight_ready) return ESP_OK;

    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t chan = {
        .gpio_num = BSP_TFT_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "ledc channel");

    s_backlight_ready = true;
    return ESP_OK;
}

static const int rotation_map[4][3] = {
    /* swap_xy, mirror_x, mirror_y */
    { 0, 0, 1 }, /*   0 deg: MADCTL 0x80 */
    { 1, 0, 0 }, /*  90 deg: MADCTL 0x20 */
    { 0, 1, 0 }, /* 180 deg: MADCTL 0x40 */
    { 1, 1, 1 }, /* 270 deg: MADCTL 0xE0 (ardnew default) */
};

static esp_err_t apply_rotation(int rotation)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    int idx;
    switch (rotation) {
        case 0:   idx = 0; break;
        case 90:  idx = 1; break;
        case 180: idx = 2; break;
        case 270: idx = 3; break;
        default:  return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, rotation_map[idx][0]), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, rotation_map[idx][1], rotation_map[idx][2]), TAG, "mirror");
    return ESP_OK;
}

esp_err_t bsp_tft_init(void)
{
    if (s_panel) return ESP_OK;

    spi_bus_config_t buscfg = {
        .mosi_io_num = BSP_TFT_PIN_MOSI,
        .miso_io_num = BSP_TFT_PIN_MISO,
        .sclk_io_num = BSP_TFT_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_TFT_H_RES * 80 * 2,
    };
    esp_err_t err = spi_bus_initialize(BSP_TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BSP_TFT_PIN_CS,
        .dc_gpio_num = BSP_TFT_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_BSP_TFT_SPI_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_TFT_SPI_HOST,
                                                 &io_config, &s_io), TAG, "new panel io");

    ESP_RETURN_ON_ERROR(st7796_new_panel(s_io, -1, &s_panel), TAG, "new panel");

    ESP_RETURN_ON_ERROR(apply_rotation(BSP_TFT_DEFAULT_ROTATION), TAG, "rotation");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(bsp_tft_set_backlight(CONFIG_BSP_TFT_BACKLIGHT_DEFAULT), TAG, "bl set");

    ESP_LOGI(TAG, "ST7796 initialized at %d MHz, rotation %d",
             CONFIG_BSP_TFT_SPI_CLK_HZ / 1000000, BSP_TFT_DEFAULT_ROTATION);
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_tft_get_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t bsp_tft_get_io(void)
{
    return s_io;
}

esp_err_t bsp_tft_set_rotation(int rotation)
{
    return apply_rotation(rotation);
}

int bsp_tft_get_rotation(void)
{
    if (!s_panel) return BSP_TFT_DEFAULT_ROTATION;
    uint8_t idx = bsp_tft_rotation_index();
    static const int map[4] = {0, 90, 180, 270};
    return map[idx];
}

uint8_t bsp_tft_rotation_index(void)
{
    if (!s_panel) return (BSP_TFT_DEFAULT_ROTATION / 90) & 3;
    st7796_panel_t *p = __containerof(s_panel, st7796_panel_t, base);
    return (st7796_madctl_value(p) >> 5) & 0x3;
}

uint16_t bsp_tft_width(void)
{
    if (!s_panel) return BSP_TFT_H_RES;
    st7796_panel_t *p = __containerof(s_panel, st7796_panel_t, base);
    return p->swap_xy ? BSP_TFT_V_RES : BSP_TFT_H_RES;
}

uint16_t bsp_tft_height(void)
{
    if (!s_panel) return BSP_TFT_V_RES;
    st7796_panel_t *p = __containerof(s_panel, st7796_panel_t, base);
    return p->swap_xy ? BSP_TFT_H_RES : BSP_TFT_V_RES;
}

esp_err_t bsp_tft_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init");

    uint32_t duty = (uint32_t)(percent * 255 / 100);
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "set duty");
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}
