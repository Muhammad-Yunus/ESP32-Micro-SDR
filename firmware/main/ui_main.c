/*
 * SPDX-License-Identifier: MIT
 *
 * UI SDR — scaffold F1: bar frekuensi atas.
 * Kontrol sentuh, waterfall dan spektrum menyusul (F2/F6).
 */
#include "ui_main.h"

#include "esp_log.h"
#include "bsp_esp323248s035.h"
#include "lvgl.h"

static const char *TAG = "ui";

/* Layout landscape 480x320 (rotasi 270). */
#define UI_W                bsp_tft_width()
#define UI_H                bsp_tft_height()

#define UI_BAR_TOP_H        44
#define UI_COLOR_BG         0x1082      /* RGB565 gelap        */
#define UI_COLOR_FG         0xFFFF      /* RGB565 putih        */
#define UI_COLOR_ACCENT     0xF800      /* RGB565 merah        */

static lv_obj_t *s_label_freq;

esp_err_t ui_init(void)
{
    if (!bsp_lvgl_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x102030), 0);

    /* ---- Bar frekuensi atas ---- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_W, UI_BAR_TOP_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x001020), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    s_label_freq = lv_label_create(bar);
    lv_label_set_text(s_label_freq, "7.030.000  [+0]  USB");
    lv_obj_set_style_text_color(s_label_freq, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_label_freq, &lv_font_montserrat_28, 0);
    lv_obj_center(s_label_freq);

    /* Placeholder center untuk area scope (F6) */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "ESP32-Micro-SDR");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x506070), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_24, 0);
    lv_obj_center(hint);

    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "UI initialized (%ux%u)", UI_W, UI_H);
    return ESP_OK;
}
