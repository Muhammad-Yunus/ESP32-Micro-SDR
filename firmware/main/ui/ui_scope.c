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
 * Drag horizontal pada waterfall mengubah nco_offset (§8).
 *
 * Skala tampilan RUNTIME (Settings modal):
 *   - span (half-width ± Hz) dari sdr_config.span_half_hz, di-clamp ke
 *     FS/2 (6 kHz @12 kHz) — lihat sdr_config_span_half_effective().
 *   - lebar bin diturunkan dari jumlah bin keluaran (1024 atau 2048).
 *   Grid vertikal & label Hz mengikuti span aktif; preset di atas FS/2
 *   dijepit dan ditampilkan sebagai nilai efektif.
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


/* Geometri area scope (480x320 landscape, top 52 / bottom 56).
 * Objek chart mentok kiri-x2=0 dan x479. Strip sumbu: kiri 30 px
 * (label dBFS) + bawah 18 px (label Hz), di dalam objek. Plot = 450×128
 * (x30..479). Waterfall di bawah plot, x30..479. */
#define SCOPE_X            30
#define SCOPE_Y            (52 + 4)
#define SCOPE_W            442
#define YLABEL_W          30
#define XLABEL_H          18
#define PLOT_PAD_R         8    /* padding grid chart vs tepi kanan LCD     */
#define WF_W               (SCOPE_W + YLABEL_W + PLOT_PAD_R) /* WF full LCD */
#define WF_CX              (YLABEL_W + SCOPE_W / 2) /* pusat frekuensi (screen x) */
#define WF_FALLBACK_ROWS   44
#define BAR_GAP            2
#define BAR_H              128
#define BAR_Y              SCOPE_Y
#define WF_Y               (BAR_Y + BAR_H + XLABEL_H + BAR_GAP)

/* Peta dBFS → tinggi/warna: atas +10 (penuh); dasar DYNAMIS =
 * p25 (persentil 25 noise floor) − 40 dB (clamp [−180, 0]) sehingga
 * seluruh titik spektrum selalu melayang 40 dB di atas dasar chart. */
#define SCOPE_DB_MAX       (10.0f)
#define SCOPE_FLOOR_ABOVE  (40.0f)   /* headroom floor riil di atas dasar */
#define SCOPE_DB_MIN_FLOOR (-200.0f) /* clamp dasar chart terdalam        */
#define SCOPE_DB_MIN_CEIL   (0.0f)   /* clamp dasar chart teratas         */

static lv_obj_t *s_bar_obj;
static lv_obj_t *s_canvas;
static lv_timer_t *s_timer;

/* Waterfall buffer (heap) + jumlah baris efektif. */
static uint8_t *s_wf_buf;
static int s_wf_rows;

/* Hasil spektrum terakhir (untuk redraw LVGL saat invalidasi).
 * Band penuh terpusat DC (index tengah = DC); jumlah bin runtime
 * 1024 atau 2048 (mode FFT dari Settings). */
static float s_db[SPECTRUM_MAX_OUT_NBINS];
static int s_nbins;
static float s_db_min = SCOPE_DB_MIN_FLOOR; /* dasar chart dinamis (state) */

/* Skala runtime: half-span efektif (± Hz) & lebar bin (Hz). */
static int32_t s_span_hz = CONFIG_SDR_DISPLAY_SPAN_HZ;
static float s_bin_hz = (float)CONFIG_SDR_AUDIO_FS_HZ /
                        (float)SPECTRUM_MAX_OUT_NBINS;

/* Line chart (F6): titik per piksel + marker puncak merah + chip freq. */
static lv_point_precise_t s_chart_pts[SCOPE_W];
static lv_point_precise_t s_marker_pts[2];
static lv_obj_t *s_peak_chip;
static lv_obj_t *s_peak_lbl;
static int s_peak_x = -1;                       /* piksel puncak (bar)     */
static float s_peak_frac = 1.0f;                /* tinggi puncak [0..1]    */

/* Drag waterfall → offset. */
static bool s_dragging;
static int32_t s_drag_off0;
static int32_t s_drag_x0;

/* ------------------------------------------------------------------ */
/* Utilitas                                                            */
/* ------------------------------------------------------------------ */

/* dB → frac [0,1] untuk tinggi chart & indeks warna waterfall.
 * Dasar = s_db_min (dinamis, dihitung di update_peak);
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

/* Half-span efektif saat ini (± Hz, sudah di-clamp ke FS/2). */
static int32_t scope_span_hz(void)
{
    return s_span_hz;
}

/* Sinkronkan span efektif dengan sdr_config (Settings modal). */
static void scope_scale_update(void)
{
    const int32_t s = sdr_config_span_half_effective();
    if (s > 0) {
        s_span_hz = s;
    }
}

/* Grid vertikal: {-S, -S/2, 0, +S/2, +S} dengan S = span efektif. */
static void scope_hz_marks(int32_t *out)
{
    const int32_t s = scope_span_hz();
    out[0] = -s;
    out[1] = -(s / 2);
    out[2] = 0;
    out[3] = s / 2;
    out[4] = s;
}

/* Rentang bin untuk kolom piksel x: zoom ke ±span efektif.
 * Lebar bin = s_bin_hz (runtime) — tiap piksel mengambil maksimum bin
 * dalam rentangnya (bukan interpolasi). Pembulatan memakai floorf agar
 * tidak ada bin yang terlewat di sekitar DC (truncasi ke nol melompati
 * bin pada sisi kiri). */
static void scope_bin_range(int x, int nbins, int *k0, int *k1)
{
    const int center = nbins / 2;
    const float half_px = (float)(SCOPE_W / 2);
    const float span = (float)scope_span_hz() / s_bin_hz;

    const float fa = (float)center +
                     (((float)x - half_px + 0.5f) * span / half_px);
    const float fb = (float)center +
                     (((float)x - half_px + 1.5f) * span / half_px);
    int a = (int)floorf(fa);
    int b = (int)floorf(fb);
    if (b <= a) {
        b = a + 1;
    }
    if (a < 0) {
        a = 0;
    }
    if (b > nbins) {
        b = nbins;
    }
    if (b <= a) {
        a = nbins - 1;                /* di luar array → bin terakhir */
        b = nbins;
    }
    *k0 = a;
    *k1 = b;
}

/* Rentang bin untuk kolom waterfall pada koordinat layar sx (0..WF_W-1).
 * Skala frekuensi identik dgn chart (pusat di WF_CX, half_px = SCOPE_W/2),
 * sehingga puncak waterfall tetap segaris dgn puncak spektrum. Kolom di
 * luar tepi plot (sx < YLABEL_W / > YLABEL_W+SCOPE_W) memakai bin FFT di
 * luar ±span — mengisi penuh lebar LCD tanpa strip warna solid. */
static void wf_bin_range(int sx, int nbins, int *k0, int *k1)
{
    const int center = nbins / 2;
    const float half_px = (float)(SCOPE_W / 2);
    const float span = (float)scope_span_hz() / s_bin_hz;

    const float fa = (float)center +
                     (((float)(sx - WF_CX) + 0.5f) * span / half_px);
    const float fb = (float)center +
                     (((float)(sx - WF_CX) + 1.5f) * span / half_px);
    int a = (int)floorf(fa);
    int b = (int)floorf(fb);
    if (b <= a) {
        b = a + 1;
    }
    if (a < 0) {
        a = 0;
    }
    if (b > nbins) {
        b = nbins;
    }
    if (b <= a) {
        a = nbins - 1;
        b = nbins;
    }
    *k0 = a;
    *k1 = b;
}

/* Nilai waterfall kolom layar sx: maksimum bin dalam rentang. */
static float wf_val(const float *db, int sx, int nbins)
{
    int k0, k1;
    wf_bin_range(sx, nbins, &k0, &k1);
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
                    (float)hz * half_px / (float)scope_span_hz();
    int32_t xi = (int32_t)lroundf(x);
    if (xi < 0) {
        xi = 0;
    } else if (xi > SCOPE_W - 1) {
        xi = SCOPE_W - 1;
    }
    return xi;
}

/* "20.00000 M" — MHz 5 desimal (resolusi 10 Hz) dengan pembulatan. */
static void fmt_mhz(char *out, size_t cap, int32_t hz)
{
    if (hz < 0) {
        hz = 0;
    }
    uint32_t mhz = (uint32_t)hz / 1000000u;
    uint32_t frac = ((uint32_t)hz % 1000000u + 5u) / 10u;
    if (frac >= 100000u) {
        frac = 0u;
        mhz += 1u;
    }
    snprintf(out, cap, "%u.%05u M", (unsigned)mhz, (unsigned)frac);
}

/* Persegi plot di dalam s_bar_obj: objek mencakup plot + strip label
 * (kiri YLABEL_W, bawah XLABEL_H) karena LVGL memotong semua gambar
 * DRAW_MAIN ke clip area objek (koordinat relatif objek = 0,0). */
typedef struct {
    int32_t x1, y1, x2, y2;   /* absolut (ruang layar) */
    int32_t h;                /* tinggi plot px */
} plot_rect_t;

static void get_plot_rect(const lv_area_t *obj, plot_rect_t *out)
{
    out->x1 = obj->x1 + YLABEL_W;
    out->y1 = obj->y1;
    out->x2 = obj->x2 - PLOT_PAD_R;
    out->y2 = obj->y1 + BAR_H - 1;
    out->h  = BAR_H;
}

/* Grid: horizontal di nilai dBFS "bersih" di dalam rentang dinamis
 * [s_db_min, SCOPE_DB_MAX], vertikal tiap 500 Hz + DC. */
static void draw_grid(lv_layer_t *layer, const plot_rect_t *pr)
{
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
    int db_marks[16];
    int n_marks = 0;
    /* Mulai dari kelipatan step pertama di atas lo, s.d. hi. */
    float first = (float)((int)(lo / step)) * step;
    if (first < lo) {
        first += step;
    }
    for (float v = first; v <= hi + 0.5f && n_marks < 16; v += step) {
        db_marks[n_marks++] = (int)lroundf(v);
    }
    /* Kalau tidak ada (range < step), gambar 3 garis: lo, tengah, hi. */
    if (n_marks == 0) {
        db_marks[0] = (int)lroundf(lo);
        db_marks[1] = (int)lroundf((lo + hi) * 0.5f);
        db_marks[2] = (int)lroundf(hi);
        n_marks = 3;
    }
    int32_t hz_marks[5];
    scope_hz_marks(hz_marks);
    lv_point_precise_t pts[2];

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = ui_col_grid();
    ld.width = 1;
    ld.opa = LV_OPA_COVER;
    ld.points = pts;
    ld.point_cnt = 2;

    for (int i = 0; i < n_marks; i++) {
        const int32_t y = pr->y2 -
            (int32_t)(db_frac((float)db_marks[i]) * (float)(pr->h - 1));
        pts[0].x = pr->x1;
        pts[0].y = y;
        pts[1].x = pr->x2;
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
        const int32_t x = pr->x1 + freq_to_x(hz_marks[i]);
        pts[0].x = x;
        pts[0].y = pr->y1;
        pts[1].x = x;
        pts[1].y = pr->y2;
        lv_draw_line(layer, &vd);
    }
}

/* Label dBFS di strip kiri + label Hz di strip bawah, keduanya di luar
 * tepi grid plot (inset 30px kiri / 18px bawah pada s_bar_obj). */
static void draw_scale_labels(lv_layer_t *layer, const plot_rect_t *pr,
                              const lv_area_t *obj)
{
    int32_t hz_marks[5];
    scope_hz_marks(hz_marks);
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
    int db_marks[16];
    int n_marks = 0;
    float first = (float)((int)(lo / step)) * step;
    if (first < lo) {
        first += step;
    }
    for (float v = first; v <= hi + 0.5f && n_marks < 16; v += step) {
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
    static char y_text[16][8];

    /* X axis = LO + nco_offset + offset grid, MHz 5 desimal (1 kHz/tick). */
    sdr_config_t cfg;
    sdr_config_get(&cfg);
    for (size_t i = 0; i < 5; i++) {
        const int32_t hz = cfg.lo_label_hz + cfg.nco_offset_hz + hz_marks[i];
        fmt_mhz(x_text[i], sizeof(x_text[i]), hz);
    }

    /* Y axis label = dBFS aktual (dasar dinamis, 0 dBFS = full-scale I/Q). */
    for (int i = 0; i < n_marks; i++) {
        snprintf(y_text[i], sizeof(y_text[i]), "%d", db_marks[i]);
    }

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(0x000000);
    bg.bg_opa = LV_OPA_0;
    bg.radius = 0;

    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.color = ui_col_grid_text();
    td.font = &lv_font_montserrat_10;
    td.align = LV_TEXT_ALIGN_RIGHT;

    /* Satuan sumbu Y di strip kiri, sejajar tepi atas plot. */
    lv_area_t unit = {
        .x1 = obj->x1,
        .y1 = obj->y1,
        .x2 = obj->x1 + YLABEL_W - 1,
        .y2 = obj->y1 + 11,
    };
    lv_draw_rect(layer, &bg, &unit);
    td.text = "dBFS";
    lv_draw_label(layer, &td, &unit);

    /* Label dBFS di strip kiri, align kanan, 3 px padding dari tepi plot.
     * 0 dBFS dilewati — strip atas direserve utk label satuan "dBFS". */
    for (int i = 0; i < n_marks; i++) {
        if (db_marks[i] == 0) {
            continue;
        }
        const int32_t y = pr->y2 -
            (int32_t)(db_frac((float)db_marks[i]) * (float)(pr->h - 1));
        lv_area_t a = {
            .x1 = obj->x1,
            .y1 = y - 6,
            .x2 = obj->x1 + YLABEL_W - 4,
            .y2 = y + 6,
        };
        lv_draw_rect(layer, &bg, &a);
        td.text = y_text[i];
        lv_draw_label(layer, &td, &a);
    }

    /* Label Hz di strip bawah.
     * - label pertama (hz=-500): align kiri  di tepi kiri plot  (x1, lebar 52 px)
     * - label terakhir (hz=+500): align kanan di tepi kanan plot (lebar 52 px)
     * - label tengah: align center di tengah tick */
    const int32_t lbl_w = 64;   /* 10 karakter "xx.xxxxx M" @Montserrat 10 */
    for (size_t i = 0; i < 5; i++) {
        int32_t x = pr->x1 + freq_to_x(hz_marks[i]);
        lv_area_t a;
        if (i == 0) {
            /* label pertama: memaksa ke tepi kiri, align kiri */
            td.align = LV_TEXT_ALIGN_LEFT;
            a.x1 = pr->x1;
            a.x2 = pr->x1 + lbl_w - 1;
        } else if (i == 4) {
            /* label terakhir: memaksa ke tepi kanan, align kanan */
            td.align = LV_TEXT_ALIGN_RIGHT;
            a.x1 = pr->x2 - lbl_w + 1;
            a.x2 = pr->x2;
        } else {
            /* label tengah: center di tick */
            td.align = LV_TEXT_ALIGN_CENTER;
            a.x1 = x - lbl_w / 2;
            a.x2 = x + lbl_w / 2;
            if (a.x1 < pr->x1) {
                a.x1 = pr->x1;
            }
            if (a.x2 > pr->x2) {
                a.x2 = pr->x2;
            }
        }
        a.y1 = pr->y2 + 1;
        a.y2 = pr->y2 + XLABEL_H;
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
    plot_rect_t pr;
    get_plot_rect(&coords, &pr);

    draw_grid(layer, &pr);

    /* Line chart kuning pucat: SATU titik per bin FFT (nilai asli, TANPA
     * interpolasi) pada posisi frekuensi binnya. Bila bin dalam tampilan
     * melebihi jumlah piksel, digambar envelope maksimum per piksel
     * (gaya SDR klasik) supaya puncak tidak hilang. */
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = ui_col_scope_line();
    dsc.width = 1;
    dsc.opa = LV_OPA_COVER;
    dsc.points = s_chart_pts;
    dsc.point_cnt = 0;

    const int center = s_nbins / 2;
    const float half_px = (float)(SCOPE_W / 2);
    const float span_bins = (float)scope_span_hz() / s_bin_hz;
    int k0 = center - (int)ceilf(span_bins) - 1;
    int k1 = center + (int)ceilf(span_bins) + 1;
    if (k0 < 0) {
        k0 = 0;
    }
    if (k1 > s_nbins - 1) {
        k1 = s_nbins - 1;
    }
    int npts = 0;
    if (k1 - k0 + 1 <= SCOPE_W) {
        /* Titik per bin asli: bin yang jatuh pada piksel sama diambil
         * yang paling tinggi (hindari zigzag saat bin > piksel). */
        int last_x = -1;
        float last_db = -1.0e9f;
        for (int k = k0; k <= k1; k++) {
            const float hz = ((float)k - (float)center) * s_bin_hz;
            const int32_t x = (int32_t)lroundf(
                half_px - 0.5f + hz * half_px / (float)scope_span_hz());
            if (x < 0 || x >= SCOPE_W) {
                continue;                           /* di luar plot ±span      */
            }
            const float f = db_frac(s_db[k]);
            if (x == last_x && npts > 0) {
                if (s_db[k] > last_db) {
                    s_chart_pts[npts - 1].y = (lv_value_precise_t)(
                        pr.y2 - (int32_t)(f * (float)(pr.h - 1)));
                    last_db = s_db[k];
                }
                continue;
            }
            s_chart_pts[npts].x = (lv_value_precise_t)(pr.x1 + x);
            s_chart_pts[npts].y =
                (lv_value_precise_t)(pr.y2 - (int32_t)(f * (float)(pr.h - 1)));
            npts++;
            last_x = x;
            last_db = s_db[k];
        }
    } else {
        /* Terlalu banyak bin untuk satu titik/piksel → envelope max. */
        for (int x = 0; x < SCOPE_W; x++) {
            int a, b;
            scope_bin_range(x, s_nbins, &a, &b);
            float v = s_db[a];
            for (int k = a + 1; k < b; k++) {
                if (s_db[k] > v) {
                    v = s_db[k];
                }
            }
            const float f = db_frac(v);
            s_chart_pts[npts].x = (lv_value_precise_t)(pr.x1 + x);
            s_chart_pts[npts].y =
                (lv_value_precise_t)(pr.y2 - (int32_t)(f * (float)(pr.h - 1)));
            npts++;
        }
    }
    dsc.point_cnt = npts;
    lv_draw_line(layer, &dsc);

    /* Marker vertikal merah di puncak (dalam plot). */
    if (s_peak_x >= 0 && s_peak_x < SCOPE_W) {
        lv_draw_line_dsc_t mdsc;
        lv_draw_line_dsc_init(&mdsc);
        mdsc.color = ui_col_peak();
        mdsc.width = 1;
        mdsc.opa = LV_OPA_COVER;
        mdsc.points = s_marker_pts;
        mdsc.point_cnt = 2;
        s_marker_pts[0].x = (lv_value_precise_t)(pr.x1 + s_peak_x);
        s_marker_pts[1].x = s_marker_pts[0].x;
        s_marker_pts[0].y = (lv_value_precise_t)(pr.y1 + 2);
        s_marker_pts[1].y = (lv_value_precise_t)pr.y2;
        lv_draw_line(layer, &mdsc);

        /* Segitiga kuning isi, jarak 10 px di atas nilai puncak,
         * mengikuti peak naik/turun. Apex mengarah ke bawah. */
        const int32_t px = pr.x1 + s_peak_x;
        int32_t py = pr.y2 - (int32_t)(s_peak_frac * (float)(pr.h - 1));
        if (py < pr.y1 + 21) {
            py = pr.y1 + 21;   /* jaga segitiga tetap di dalam plot */
        }
        lv_draw_triangle_dsc_t trid;
        lv_draw_triangle_dsc_init(&trid);
        trid.p[0].x = (lv_value_precise_t)px;             /* apex (bawah) */
        trid.p[0].y = (lv_value_precise_t)(py - 10);
        trid.p[1].x = (lv_value_precise_t)(px - 5);       /* kiri atas    */
        trid.p[1].y = (lv_value_precise_t)(py - 19);
        trid.p[2].x = (lv_value_precise_t)(px + 5);       /* kanan atas   */
        trid.p[2].y = (lv_value_precise_t)(py - 19);
        trid.color = ui_col_peak_tri();
        trid.opa = LV_OPA_COVER;
        lv_draw_triangle(layer, &trid);
    }

    draw_scale_labels(layer, &pr, &coords);
}

/* ------------------------------------------------------------------ */
/* Waterfall                                                           */
/* ------------------------------------------------------------------ */

/* Dorong satu baris (warna dari db) ke bawah; baris terbaru di atas.
 * Kanva selebar WF_W (full LCD): semua kolom berisi spektrum nyata. */
static void wf_push_row(const float *db, int nbins)
{
    if (s_wf_buf == NULL || s_nbins <= 0) {
        return;
    }
    const size_t stride = (size_t)WF_W * sizeof(uint16_t);
    uint16_t *buf = (uint16_t *)s_wf_buf;

    memmove(buf + WF_W, buf, stride * (size_t)(s_wf_rows - 1));

    /* Seluruh lebar LCD (WF_W kolom): skala frekuensi sama dgn chart,
     * puncak segaris; tepi kiri/kanan terisi bin FFT nyata (tanpa strip). */
    for (int x = 0; x < WF_W; x++) {
        const float f = db_frac(wf_val(db, x, nbins));
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
            (2.0f * (float)scope_span_hz()) / (float)SCOPE_W;
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

/* Teks chip merah: "xxx.xxxxx MHz @ -xxx dBFS" (freq dalam MHz + level). */
static void fmt_peak(char *out, size_t cap, int32_t hz, float level_db)
{
    if (hz < 0) {
        hz = 0;
    }
    const uint32_t v = (uint32_t)hz;
    const int mb = (int)(v / 1000000u);
    const int frac = (int)((v % 1000000u) * 100000u / 1000000u); /* 5 desimal */
    snprintf(out, cap, "%d.%05d MHz @ %d dBFS",
             mb, frac, (int)lroundf(level_db));
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
    /* Posisi marker = piksel puncak bin (frekuensi best_bin dipetakan ke x),
     * sejajar titik chart utk bin tersebut. */
    {
        const int center = nbins / 2;
        const float half_px = (float)(SCOPE_W / 2);
        const float span = (float)scope_span_hz() / s_bin_hz;
        float xf = half_px - 0.5f +
                   ((float)best_bin - (float)center) * half_px / span;
        int bx = (int)lroundf(xf);
        if (bx < 0) {
            bx = 0;
        } else if (bx >= SCOPE_W) {
            bx = SCOPE_W - 1;
        }
        best_x = bx;
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

    /* Dasar chart dinamis: p25 − 40 dB headroom, clamp [−180, 0]. */
    float chart_min = floor_est - SCOPE_FLOOR_ABOVE;
    if (chart_min < -180.0f) {
        chart_min = -180.0f;
    } else if (chart_min > SCOPE_DB_MIN_CEIL) {
        chart_min = SCOPE_DB_MIN_CEIL;
    }
    s_db_min = chart_min;
    s_peak_frac = db_frac(best);   /* tinggi puncak (untuk segitiga penanda) */

    /* Frekuensi absolut = LO label + offset NCO + offset bin (DC = tuned). */
    sdr_config_t cfg;
    sdr_config_get(&cfg);
    const int32_t off_hz =
        (int32_t)((float)(best_bin - nbins / 2) * s_bin_hz);
    char txt[32];
    fmt_peak(txt, sizeof(txt), cfg.lo_label_hz + cfg.nco_offset_hz + off_hz,
             best);
    lv_label_set_text(s_peak_lbl, txt);

    /* Chip di samping marker: kanan bila puncak di kiri, dan sebaliknya. */
    lv_obj_update_layout(s_peak_chip);
    const int32_t cw = lv_obj_get_width(s_peak_chip);
    int32_t cx = (best_x < SCOPE_W / 2) ? (YLABEL_W + best_x + 8)
                                        : (YLABEL_W + best_x - cw - 8);
    if (cx < YLABEL_W + 2) {
        cx = YLABEL_W + 2;
    } else if (cx + cw > SCOPE_W + YLABEL_W - 2) {
        cx = SCOPE_W + YLABEL_W - cw - 2;
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
    if (nbins > 0) {
        /* Lebar bin runtime mengikuti jumlah bin keluaran (1024/2048). */
        s_bin_hz = (float)CONFIG_SDR_AUDIO_FS_HZ / (float)nbins;
    }
    scope_scale_update();
    update_peak(db, nbins);
    wf_push_row(db, nbins);
    if (s_bar_obj != NULL) {
        lv_obj_invalidate(s_bar_obj);
    }
}

void ui_scope_refresh(void)
{
    scope_scale_update();
    /* Bersihkan waterfall: baris lama memakai skala span/FFT sebelumnya. */
    if (s_wf_buf != NULL && s_wf_rows > 0) {
        memset(s_wf_buf, 0,
               (size_t)WF_W * (size_t)s_wf_rows * sizeof(uint16_t));
        if (s_canvas != NULL) {
            lv_obj_invalidate(s_canvas);
        }
    }
    if (s_bar_obj != NULL) {
        lv_obj_invalidate(s_bar_obj);
    }
}

esp_err_t ui_scope_init(lv_obj_t *parent)
{
    lv_obj_t *scr = (parent != NULL) ? parent : lv_screen_active();

    /* Waterfall canvas (RGB565, buffer heap; fallback 48 baris).
     * Lebar WF_W = full LCD (SCOPE_W + YLABEL_W + PLOT_PAD_R): spektrum
     * di-stretch selebar layar, skala sama dgn chart → puncak segaris. */
    s_wf_rows = CONFIG_SDR_WATERFALL_ROWS;
    size_t buf_size = (size_t)WF_W * (size_t)s_wf_rows * sizeof(uint16_t);
    s_wf_buf = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_wf_buf == NULL && s_wf_rows > WF_FALLBACK_ROWS) {
        s_wf_rows = WF_FALLBACK_ROWS;
        buf_size = (size_t)WF_W * (size_t)s_wf_rows * sizeof(uint16_t);
        s_wf_buf = heap_caps_malloc(buf_size,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_wf_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_wf_buf, 0, buf_size);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_wf_buf, WF_W, s_wf_rows,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, WF_Y);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_canvas, wf_drag_cb, LV_EVENT_PRESS_LOST, NULL);

    /* Spectrum line chart: objek gelap mencakup plot + strip label sumbu
     * (kiri YLABEL_W, bawah XLABEL_H) — LVGL memotong gambar DRAW_MAIN ke
     * clip area objek, jadi label harus dalam objek. */
    s_bar_obj = lv_obj_create(scr);
    lv_obj_set_size(s_bar_obj, SCOPE_W + YLABEL_W + PLOT_PAD_R,
                    BAR_H + XLABEL_H);
    lv_obj_set_pos(s_bar_obj, SCOPE_X - YLABEL_W, BAR_Y);
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
    lv_obj_set_style_text_font(s_peak_lbl, &lv_font_montserrat_10, 0);
    lv_label_set_text(s_peak_lbl, "20.000 MHz @ 0 dBFS");
    /* Chip child s_bar_obj: x relatif obj → +YLABEL_W (offset strip kiri). */
    lv_obj_set_pos(s_peak_chip, YLABEL_W + 2, 4);

    s_timer = lv_timer_create(scope_timer_cb, 30, NULL);
    if (s_timer == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
