/*
 * LVGL 9 port for the ESP32-3248S035.
 * Integration pattern ported from TPC_LCD in
 * https://github.com/ardnew/ESP32-3248S035
 */
#include "bsp_esp323248s035.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_lvgl";

#define LVGL_BUF_BYTES (BSP_TFT_V_RES * CONFIG_BSP_LVGL_BUFFER_LINES * 2)

static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;
static SemaphoreHandle_t s_mux = NULL;
static esp_timer_handle_t s_tick = NULL;
static TaskHandle_t s_task = NULL;
static void *s_buf1 = NULL;
static void *s_buf2 = NULL;

/* ------------------------------------------------------------------ */
/* tick                                                                */
/* ------------------------------------------------------------------ */

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(CONFIG_BSP_LVGL_TICK_MS);
}

/* ------------------------------------------------------------------ */
/* display                                                             */
/* ------------------------------------------------------------------ */

/* The LCD is a big-endian receiver but the ESP32 stores RGB565 pixels in
 * little-endian order, and esp_lcd sends the colour buffer byte-for-byte.
 * The working LovyanGFX / TFT_eSPI references therefore push the pixels with
 * a byte swap (pushColors(..., true)); do the same here so the panel sees the
 * high byte first. */
static void swap_bytes(uint8_t *buf, size_t len_bytes)
{
    for (size_t i = 0; i + 1 < len_bytes; i += 2) {
        uint8_t t = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = t;
    }
}

static bool lvgl_flush_done_cb(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    size_t len_bytes = (size_t)lv_area_get_width(area) * (size_t)lv_area_get_height(area) * 2;
    swap_bytes(px_map, len_bytes);

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static void lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static int32_t last_x = 0;
    static int32_t last_y = 0;

    uint16_t x, y;
    if (bsp_touch_read(&x, &y)) {
        last_x = x;
        last_y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

/* ------------------------------------------------------------------ */
/* task                                                                */
/* ------------------------------------------------------------------ */

static void lvgl_task(void *arg)
{
    (void)arg;
    uint32_t max_period = CONFIG_BSP_LVGL_TASK_PERIOD_MS;
    for (;;) {
        uint32_t delay_ms = max_period;
        if (bsp_lvgl_lock(portMAX_DELAY)) {
            uint32_t time_till_next = lv_timer_handler();
            bsp_lvgl_unlock();
            delay_ms = time_till_next;
            if (delay_ms < 1) delay_ms = 1;
            if (delay_ms > max_period) delay_ms = max_period;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t bsp_lvgl_init(void)
{
    if (s_disp) return ESP_OK;

    ESP_RETURN_ON_ERROR(bsp_tft_init(), TAG, "tft");
    ESP_RETURN_ON_ERROR(bsp_touch_init(), TAG, "touch");

    lv_init();

    s_mux = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_mux, ESP_ERR_NO_MEM, TAG, "no mem for mutex");

    s_buf1 = heap_caps_malloc(LVGL_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf2 = heap_caps_malloc(LVGL_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_buf1 && s_buf2, ESP_ERR_NO_MEM, TAG, "no mem for draw buffers");

    s_disp = lv_display_create(bsp_tft_width(), bsp_tft_height());
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "no mem for display");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, LVGL_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_disp, bsp_tft_get_panel());

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_flush_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(bsp_tft_get_io(), &cbs, s_disp),
                        TAG, "register io callbacks");

    s_indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_indev, ESP_ERR_NO_MEM, TAG, "no mem for indev");
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, lvgl_read_cb);
    lv_indev_set_display(s_indev, s_disp);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &s_tick), TAG, "tick create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick, CONFIG_BSP_LVGL_TICK_MS * 1000ULL),
                        TAG, "tick start");

    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", CONFIG_BSP_LVGL_TASK_STACK, NULL,
                                            CONFIG_BSP_LVGL_TASK_PRIORITY, &s_task,
                                            CONFIG_BSP_LVGL_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "no mem for task");

    ESP_LOGI(TAG, "LVGL %d.%d initialized (%ux%u)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, bsp_tft_width(), bsp_tft_height());
    return ESP_OK;
}

bool bsp_lvgl_lock(uint32_t timeout_ms)
{
    if (!s_mux) return false;
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_mux, ticks) == pdTRUE;
}

void bsp_lvgl_unlock(void)
{
    if (s_mux) {
        xSemaphoreGiveRecursive(s_mux);
    }
}

lv_display_t *bsp_lvgl_display(void)
{
    return s_disp;
}

lv_indev_t *bsp_lvgl_indev(void)
{
    return s_indev;
}
