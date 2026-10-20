/*
 * SPDX-License-Identifier: MIT
 *
 * ui_scope — spectrum bar + waterfall F6 (FR-6, ARCHITECTURE.md §8).
 *
 * Layout area tengah (di bawah top bar 52 px, di atas bottom bar 56 px):
 *   spectrum line chart 464×136 — digambar di LV_EVENT_DRAW_MAIN (tanpa
 *   buffer penuh); marker vertikal merah di puncak + chip merah (teks
 *   hitam, radius 0) berisi frekuensi puncak.
 *   waterfall lv_canvas 464×64 (RGB565, buffer heap; fallback 48 baris)
 *
 * Timer 30 ms (Core 0): spectrum_get_db() → ui_scope_push() →
 * invalidate bar + dorong satu baris waterfall. Frame boleh drop (NFR).
 * Drag horizontal pada waterfall mengubah nco_offset_hz (±3 kHz span).
 */
#include "ui/ui_scope.h"

#include <math.h>
#include <stdio.h>
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

/* Peta dBFS → tinggi/warna: atas +10 (penuh); dasar DYNAMIS =
 * minimum spektrum − 20 dB (clamp [−200, 0]) sehingga nilai minimum
 * spektrum selalu melayang ~20 dB di atas dasar chart. */
#define SCOPE_DB_MAX       (10.0f)
#define SCOPE_FLOOR_ABOVE  (20.0f)   /* headroom min spektrum di atas dasar */
#define SCOPE_DB_MIN_FLOOR (-200.0f) /* clamp dasar chart terdalam        */
#define SCOPE_DB_MIN_CEIL   (0.0f)   /* clamp dasar chart teratas         */

static lv_obj_t *s_bar_obj;
static lv_obj_t *s_canvas;
static lv_timer_t *s_timer;

/* Waterfall buffer (heap) + jumlah baris efektif. */
static uint8_t *s_wf_buf;
static int s_wf_rows;

/* Hasil spektrum terakhir (untuk redraw LVGL saat invalidasi). */
static float s_db[CONFIG_SDR_FFT_N];
static int s_nbins;
static float s_db_min = SCOPE_DB_MIN_FLOOR; /* dasar chart dinamis (state) */

/* Line chart (F6): titik per piksel + marker puncak merah + chip freq. */
static lv_point_precise_t s_chart_pts[SCOPE_W];
static lv_point_precise_t s_marker_pts[2];
static lv_obj_t *s_peak_chip;
static lv_obj_t *s_peak_lbl;
static int s_peak_x = -1;                       /* piksel puncak (bar)     */

/* Drag waterfall → offset. */
static bool s_dragging;
static int32_t s_drag_off0;
static int32_t s_drag_x0;

/* ------------------------------------------------------------------ */
/* Utilitas                                                            */
/* ------------------------------------------------------------------ */

/* dB → frac [0,1] untuk tinggi chart / indeks warna waterfall.
 * Dasar chart = s_db_min (dinamis, dihitung di update_peak);
 * atas tetap SCOPE_DB_MAX. */
static float db_frac(float db)
{
    const float lo = s_db_min;
    const float hi = SCOPE_DB_MAX;
    const float range = hi - lo;
    if (range < 1.0f) {
        return (db >= hi) ? 1.0f : 0.0f;
    }
    float f = (db - lo) / range;
    if (f < 0.0f) {
        f = 0.0f;
    } else if (f > 1.0f) {
        f = 1.0f;
    }
    return f;
}

/* Rentang bin untuk kolom piksel x: zoom ke ±SDR_DISPLAY_SPAN_HZ —
 * ~1 bin/piksel (1024 bin → 512 bin untuk ±3 kHz di 464 px), sehingga
 * tidak ada bin yang dilewati (bukan point-sampling/interpolasi). */
static void scope_bin_range(int x, int nbins, int *k0, int *k1)
{
    const int center = nbins / 2;
    const int half_px = SCOPE_W / 2;
    const int span = (int)((float)CONFIG_SDR_DISPLAY_SPAN_HZ *
                           (float)nbins / (float)CONFIG_SDR_AUDIO_FS_HZ);

    int a = center + (int)(((float)x - (float)half_px + 0.5f) *
                           (float)span / (float)half_px);
    int b = center + (int)(((float)x - (float)half_px + 1.5f) *
                           (float)span / (float)half_px);
    if (b <= a) {
        b = a + 1;
    }
    if (a < 0) {
        a = 0;
    }
    if (b > nbins) {
        b = nbins;
    }
    if (a >= b) {
        a = b - 1;
    }
    *k0 = a;
    *k1 = b;
}

/* Nilai tampilan kolom x: maksimum bin dalam rentang (pertahankan puncak). */
static float scope_val(const float *db, int x, int nbins)
{
    int k0, k1;
    scope_bin_range(x, nbins, &k0, &k1);
    float v = db[k0];
    for (int k = k0 + 1; k < k1; k++) {
        if (db[k] > v) {
            v = db[k];
        }
    }
    return v;
}

/* Frekuensi (Hz, relatif DC) → x piksel (kebalikan scope_bin_range). */
static int32_t freq_to_x(int32_t hz)
{
    const float half_px = (float)(SCOPE_W / 2);
    const float x = half_px - 0.5f +
                    (float)hz * half_px / (float)CONFIG_SDR_DISPLAY_SPAN_HZ;
    return (int32_t)lroundf(x);
}

/* Grid: horizontal di nilai dBFS "bersih" di dalam rentang dinamis
 * [s_db_min, SCOPE_DB_MAX], vertikal tiap 500 Hz + DC. */
static void draw_grid(lv_layer_t *layer, const lv_area_t *coords)
{
    const int32_t h = coords->y2 - coords->y1 + 1;
    /* 6 garis horizontal dari s_db_min s.d. SCOPE_DB_MAX, dibulatkan
     * ke kelipatan 10 dB (nilai bersih). */
    const float lo = s_db_min;
    const float hi = SCOPE_DB_MAX;
    /* Range dinamis: cari step grid yang bagus (10/20/50 dB). */
    const float range = hi - lo;
    float step = 10.0f;
    if (range > 300.0f) {
        step = 50.0f;
    } else if (range > 150.0f) {
        step = 20.0f;
    }
    int db_marks[8];
    int n_marks = 0;
    /* Mulai dari kelipatan step pertama di atas lo, s.d. hi. */
    float first = (float)((int)(lo / step)) * step;
    if (first < lo) {
        first += step;
    }
    for (float v = first; v <= hi + 0.5f && n_marks < 8; v += step) {
        db_marks[n_marks++] = (int)lroundf(v);
    }
    /* Kalau tidak ada (range < step), gambar 3 garis: lo, tengah, hi. */
    if (n_marks == 0) {
        db_marks[0] = (int)lroundf(lo);
        db_marks[1] = (int)lroundf((lo + hi) * 0.5f);
        db_marks[2] = (int)lroundf(hi);
        n_marks = 3;
    }
    static const int32_t hz_marks[] = {-500, -250, 0, 250, 500};
    lv_point_precise_t pts[2];

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = ui_col_grid();
    ld.width = 1;
    ld.opa = LV_OPA_COVER;
    ld.points = pts;
    ld.point_cnt = 2;

    for (int i = 0; i < n_marks; i++) {
        const int32_t y = coords->y2 -
            (int32_t)(db_frac((float)db_marks[i]) * (float)(h - 1));
        pts[0].x = coords->x1;
        pts[0].y = y;
        pts[1].x = coords->x2;
        pts[1].y = y;
        lv_draw_line(layer, &ld);
    }

    for (size_t i = 0; i < sizeof(hz_marks) / sizeof(hz_marks[0]); i++) {
        lv_draw_line_dsc_t vd;
        lv_draw_line_dsc_init(&vd);
        vd.color = (hz_marks[i] == 0) ? ui_col_grid_dc() : ui_col_grid();
        vd.width = 1;
        vd.opa = LV_OPA_COVER;
        vd.points = pts;
        vd.point_cnt = 2;
        const int32_t x = freq_to_x(hz_marks[i]);
        pts[0].x = x;
        pts[0].y = coords->y1;
        pts[1].x = x;
        pts[1].y = coords->y2;
        lv_draw_line(layer, &vd);
    }
}

/* Label skala dBFS adaptif (kanan, nilai dinamis) + frekuensi absolut (bawah). */
static void draw_scale_labels(lv_layer_t *layer, const lv_area_t *coords)
{
    const int32_t h = coords->y2 - coords->y1 + 1;
    const int32_t hz_marks[] = {-500, -250, 0, 250, 500};
    /* Grid dBFS dinamis — sama persis dengan logika di draw_grid(). */
    const float lo = s_db_min;
    const float hi = SCOPE_DB_MAX;
    const float range = hi - lo;
    float step = 10.0f;
    if (range > 300.0f) {
        step = 50.0f;
    } else if (range > 150.0f) {
        step = 20.0f;
    }
    int db_marks[8];
    int n_marks = 0;
    float first = (float)((int)(lo / step)) * step;
    if (first < lo) {
        first += step;
    }
    for (float v = first; v <= hi + 0.5f && n_marks < 8; v += step) {
        db_marks[n_marks++] = (int)lroundf(v);
    }
    if (n_marks == 0) {
        db_marks[0] = (int)lroundf(lo);
        db_marks[1] = (int)lroundf((lo + hi) * 0.5f);
        db_marks[2] = (int)lroundf(hi);
        n_marks = 3;
    }
    /* Buffer statis: pointer teks dipakai draw task hingga render. */
    static char x_text[5][16];
    static char y_text[8][8];

    /* X axis = LO + nco_offset + offset grid, MHz 3 desimal (1 kHz/tick). */
    sdr_config_t cfg;
    sdr_config_get(&cfg);
    for (size_t i = 0; i < 5; i++) {
        int32_t hz = cfg.lo_label_hz + cfg.nco_offset_hz + hz_marks[i];
        if (hz < 0) {
            hz = 0;
        }
        snprintf(x_text[i], sizeof(x_text[i]), "%u.%03u M",
                 (unsigned)(hz / 1000000), (unsigned)((hz / 1000) % 1000));
    }

    /* Y axis label = dBFS aktual (dasar dinamis, 0 dBFS = full-scale I/Q). */
    for (int i = 0; i < n_marks; i++) {
        snprintf(y_text[i], sizeof(y_text[i]), "%d", db_marks[i]);
    }

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(0x000000);
    bg.bg_opa = LV_OPA_60;
    bg.radius = 0;

    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.color = ui_col_grid_text();
    td.font = &lv_font_montserrat_10;
    td.align = LV_TEXT_ALIGN_RIGHT;

    /* Satuan sumbu Y di pojok kanan atas. */
    lv_area_t unit = {
        .x1 = coords->x2 - 32,
        .y1 = coords->y1 + 1,
        .x2 = coords->x2 - 2,
        .y2 = coords->y1 + 12,
    };
    lv_draw_rect(layer, &bg, &unit);
    td.text = "dBFS";
    lv_draw_label(layer, &td, &unit);

    for (int i = 0; i < n_marks; i++) {
        const int32_t y = coords->y2 -
            (int32_t)(db_frac((float)db_marks[i]) * (float)(h - 1));
        lv_area_t a = {
            .x1 = coords->x2 - 32,
            .y1 = y - 6,
            .x2 = coords->x2 - 2,
            .y2 = y + 6,
        };
        lv_draw_rect(layer, &bg, &a);
        td.text = y_text[i];
        lv_draw_label(layer, &td, &a);
    }

    td.align = LV_TEXT_ALIGN_CENTER;
    for (size_t i = 0; i < 5; i++) {
        const int32_t x = freq_to_x(hz_marks[i]);
        /* "20.000 M" 8 karakter @Montserrat 10 ≈ 48 px; lebar ±25 px. */
        lv_area_t a = {
            .x1 = x - 25,
            .y1 = coords->y2 - 12,
            .x2 = x + 25,
            .y2 = coords->y2 - 1,
        };
        if (a.x1 < coords->x1) {
            a.x1 = coords->x1;
        }
        if (a.x2 > coords->x2) {
            a.x2 = coords->x2;
        }
        lv_draw_rect(layer, &bg, &a);
        td.text = x_text[i];
        lv_draw_label(layer, &td, &a);
    }
}

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

    draw_grid(layer, &coords);

    /* Line chart biru muda, satu draw task (polyline 464 titik). */
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = ui_col_scope_line();
    dsc.width = 1;
    dsc.opa = LV_OPA_COVER;
    dsc.points = s_chart_pts;
    dsc.point_cnt = SCOPE_W;

    for (int x = 0; x < SCOPE_W; x++) {
        const float f = db_frac(scope_val(s_db, x, s_nbins));
        s_chart_pts[x].x = (lv_value_precise_t)(coords.x1 + x);
        s_chart_pts[x].y =
            (lv_value_precise_t)(coords.y2 - (int32_t)(f * (float)(h - 1)));
    }
    lv_draw_line(layer, &dsc);

    /* Marker vertikal merah di puncak. */
    if (s_peak_x >= 0 && s_peak_x < SCOPE_W) {
        lv_draw_line_dsc_t mdsc;
        lv_draw_line_dsc_init(&mdsc);
        mdsc.color = ui_col_peak();
        mdsc.width = 1;
        mdsc.opa = LV_OPA_COVER;
        mdsc.points = s_marker_pts;
        mdsc.point_cnt = 2;
        s_marker_pts[0].x = (lv_value_precise_t)(coords.x1 + s_peak_x);
        s_marker_pts[1].x = s_marker_pts[0].x;
        s_marker_pts[0].y = (lv_value_precise_t)(coords.y1 + 2);
        s_marker_pts[1].y = (lv_value_precise_t)coords.y2;
        lv_draw_line(layer, &mdsc);
    }

    draw_scale_labels(layer, &coords);
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
        const float f = db_frac(scope_val(db, x, nbins));
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
/* Puncak spektrum: marker vertikal + chip frekuensi (F6)              */
/* ------------------------------------------------------------------ */

static void fmt_peak_hz(char *out, size_t cap, int32_t hz)
{
    if (hz < 0) {
        hz = 0;
    }
    const uint32_t v = (uint32_t)hz;
    snprintf(out, cap, "%u.%03u.%03u Hz",
             (unsigned)(v / 1000000u),
             (unsigned)((v / 1000u) % 1000u),
             (unsigned)(v % 1000u));
}

/* Cari puncak + floor robust (persentil 25), lalu posisikan marker + chip. */
static float s_px_vals[SCOPE_W];   /* nilai per piksel untuk persentil     */

static void update_peak(const float *db, int nbins)
{
    if (s_peak_chip == NULL || s_peak_lbl == NULL || nbins <= 0) {
        return;
    }

    int best_x = 0;
    int best_bin = 0;
    float best = -1000.0f;
    for (int x = 0; x < SCOPE_W; x++) {
        int k0, k1;
        scope_bin_range(x, nbins, &k0, &k1);
        float v = db[k0];
        for (int k = k0 + 1; k < k1; k++) {
            if (db[k] > v) {
                v = db[k];
            }
        }
        s_px_vals[x] = v;                  /* nilai per piksel (max bin)  */
        if (v > best) {
            best = v;
            best_x = x;
            best_bin = k0;
            for (int k = k0 + 1; k < k1; k++) {
                if (db[k] == v) {
                    best_bin = k;
                    break;
                }
            }
        }
    }
    s_peak_x = best_x;

    /* Floor robust: persentil 25 dari nilai per piksel (464 sampel).
     * Quickselect sederhana: bubble ke posisi 25% dari bawah. */
    const int n = SCOPE_W;
    const int p25_idx = n / 4;   /* ~116 */
    /* Salin + partial sort O(n·p) cukup murah tiap 30 ms di Core 0. */
    for (int i = 0; i <= p25_idx; i++) {
        int min_pos = i;
        for (int j = i + 1; j < n; j++) {
            if (s_px_vals[j] < s_px_vals[min_pos]) {
                min_pos = j;
            }
        }
        float tmp = s_px_vals[i];
        s_px_vals[i] = s_px_vals[min_pos];
        s_px_vals[min_pos] = tmp;
    }
    const float floor_est = s_px_vals[p25_idx];   /* p25 ≈ noise floor */

    /* Dasar chart dinamis: p25 − 20 dB headroom, clamp [−160, 0]. */
    float chart_min = floor_est - SCOPE_FLOOR_ABOVE;
    if (chart_min < -160.0f) {
        chart_min = -160.0f;
    } else if (chart_min > SCOPE_DB_MIN_CEIL) {
        chart_min = SCOPE_DB_MIN_CEIL;
    }
    s_db_min = chart_min;

    /* Frekuensi absolut = LO label + offset NCO + offset bin (DC = tuned). */
    sdr_config_t cfg;
    sdr_config_get(&cfg);
    const float bin_hz = (float)CONFIG_SDR_AUDIO_FS_HZ / (float)nbins;
    const int32_t off_hz =
        (int32_t)((float)(best_bin - nbins / 2) * bin_hz);
    char txt[24];
    fmt_peak_hz(txt, sizeof(txt), cfg.lo_label_hz + cfg.nco_offset_hz + off_hz);
    lv_label_set_text(s_peak_lbl, txt);

    /* Chip di samping marker: kanan bila puncak di kiri, dan sebaliknya. */
    lv_obj_update_layout(s_peak_chip);
    const int32_t cw = lv_obj_get_width(s_peak_chip);
    int32_t cx = (best_x < SCOPE_W / 2) ? (best_x + 8) : (best_x - cw - 8);
    if (cx < 2) {
        cx = 2;
    } else if (cx + cw > SCOPE_W - 2) {
        cx = SCOPE_W - cw - 2;
    }
    lv_obj_set_pos(s_peak_chip, cx, 4);
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
    update_peak(db, nbins);
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

    /* Spectrum line chart: objek gelap + gambar polyline di DRAW_MAIN. */
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

    /* Chip frekuensi puncak: merah, teks hitam, radius 0 (child bar). */
    s_peak_chip = lv_obj_create(s_bar_obj);
    lv_obj_remove_style_all(s_peak_chip);
    lv_obj_set_style_bg_color(s_peak_chip, ui_col_peak(), 0);
    lv_obj_set_style_bg_opa(s_peak_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_peak_chip, 0, 0);
    lv_obj_set_style_pad_hor(s_peak_chip, 4, 0);
    lv_obj_set_style_pad_ver(s_peak_chip, 1, 0);
    lv_obj_set_size(s_peak_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_peak_chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_peak_lbl = lv_label_create(s_peak_chip);
    lv_obj_set_style_text_color(s_peak_lbl, ui_col_peak_fg(), 0);
    lv_obj_set_style_text_font(s_peak_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_peak_lbl, "20.000.000 Hz");
    lv_obj_set_pos(s_peak_chip, 2, 4);

    s_timer = lv_timer_create(scope_timer_cb, 30, NULL);
    if (s_timer == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
