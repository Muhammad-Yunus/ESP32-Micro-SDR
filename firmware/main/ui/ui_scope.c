/*
 * SPDX-License-Identifier: MIT
 *
 * ui_scope — spectrum bar + waterfall F6 (FR-6, ARCHITECTURE.md §8).
 *
 * Layout area tengah (di bawah top bar 52 px, di atas bottom bar 56 px):
 *   spectrum bar 464×136 — digambar di LV_EVENT_DRAW_MAIN, tanpa buffer
 *   penuh (hanya 232 kolom @2 px).
 *   waterfall lv_canvas 464×64 (RGB565, buffer heap; fallback 48 baris)
 *
 * Timer 30 ms (Core 0): spectrum_get_db() → ui_scope_push() →
 * invalidate bar + dorong satu baris waterfall. Frame boleh drop (NFR).
 * Drag horizontal pada waterfall mengubah nco_offset_hz (±3 kHz span).
 */
#include "ui/ui_scope.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "sdr_config.h"
#include "spectrum.h"
#include "ui/ui_colors.h"
#include "ui/ui_main.h"

/* Geometri area scope (480x320 landscape, top 52 / bottom 56). */
#define SCOPE_X            8
#define SCOPE_Y            (52 + 6)
#define SCOPE_W            464
#define WF_H               64
#define WF_FALLBACK_ROWS   48
#define BAR_GAP            4
#define BAR_H              136
#define BAR_Y              SCOPE_Y
#define WF_Y               (BAR_Y + BAR_H + BAR_GAP)

#define SCOPE_BAR_W        2                    /* lebar kolom spektrum    */
#define SCOPE_NBAR         (SCOPE_W / SCOPE_BAR_W)

/* Pemetaan dB → tinggi/warna: 0 dB = penuh, −90 dB = dasar. */
#define SCOPE_DB_MIN       (-90.0f)

static lv_obj_t *s_bar_obj;
static lv_obj_t *s_canvas;
static lv_timer_t *s_timer;

/* Waterfall buffer (heap) + jumlah baris efektif. */
static uint8_t *s_wf_buf;
static int s_wf_rows;

/* Hasil spektrum terakhir (untuk redraw LVGL saat invalidasi). */
static float s_db[CONFIG_SDR_FFT_N];
static int s_nbins;

/* Drag waterfall → offset. */
static bool s_dragging;
static int32_t s_drag_off0;
static int32_t s_drag_x0;

/* ------------------------------------------------------------------ */
/* Utilitas                                                            */
/* ------------------------------------------------------------------ */

/* dB → frac [0,1] untuk tinggi bar / indeks warna waterfall. */
static float db_frac(float db)
{
    float f = (db - SCOPE_DB_MIN) / (0.0f - SCOPE_DB_MIN);
    if (f < 0.0f) {
        f = 0.0f;
    } else if (f > 1.0f) {
        f = 1.0f;
    }
    return f;
}

/* Pilih bin untuk kolom piksel x (0..SCOPE_W-1) di sekitar DC. */
static int bar_bin(int x, int nbins)
{
    const int center = nbins / 2;
    const int half_px = SCOPE_W / 2;
    int off = (x - half_px + 1) * (nbins / 2) / half_px;
    int k = center + off;
    if (k < 0) {
        k = 0;
    } else if (k >= nbins) {
        k = nbins - 1;
    }
    return k;
}

/* ------------------------------------------------------------------ */
/* Spectrum bar (LV_EVENT_DRAW_MAIN, tanpa buffer penuh)               */
/* ------------------------------------------------------------------ */

static void bar_draw_cb(lv_event_t *e)
{
    if (s_nbins <= 0) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const int32_t h = coords.y2 - coords.y1 + 1;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = ui_col_accent();
    dsc.bg_opa = LV_OPA_COVER;

    for (int b = 0; b < SCOPE_NBAR; b++) {
        const int x0 = b * SCOPE_BAR_W;
        /* Ambil bin maksimum dalam lebar kolom (resolusi penuh ±1 bin). */
        float db = SCOPE_DB_MIN;
        for (int dx = 0; dx < SCOPE_BAR_W; dx++) {
            const float v = s_db[bar_bin(x0 + dx, s_nbins)];
            if (v > db) {
                db = v;
            }
        }
        int32_t bh = (int32_t)(db_frac(db) * (float)h);
        if (bh < 1) {
            bh = 1;                             /* garis dasar 1 px        */
        }
        lv_area_t rect = {
            .x1 = coords.x1 + x0,
            .y1 = coords.y2 - bh + 1,
            .x2 = coords.x1 + x0 + SCOPE_BAR_W - 1,
            .y2 = coords.y2,
        };
        lv_draw_rect(layer, &dsc, &rect);
    }
}

/* ------------------------------------------------------------------ */
/* Waterfall                                                           */
/* ------------------------------------------------------------------ */

/* Dorong satu baris (warna dari db) ke bawah; baris terbaru di atas. */
static void wf_push_row(const float *db, int nbins)
{
    if (s_wf_buf == NULL || s_nbins <= 0) {
        return;
    }
    const size_t stride = (size_t)SCOPE_W * sizeof(uint16_t);
    uint16_t *buf = (uint16_t *)s_wf_buf;

    memmove(buf + SCOPE_W, buf, stride * (size_t)(s_wf_rows - 1));

    for (int x = 0; x < SCOPE_W; x++) {
        const float f = db_frac(db[bar_bin(x, nbins)]);
        buf[x] = ui_wf_color((int)(f * (float)(UI_WF_LUT_N - 1)));
    }
    lv_obj_invalidate(s_canvas);
}

/* ------------------------------------------------------------------ */
/* Drag waterfall → NCO offset (ARCHITECTURE.md §8)                    */
/* ------------------------------------------------------------------ */

static void wf_drag_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    const int32_t x = pt.x;

    if (code == LV_EVENT_PRESSED) {
        sdr_config_t cfg;
        sdr_config_get(&cfg);
        s_dragging = true;
        s_drag_off0 = cfg.nco_offset_hz;
        s_drag_x0 = x;
    } else if (code == LV_EVENT_PRESSING && s_dragging) {
        const float hz_per_px =
            (2.0f * (float)CONFIG_SDR_DISPLAY_SPAN_HZ) / (float)SCOPE_W;
        /* Geser kanan → offset turun (spektrum bergeser kanan). */
        const int32_t delta =
            (int32_t)((float)(s_drag_x0 - x) * hz_per_px);
        sdr_config_set_offset(s_drag_off0 + delta);
        ui_main_refresh_freq();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_dragging = false;
    }
}

/* ------------------------------------------------------------------ */
/* Timer 30 ms: drain spectrum_get_db → push                          */
/* ------------------------------------------------------------------ */

static void scope_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int n = 0;
    if (!spectrum_get_db(s_db, &n)) {
        return;                                 /* frame boleh drop (NFR)  */
    }
    s_nbins = n;
    ui_scope_push(s_db, n);
}

/* ------------------------------------------------------------------ */
/* API publik                                                          */
/* ------------------------------------------------------------------ */

void ui_scope_push(const float *db, int nbins)
{
    wf_push_row(db, nbins);
    if (s_bar_obj != NULL) {
        lv_obj_invalidate(s_bar_obj);
    }
}

esp_err_t ui_scope_init(lv_obj_t *parent)
{
    lv_obj_t *scr = (parent != NULL) ? parent : lv_screen_active();

    /* Waterfall canvas (RGB565, buffer heap; fallback 48 baris). */
    s_wf_rows = CONFIG_SDR_WATERFALL_ROWS;
    size_t buf_size = (size_t)SCOPE_W * (size_t)s_wf_rows * sizeof(uint16_t);
    s_wf_buf = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_wf_buf == NULL && s_wf_rows > WF_FALLBACK_ROWS) {
        s_wf_rows = WF_FALLBACK_ROWS;
        buf_size = (size_t)SCOPE_W * (size_t)s_wf_rows * sizeof(uint16_t);
        s_wf_buf = heap_caps_malloc(buf_size,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_wf_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_wf_buf, 0, buf_size);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_wf_buf, SCOPE_W, s_wf_rows,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, SCOPE_X, WF_Y);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_PRESS_LOST, NULL);

    /* Spectrum bar: objek gelap + gambar kolom di DRAW_MAIN. */
    s_bar_obj = lv_obj_create(scr);
    lv_obj_set_size(s_bar_obj, SCOPE_W, BAR_H);
    lv_obj_set_pos(s_bar_obj, SCOPE_X, BAR_Y);
    lv_obj_set_style_bg_color(s_bar_obj, ui_col_topbar(), 0);
    lv_obj_set_style_bg_opa(s_bar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar_obj, 0, 0);
    lv_obj_set_style_radius(s_bar_obj, 0, 0);
    lv_obj_set_style_pad_all(s_bar_obj, 0, 0);
    lv_obj_set_scrollable(s_bar_obj, false);
    lv_obj_clear_flag(s_bar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_bar_obj, bar_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_timer = lv_timer_create(scope_timer_cb, 30, NULL);
    if (s_timer == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
