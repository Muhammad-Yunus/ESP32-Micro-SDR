/*
 * SPDX-License-Identifier: MIT
 *
 * UI SDR — layout 480x320 & controls (Core 0).
 *
 * Module boundary (docs/ARCHITECTURE.md §3): this file only touches LVGL
 * and sdr_config_set_* — never ADC, DSP or bsp_audio_pcm_write().
 *
 * F2 scope: top frequency bar (FR-8), bottom control bar + numeric keypad
 * modal (FR-3 sources a/b, FR-9).
 * F6 scope: spectrum bar + waterfall via ui_scope_init() (FR-6).
 */
#include "ui/ui_main.h"

#include "bsp_esp323248s035.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "sdr_config.h"
#include "ui/ui_colors.h"
#include "ui/ui_scope.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* Layout landscape 480x320 (rotation 270). */
#define UI_W               bsp_tft_width()
#define UI_H               bsp_tft_height()

#define UI_BAR_TOP_H       52
#define UI_BAR_BOT_H       56
#define UI_BTN_H           40
#define UI_BTN_GAP         8
#define UI_PAD_X           8

/* 7-segment LO display font (main/ui/font_dseg7_36.c, DSEG7 Classic Bold). */
LV_FONT_DECLARE(font_dseg7_36)

/* Bottom bar geometry: 7 buttons, total width must fit 480.
 * Layout:  8|64|8|44|8|100|8|44|8|52|8|56|8|56|8  = 480
 * (KB→vol− gap fixed: 8, not 12 as before). */
#define UI_BTN_MODE_W      64
#define UI_BTN_TUNE_W      44   /* [-] and [+]                              */
#define UI_BTN_STEP_W       100
#define UI_BTN_KB_W        52
#define UI_BTN_VOL_W        56

typedef enum {
    ENTRY_OFFSET = 0,
    ENTRY_LO,
} entry_target_t;

#define UI_VOL_BARS        5

static lv_obj_t *s_label_freq;
static lv_obj_t *s_label_off;
static lv_obj_t *s_label_mode_btn;
static lv_obj_t *s_label_step_btn;
static lv_obj_t *s_label_vol_pct;
static lv_obj_t *s_label_vol_icon;
static lv_obj_t *s_vol_bars[UI_VOL_BARS];

/* Keypad modal (exists only while open; all pointers NULL otherwise). */
static lv_obj_t *s_modal;
static lv_obj_t *s_ta;
static lv_obj_t *s_kb;
static lv_obj_t *s_btn_entry_off;
static lv_obj_t *s_btn_entry_lo;
static entry_target_t s_entry = ENTRY_OFFSET;

/* Settings modal. */
static lv_obj_t *s_modal_settings;
static lv_obj_t *s_lbl_fft_bin_hz;
static lv_obj_t *s_lbl_span_val;
static lv_obj_t *s_lbl_gain;
static lv_obj_t *s_lbl_lpf;
static lv_obj_t *s_btn_fft_512;
static lv_obj_t *s_btn_fft_1024;
static lv_obj_t *s_btn_fft_2048;
#define UI_SPAN_BTN_MAX 8
static lv_obj_t *s_btn_span[UI_SPAN_BTN_MAX];
static int32_t s_btn_span_val[UI_SPAN_BTN_MAX];
static int s_btn_span_n;

/* Toggle-button groups in the settings modal (index <-> value mapping fixed
 * at creation; used to reflect the checked state when reopening). */
#define UI_ATTN_BTN_MAX 4
static lv_obj_t *s_btn_atten[UI_ATTN_BTN_MAX];
static uint8_t s_btn_atten_val[UI_ATTN_BTN_MAX];
#define UI_SRC_BTN_MAX 3
static lv_obj_t *s_btn_src[UI_SRC_BTN_MAX];
static uint8_t s_btn_src_val[UI_SRC_BTN_MAX];

/* Sliders (AGC-off gain only matters; dimmed when AGC is on). */
static lv_obj_t *s_sl_gain;

/* Preset frekuensi tone uji (250/1k/2k/4k) — isolasi harmonik SDM/amp. */
#define UI_TTONE_BTN_MAX 4
static lv_obj_t *s_btn_ttone[UI_TTONE_BTN_MAX];
static uint16_t s_btn_ttone_val[UI_TTONE_BTN_MAX];

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

/* "7030000" -> "7.030.000" (groups of 3 from the right). */
static void fmt_hz_dot(char *out, size_t cap, uint32_t v)
{
    char digits[10];
    int nd = 0;
    do {
        digits[nd++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && nd < (int)sizeof(digits));

    size_t o = 0;
    for (int i = nd - 1; i >= 0 && o + 1 < cap; i--) {
        out[o++] = digits[i];
        if (i > 0 && (i % 3) == 0 && o + 1 < cap) {
            out[o++] = '.';
        }
    }
    out[o] = '\0';
}

/* "[+1.200 Hz]" / "[-500 Hz]" / "[+0 Hz]" */
static void fmt_offset_bracket(char *out, size_t cap, int32_t hz)
{
    const char sign = (hz < 0) ? '-' : '+';
    uint32_t abs_hz = (hz < 0) ? (uint32_t)(-hz) : (uint32_t)hz;
    char num[16];
    fmt_hz_dot(num, sizeof(num), abs_hz);
    snprintf(out, cap, "[%c%s Hz]", sign, num);
}

/* "10 Hz" / "1 kHz" / "10 kHz" / "1 MHz" */
static void fmt_step(char *out, size_t cap, int32_t hz)
{
    if (hz >= 1000000) {
        snprintf(out, cap, "%ld MHz", (long)(hz / 1000000));
    } else if (hz >= 1000) {
        snprintf(out, cap, "%ld kHz", (long)(hz / 1000));
    } else {
        snprintf(out, cap, "%ld Hz", (long)hz);
    }
}

/* ------------------------------------------------------------------ */
/* Frequent-update helpers                                             */
/* ------------------------------------------------------------------ */

/* Speaker icon + 5 bar level + "xx%" (FR-8/FR-5 volume indicator). */
static void update_vol_indicator(uint8_t pct)
{
    /* ceil(pct / 20) but 0 stays 0 */
    const int filled = (pct == 0) ? 0 : ((int)pct - 1) / 20 + 1;
    for (int i = 0; i < UI_VOL_BARS; i++) {
        lv_obj_set_style_bg_color(s_vol_bars[i],
                                  (i < filled) ? ui_col_accent() : ui_col_dim(),
                                  0);
    }
    const char *icon = (pct == 0)  ? LV_SYMBOL_MUTE :
                       (pct >= 100) ? LV_SYMBOL_VOLUME_MAX :
                       LV_SYMBOL_VOLUME_MID;
    lv_label_set_text(s_label_vol_icon, icon);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    lv_label_set_text(s_label_vol_pct, buf);
}

static void update_freq_bar(void)
{
    sdr_config_t cfg;
    sdr_config_get(&cfg);

    char lo[16];
    char off[24];
    const char *mode = "USB";
    if (cfg.mode == SDR_MODE_LSB) {
        mode = "LSB";
    } else if (cfg.mode == SDR_MODE_AM) {
        mode = "AM";
    } else if (cfg.mode == SDR_MODE_FM) {
        mode = "FM";
    }
    fmt_hz_dot(lo, sizeof(lo), (uint32_t)cfg.lo_label_hz);
    fmt_offset_bracket(off, sizeof(off), cfg.nco_offset_hz);
    lv_label_set_text(s_label_freq, lo);
    lv_label_set_text(s_label_off, off);

    lv_label_set_text(s_label_mode_btn, mode);
    char step[16];
    fmt_step(step, sizeof(step), cfg.nco_step_hz);
    lv_label_set_text(s_label_step_btn, step);

    update_vol_indicator(cfg.volume_pct);
}

void ui_main_refresh_freq(void)
{
    update_freq_bar();
}

/* ------------------------------------------------------------------ */
/* Keypad modal                                                        */
/* ------------------------------------------------------------------ */

static int32_t parse_entry_text(const char *txt)
{
    /* Accept grouped input ("1.200" -> 1200): keep digits, allow one
     * leading sign. */
    char clean[12];
    size_t j = 0;
    for (const char *p = txt; *p != '\0' && j + 1 < sizeof(clean); p++) {
        if (*p >= '0' && *p <= '9') {
            clean[j++] = *p;
        } else if ((*p == '-' || *p == '+') && j == 0) {
            clean[j++] = *p;
        }
    }
    clean[j] = '\0';
    return (int32_t)strtol(clean, NULL, 10);
}

static void keypad_apply(void)
{
    if (s_ta == NULL) {
        return;
    }
    const int32_t v = parse_entry_text(lv_textarea_get_text(s_ta));
    if (s_entry == ENTRY_OFFSET) {
        sdr_config_set_offset(v);
    } else {
        sdr_config_set_lo_label(v);
    }
    update_freq_bar();
}

static void modal_close(void)
{
    if (s_modal != NULL) {
        lv_obj_t *m = s_modal;
        s_modal = NULL;
        s_ta = NULL;
        s_kb = NULL;
        s_btn_entry_off = NULL;
        s_btn_entry_lo = NULL;
        lv_obj_delete_async(m);
    }
}

static void settings_modal_close(void)
{
    if (s_modal_settings != NULL) {
        lv_obj_t *m = s_modal_settings;
        s_modal_settings = NULL;
        s_lbl_fft_bin_hz = NULL;
        s_lbl_span_val = NULL;
        s_lbl_gain = NULL;
        s_lbl_lpf = NULL;
        s_btn_fft_512 = NULL;
        s_btn_fft_1024 = NULL;
        s_btn_fft_2048 = NULL;
        for (int i = 0; i < s_btn_span_n; i++) {
            s_btn_span[i] = NULL;
        }
        s_btn_span_n = 0;
        for (int i = 0; i < UI_ATTN_BTN_MAX; i++) {
            s_btn_atten[i] = NULL;
        }
        for (int i = 0; i < UI_SRC_BTN_MAX; i++) {
            s_btn_src[i] = NULL;
        }
        lv_obj_delete_async(m);
    }
}

static void kb_ok_cb(lv_event_t *e)
{
    (void)e;
    keypad_apply();
    modal_close();
}

static void kb_close_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void ta_bs_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta != NULL) {
        lv_textarea_delete_char(s_ta);
    }
}

static void ta_left_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta != NULL) {
        lv_textarea_cursor_left(s_ta);
    }
}

static void ta_right_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta != NULL) {
        lv_textarea_cursor_right(s_ta);
    }
}

/* Live thousand separators ("1000" -> "1.000") while typing.
 * parse_entry_text() strips the dots again on apply. */
static void format_thousands(void)
{
    static bool busy = false;
    if (s_ta == NULL || busy) {
        return;
    }

    const char *txt = lv_textarea_get_text(s_ta);
    char digits[13];
    char out[20];
    size_t nd = 0;
    size_t o = 0;
    char sign = '\0';

    for (const char *p = txt; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            if (nd + 1 < sizeof(digits)) {
                digits[nd++] = *p;
            }
        } else if ((*p == '-' || *p == '+') && nd == 0) {
            sign = *p;
        }
    }
    digits[nd] = '\0';
    if (nd == 0) {
        return;
    }

    /* LO label: unsigned and capped at SDR_LO_MAX_HZ while typing. */
    if (s_entry == ENTRY_LO) {
        sign = '\0';
        if (strtoul(digits, NULL, 10) > (unsigned long)CONFIG_SDR_LO_MAX_HZ) {
            snprintf(digits, sizeof(digits), "%lu",
                     (unsigned long)CONFIG_SDR_LO_MAX_HZ);
            nd = strlen(digits);
        }
    }

    if (sign != '\0') {
        out[o++] = sign;
    }
    for (size_t i = 0; i < nd; i++) {
        if (i > 0 && ((nd - i) % 3u) == 0u) {
            out[o++] = '.';
        }
        out[o++] = digits[i];
    }
    out[o] = '\0';

    if (strcmp(out, txt) == 0) {
        return;
    }
    busy = true;
    lv_textarea_set_text(s_ta, out);
    lv_textarea_set_cursor_pos(s_ta, LV_TEXTAREA_CURSOR_LAST);
    busy = false;
}

static void ta_value_cb(lv_event_t *e)
{
    (void)e;
    format_thousands();
}

static void sync_entry_buttons(void)
{
    if (s_btn_entry_off == NULL) {
        return;
    }
    if (s_entry == ENTRY_OFFSET) {
        lv_obj_add_state(s_btn_entry_off, LV_STATE_CHECKED);
        lv_obj_remove_state(s_btn_entry_lo, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_btn_entry_off, LV_STATE_CHECKED);
        lv_obj_add_state(s_btn_entry_lo, LV_STATE_CHECKED);
    }
}

static void refill_textarea(void)
{
    if (s_ta == NULL) {
        return;
    }
    sdr_config_t cfg;
    sdr_config_get(&cfg);
    char buf[16];
    if (s_entry == ENTRY_OFFSET) {
        snprintf(buf, sizeof(buf), "%ld", (long)cfg.nco_offset_hz);
    } else {
        snprintf(buf, sizeof(buf), "%ld", (long)cfg.lo_label_hz);
    }
    lv_textarea_set_text(s_ta, buf);
    format_thousands();
}

static void entry_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const entry_target_t want =
        (lv_obj_get_user_data(btn) != NULL) ? ENTRY_LO : ENTRY_OFFSET;
    if (want != s_entry) {
        s_entry = want;
        sync_entry_buttons();
        refill_textarea();
    }
}

static void keypad_open(void)
{
    if (s_modal != NULL) {
        return; /* already open */
    }

    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, 400, 268);
    lv_obj_center(s_modal);
    lv_obj_set_style_bg_color(s_modal, ui_col_modal(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_flex_flow(s_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_modal, 6, 0);
    lv_obj_set_style_pad_row(s_modal, 6, 0);
    lv_obj_set_scrollable(s_modal, false);

    /* Entry target selector: NCO offset vs. LO label + inline close [X] */
    lv_obj_t *row = lv_obj_create(s_modal);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_btn_entry_off = lv_button_create(row);
    s_entry = ENTRY_OFFSET;
    lv_obj_set_user_data(s_btn_entry_off, (void *)(uintptr_t)ENTRY_OFFSET);
    lv_obj_set_size(s_btn_entry_off, LV_PCT(100), 34);
    lv_obj_set_flex_grow(s_btn_entry_off, 1);
    lv_obj_set_style_bg_color(s_btn_entry_off, ui_col_btn(), 0);
    lv_obj_set_style_bg_color(s_btn_entry_off, ui_col_btn_press(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_btn_entry_off, ui_col_btn_checked(), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(s_btn_entry_off, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_entry_off, 0, 0);
    lv_obj_set_checkable(s_btn_entry_off, true);
    lv_obj_add_event_cb(s_btn_entry_off, entry_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_btn_entry_off);
    lv_label_set_text(lbl, "NCO offset");
    lv_obj_set_style_text_color(lbl, ui_col_fg(), 0);
    lv_obj_center(lbl);

    s_btn_entry_lo = lv_button_create(row);
    lv_obj_set_user_data(s_btn_entry_lo, (void *)(uintptr_t)ENTRY_LO);
    lv_obj_set_size(s_btn_entry_lo, LV_PCT(100), 34);
    lv_obj_set_flex_grow(s_btn_entry_lo, 1);
    lv_obj_set_style_bg_color(s_btn_entry_lo, ui_col_btn(), 0);
    lv_obj_set_style_bg_color(s_btn_entry_lo, ui_col_btn_press(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_btn_entry_lo, ui_col_btn_checked(), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(s_btn_entry_lo, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_entry_lo, 0, 0);
    lv_obj_add_event_cb(s_btn_entry_lo, entry_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(s_btn_entry_lo);
    lv_label_set_text(lbl, "LO label");
    lv_obj_set_style_text_color(lbl, ui_col_fg(), 0);
    lv_obj_center(lbl);

    lv_obj_t *btn_x = lv_button_create(row);
    lv_obj_set_size(btn_x, 34, 34);
    lv_obj_set_style_bg_color(btn_x, ui_col_btn(), 0);
    lv_obj_set_style_bg_color(btn_x, ui_col_btn_press(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_x, 6, 0);
    lv_obj_set_style_border_width(btn_x, 0, 0);
    lv_obj_set_style_shadow_width(btn_x, 0, 0);
    lv_obj_add_event_cb(btn_x, kb_close_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn_x);
    lv_label_set_text(lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lbl, ui_col_fg(), 0);
    lv_obj_center(lbl);

    sync_entry_buttons();

    s_ta = lv_textarea_create(s_modal);
    lv_obj_set_width(s_ta, LV_PCT(100));
    lv_obj_set_style_radius(s_ta, 0, 0);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, 13);
    lv_textarea_set_accepted_chars(s_ta, "0123456789+-.");
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(s_ta, ta_value_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Numpad: 3-col keyboard (left) + right column with tall OK (2 rows),
     * backspace and cursor arrows — preserves original 1/5 column widths. */
    lv_obj_t *np = lv_obj_create(s_modal);
    lv_obj_remove_style_all(np);
    lv_obj_set_width(np, LV_PCT(100));
    lv_obj_set_flex_grow(np, 1);
    lv_obj_set_flex_flow(np, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(np, 0, 0);
    lv_obj_set_flex_align(np, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(np, false);

    s_kb = lv_keyboard_create(np);
    lv_obj_set_size(s_kb, 232, LV_PCT(100));
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
    static const char *const num_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "+/-", "0", ".", "",
    };
    static const lv_buttonmatrix_ctrl_t num_ctrl[] = {
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
    };
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_NUMBER, num_map, num_ctrl);
    lv_keyboard_set_textarea(s_kb, s_ta);

    lv_obj_t *rcol = lv_obj_create(np);
    lv_obj_remove_style_all(rcol);
    lv_obj_set_size(rcol, 156, LV_PCT(100));
    lv_obj_set_style_bg_color(rcol, ui_col_pad_panel(), 0);
    lv_obj_set_style_bg_opa(rcol, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rcol, 6, 0);
    lv_obj_set_style_pad_row(rcol, 6, 0);
    lv_obj_set_flex_flow(rcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rcol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(rcol, false);

    lv_obj_t *btn_ok = lv_button_create(rcol);
    lv_obj_set_width(btn_ok, LV_PCT(100));
    lv_obj_set_flex_grow(btn_ok, 2);
    lv_obj_set_style_bg_color(btn_ok, ui_col_pad_btn(), 0);
    lv_obj_set_style_bg_color(btn_ok, ui_col_pad_press(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_ok, 6, 0);
    lv_obj_set_style_border_width(btn_ok, 0, 0);
    lv_obj_set_style_shadow_width(btn_ok, 0, 0);
    lv_obj_add_event_cb(btn_ok, kb_ok_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn_ok);
    lv_label_set_text(lbl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(lbl, ui_col_pad_fg(), 0);
    lv_obj_center(lbl);

    lv_obj_t *btn_bs = lv_button_create(rcol);
    lv_obj_set_width(btn_bs, LV_PCT(100));
    lv_obj_set_flex_grow(btn_bs, 1);
    lv_obj_set_style_bg_color(btn_bs, ui_col_pad_btn(), 0);
    lv_obj_set_style_bg_color(btn_bs, ui_col_pad_press(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_bs, 6, 0);
    lv_obj_set_style_border_width(btn_bs, 0, 0);
    lv_obj_set_style_shadow_width(btn_bs, 0, 0);
    lv_obj_add_event_cb(btn_bs, ta_bs_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn_bs);
    lv_label_set_text(lbl, LV_SYMBOL_BACKSPACE);
    lv_obj_set_style_text_color(lbl, ui_col_pad_fg(), 0);
    lv_obj_center(lbl);

    lv_obj_t *ar = lv_obj_create(rcol);
    lv_obj_remove_style_all(ar);
    lv_obj_set_size(ar, LV_PCT(100), LV_PCT(25));
    lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(ar, 6, 0);
    lv_obj_set_scrollable(ar, false);

    lv_obj_t *btn_l = lv_button_create(ar);
    lv_obj_set_size(btn_l, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_grow(btn_l, 1);
    lv_obj_set_style_bg_color(btn_l, ui_col_pad_btn(), 0);
    lv_obj_set_style_bg_color(btn_l, ui_col_pad_press(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_l, 6, 0);
    lv_obj_set_style_border_width(btn_l, 0, 0);
    lv_obj_set_style_shadow_width(btn_l, 0, 0);
    lv_obj_add_event_cb(btn_l, ta_left_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn_l);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lbl, ui_col_pad_fg(), 0);
    lv_obj_center(lbl);

    lv_obj_t *btn_r = lv_button_create(ar);
    lv_obj_set_size(btn_r, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_grow(btn_r, 1);
    lv_obj_set_style_bg_color(btn_r, ui_col_pad_btn(), 0);
    lv_obj_set_style_bg_color(btn_r, ui_col_pad_press(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_r, 6, 0);
    lv_obj_set_style_border_width(btn_r, 0, 0);
    lv_obj_set_style_shadow_width(btn_r, 0, 0);
    lv_obj_add_event_cb(btn_r, ta_right_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn_r);
    lv_label_set_text(lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(lbl, ui_col_pad_fg(), 0);
    lv_obj_center(lbl);

    /* Start empty: cursor only, no pre-filled value. */
    lv_textarea_set_text(s_ta, "");
}

/* ------------------------------------------------------------------ */
/* Step picker modal (10 Hz .. 1 MHz)                                  */
/* ------------------------------------------------------------------ */

static void step_item_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    size_t count = 0;
    const int32_t *steps = sdr_config_step_options(&count);
    if (idx < count) {
        sdr_config_set_step(steps[idx]);
        update_freq_bar();
    }
    modal_close();
}

/* ------------------------------------------------------------------ */
/* Settings modal (FFT bin + LO start/stop span)                       */
/* ------------------------------------------------------------------ */

/* "11.72 Hz/bin @12 kHz" */
static void fmt_hz_per_bin(char *out, size_t cap, uint16_t bins)
{
    const int fs = CONFIG_SDR_AUDIO_FS_HZ;
    snprintf(out, cap, "%.2f Hz/bin @%d kHz",
             (float)fs / (float)bins, fs / 1000);
}

/* "start-stop: +/-500 Hz" / "start-stop: +/-6K Hz (max)" */
static void fmt_span_hz(char *out, size_t cap, int32_t eff_hz, bool clamped)
{
    const char *suffix = clamped ? " (max)" : "";
    if (eff_hz >= 1000) {
        snprintf(out, cap, "start-stop: +/-%ldK Hz%s",
                 (long)(eff_hz / 1000), suffix);
    } else {
        snprintf(out, cap, "start-stop: +/-%ld Hz%s", (long)eff_hz, suffix);
    }
}

static void fft_bin_cb(lv_event_t *e)
{
    const uint16_t bins = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    sdr_config_set_fft_bins(bins);
    if (s_btn_fft_512 != NULL && s_btn_fft_1024 != NULL &&
        s_btn_fft_2048 != NULL) {
        lv_obj_clear_state(s_btn_fft_512, LV_STATE_CHECKED);
        lv_obj_clear_state(s_btn_fft_1024, LV_STATE_CHECKED);
        lv_obj_clear_state(s_btn_fft_2048, LV_STATE_CHECKED);
        lv_obj_add_state(
            (bins == 512) ? s_btn_fft_512 :
            (bins == 1024) ? s_btn_fft_1024 : s_btn_fft_2048,
            LV_STATE_CHECKED);
    }
    if (s_lbl_fft_bin_hz != NULL) {
        char buf[32];
        fmt_hz_per_bin(buf, sizeof(buf), bins);
        lv_label_set_text(s_lbl_fft_bin_hz, buf);
    }
    ui_scope_refresh();
}

static void span_cb(lv_event_t *e)
{
    const int32_t hz = (int32_t)(uintptr_t)lv_event_get_user_data(e);
    sdr_config_set_span_half(hz);
    for (int i = 0; i < s_btn_span_n; i++) {
        if (s_btn_span[i] == NULL) {
            continue;
        }
        if (s_btn_span_val[i] == hz) {
            lv_obj_add_state(s_btn_span[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_btn_span[i], LV_STATE_CHECKED);
        }
    }
    if (s_lbl_span_val != NULL) {
        const int32_t eff = sdr_config_span_half_effective();
        char buf[40];
        fmt_span_hz(buf, sizeof(buf), eff, eff != hz);
        lv_label_set_text(s_lbl_span_val, buf);
    }
    ui_scope_refresh();
}

static void settings_close_cb(lv_event_t *e)
{
    (void)e;
    settings_modal_close();
}

static void ui_sync_group(lv_obj_t *btns[], int n, uint8_t vals[], uint8_t cur);

static void agc_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    sdr_config_set_agc_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void gain_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    const int32_t v = lv_slider_get_value(sl); /* SDR_MIN..SDR_MAX_GAIN_DB */
    sdr_config_set_manual_gain_db((int16_t)v);
    if (s_lbl_gain != NULL) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d dB", (int)v);
        lv_label_set_text(s_lbl_gain, buf);
    }
}

static void lpf_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    const int32_t v = lv_slider_get_value(sl); /* SDR_MIN..SDR_MAX_AUDIO_LPF_HZ */
    sdr_config_set_audio_lpf_hz((uint16_t)v);
    if (s_lbl_lpf != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d Hz", (int)v);
        lv_label_set_text(s_lbl_lpf, buf);
    }
}

static void testtone_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    sdr_config_set_test_tone_hz(on ? 1000 : 0);
    ESP_LOGI("ui", "test tone %d Hz %s (bypass antrean DSP)", on ? 1000 : 0,
             on ? "ON" : "OFF");
}

/* Preset frekuensi tone (250/1k/2k/4k) — pantau apakah harmonik kuat
 * mengikut 2x-f (nonlinier analog) atau tetap di satu posisi (osilasi amp). */
static void testtone_freq_cb(lv_event_t *e)
{
    const uint16_t hz = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    sdr_config_set_test_tone_hz(hz);
    for (int i = 0; i < UI_TTONE_BTN_MAX; i++) {
        if (s_btn_ttone[i] == NULL) {
            continue;
        }
        if (s_btn_ttone_val[i] == hz) {
            lv_obj_add_state(s_btn_ttone[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_btn_ttone[i], LV_STATE_CHECKED);
        }
    }
    ESP_LOGI("ui", "test tone freq -> %u Hz", (unsigned)hz);
}

static void atten_cb(lv_event_t *e)
{
    const uint8_t db = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    sdr_config_set_adc_atten_db(db);
    ui_sync_group(s_btn_atten, UI_ATTN_BTN_MAX, s_btn_atten_val, db);
}

static void src_cb(lv_event_t *e)
{
    const uint8_t src = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    sdr_config_set_src(src);
    ui_sync_group(s_btn_src, UI_SRC_BTN_MAX, s_btn_src_val, src);
}

/* Mirror sdr_config onto a group of checkable buttons (re-sync on reopen). */
static void ui_sync_group(lv_obj_t *btns[], int n, uint8_t vals[], uint8_t cur)
{
    for (int i = 0; i < n; i++) {
        if (btns[i] == NULL) {
            continue;
        }
        if (vals[i] == cur) {
            lv_obj_add_state(btns[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(btns[i], LV_STATE_CHECKED);
        }
    }
}

/* Satu baris config: label tetap + konten row. Dipakai di body settings. */
static lv_obj_t *ui_cfg_row(lv_obj_t *parent, const char *label_text,
                            uint32_t label_w)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, ui_col_fg(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl, label_w);
    return row;
}

/* Tombol checkable kecil bergaya modal (untuk group opsi: atten/src/fft). */
static lv_obj_t *ui_cfg_button(lv_obj_t *row, const char *text, uint32_t w)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, w, 28);
    lv_obj_set_style_bg_color(b, ui_col_btn(), 0);
    lv_obj_set_style_bg_color(b, ui_col_btn_press(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, ui_col_btn_checked(), LV_STATE_CHECKED);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_checkable(b, true);
    lv_obj_t *lb = lv_label_create(b);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_color(lb, ui_col_fg(), 0);
    lv_obj_center(lb);
    return b;
}

/* Slider draggable + label nilai di-kanan (gain/LPF). */
static lv_obj_t *ui_cfg_slider(lv_obj_t *row, int32_t min_v, int32_t max_v,
                               int32_t cur, lv_event_cb_t cb, uint32_t w)
{
    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_size(sl, w, 24);
    lv_slider_set_range(sl, min_v, max_v);
    lv_slider_set_value(sl, cur, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sl;
}

static void settings_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_modal_settings != NULL) {
        return;
    }

    lv_obj_t *scr = lv_screen_active();

    s_modal_settings = lv_obj_create(scr);
    lv_obj_set_size(s_modal_settings, 440, 300);
    lv_obj_center(s_modal_settings);
    lv_obj_set_style_bg_color(s_modal_settings, ui_col_modal(), 0);
    lv_obj_set_style_bg_opa(s_modal_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_modal_settings, 0, 0);
    lv_obj_set_style_pad_all(s_modal_settings, 10, 0);
    lv_obj_set_style_pad_row(s_modal_settings, 8, 0);
    lv_obj_set_flex_flow(s_modal_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s_modal_settings, false);

    /* Header: "Settings" + [X] */
    lv_obj_t *hdr = lv_obj_create(s_modal_settings);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), 28);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, ui_col_fg(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *btn_x = lv_button_create(hdr);
    lv_obj_set_size(btn_x, 28, 28);
    lv_obj_set_style_bg_color(btn_x, ui_col_btn(), 0);
    lv_obj_set_style_bg_color(btn_x, ui_col_btn_press(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_x, 4, 0);
    lv_obj_set_style_border_width(btn_x, 0, 0);
    lv_obj_set_style_shadow_width(btn_x, 0, 0);
    lv_obj_add_event_cb(btn_x, settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xlbl = lv_label_create(btn_x);
    lv_label_set_text(xlbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xlbl, ui_col_fg(), 0);
    lv_obj_center(xlbl);

    /* Body: satu baris per config; grow mengisi sisa tinggi modal, lalu
     * scrollable (FLAG_SCROLLABLE) bila isi melebihi tinggi → scrollbar AUTO. */
    lv_obj_t *body = lv_obj_create(s_modal_settings);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), 0);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    sdr_config_t s_cfg;
    sdr_config_get(&s_cfg);

    /* Baris AGC (switch on/off). */
    {
        lv_obj_t *row = ui_cfg_row(body, "AGC", 96);
        lv_obj_t *agc_sw = lv_switch_create(row);
        lv_obj_set_size(agc_sw, 44, 28);
        lv_obj_add_event_cb(agc_sw, agc_cb, LV_EVENT_VALUE_CHANGED, NULL);
        if (s_cfg.agc_enabled) {
            lv_obj_add_state(agc_sw, LV_STATE_CHECKED);
        }
    }

    /* Baris gain manual (slider -24..+54 dB, aktif saat AGC off). */
    {
        lv_obj_t *row = ui_cfg_row(body, "Gain", 96);
        s_sl_gain = ui_cfg_slider(row, SDR_MIN_GAIN_DB, SDR_MAX_GAIN_DB,
                                  (int32_t)s_cfg.manual_gain_db, gain_cb, 180);
        s_lbl_gain = lv_label_create(row);
        char gb[12];
        snprintf(gb, sizeof(gb), "%d dB", (int)s_cfg.manual_gain_db);
        lv_label_set_text(s_lbl_gain, gb);
        lv_obj_set_style_text_color(s_lbl_gain, ui_col_dim(), 0);
        lv_obj_set_style_text_font(s_lbl_gain, &lv_font_montserrat_14, 0);
        lv_obj_set_width(s_lbl_gain, 52);
        lv_obj_add_flag(s_lbl_gain, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Baris LPF audio (slider 100..6000 Hz). */
    {
        lv_obj_t *row = ui_cfg_row(body, "Audio LPF", 96);
        ui_cfg_slider(row, SDR_MIN_AUDIO_LPF_HZ, SDR_MAX_AUDIO_LPF_HZ,
                      (int32_t)s_cfg.lpf_hz, lpf_cb, 180);
        s_lbl_lpf = lv_label_create(row);
        char lb[16];
        snprintf(lb, sizeof(lb), "%d Hz", (int)s_cfg.lpf_hz);
        lv_label_set_text(s_lbl_lpf, lb);
        lv_obj_set_style_text_color(s_lbl_lpf, ui_col_dim(), 0);
        lv_obj_set_style_text_font(s_lbl_lpf, &lv_font_montserrat_14, 0);
        lv_obj_set_width(s_lbl_lpf, 52);
        lv_obj_add_flag(s_lbl_lpf, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Baris attenuasi ADC (0 / 2.5 / 6 / 12 dB). */
    {
        lv_obj_t *row = ui_cfg_row(body, "ADC atten", 96);
        const uint8_t atten_vals[] = {0, 3, 6, 11};
        const char *atten_txt[] = {"0", "2.5", "6", "12"};
        for (int a = 0; a < UI_ATTN_BTN_MAX; a++) {
            lv_obj_t *b = ui_cfg_button(row, atten_txt[a], 30);
            lv_obj_add_event_cb(b, atten_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)atten_vals[a]);
            s_btn_atten[a] = b;
            s_btn_atten_val[a] = atten_vals[a];
            if (s_cfg.adc_atten_db == atten_vals[a]) {
                lv_obj_add_state(b, LV_STATE_CHECKED);
            }
        }
    }

    /* Baris sumber I/Q (IQ / Q / I). */
    {
        lv_obj_t *row = ui_cfg_row(body, "Source", 96);
        const uint8_t src_vals[] = {SDR_SRC_IQ, SDR_SRC_Q, SDR_SRC_I};
        const char *src_txt[] = {"IQ", "Q", "I"};
        for (int s = 0; s < UI_SRC_BTN_MAX; s++) {
            lv_obj_t *b = ui_cfg_button(row, src_txt[s], 40);
            lv_obj_add_event_cb(b, src_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)src_vals[s]);
            s_btn_src[s] = b;
            s_btn_src_val[s] = src_vals[s];
            if (s_cfg.src == src_vals[s]) {
                lv_obj_add_state(b, LV_STATE_CHECKED);
            }
        }
    }

    /* Baris tone uji: "Test" ON + preset frekuensi (250/1k/2k/4k) = sine
     * murni -> SDM (bypass antrean DSP), amplitudo ikut volume.
     * OFF = kembali ke blok demod normal. */
    {
        const uint16_t tt_vals[] = {250, 1000, 2000, 4000};
        const char *tt_txt[] = {"250", "1k", "2k", "4k"};

        lv_obj_t *row = ui_cfg_row(body, "Test", 96);
        lv_obj_t *tt_sw = lv_switch_create(row);
        lv_obj_set_size(tt_sw, 44, 28);
        lv_obj_add_event_cb(tt_sw, testtone_cb, LV_EVENT_VALUE_CHANGED, NULL);
        if (s_cfg.test_tone_hz > 0) {
            lv_obj_add_state(tt_sw, LV_STATE_CHECKED);
        }

        for (int f = 0; f < UI_TTONE_BTN_MAX; f++) {
            lv_obj_t *b = ui_cfg_button(row, tt_txt[f], 30);
            lv_obj_add_event_cb(b, testtone_freq_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)tt_vals[f]);
            s_btn_ttone[f] = b;
            s_btn_ttone_val[f] = tt_vals[f];
            if (s_cfg.test_tone_hz == tt_vals[f]) {
                lv_obj_add_state(b, LV_STATE_CHECKED);
            }
        }
    }

    /* Baris FFT bin (512 / 1024 / 2048) + readout Hz/bin. */
    {
        lv_obj_t *row = ui_cfg_row(body, "FFT bin", 96);
        const uint16_t bins = sdr_config_fft_bins_get();
        s_btn_fft_512 = ui_cfg_button(row, "512", 44);
        s_btn_fft_1024 = ui_cfg_button(row, "1024", 44);
        s_btn_fft_2048 = ui_cfg_button(row, "2048", 44);
        lv_obj_add_event_cb(s_btn_fft_512, fft_bin_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)512);
        lv_obj_add_event_cb(s_btn_fft_1024, fft_bin_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)1024);
        lv_obj_add_event_cb(s_btn_fft_2048, fft_bin_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)2048);
        if (bins == 512) {
            lv_obj_add_state(s_btn_fft_512, LV_STATE_CHECKED);
        } else if (bins == 1024) {
            lv_obj_add_state(s_btn_fft_1024, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(s_btn_fft_2048, LV_STATE_CHECKED);
        }
        s_lbl_fft_bin_hz = lv_label_create(row);
        char fbh[32];
        fmt_hz_per_bin(fbh, sizeof(fbh), bins);
        lv_label_set_text(s_lbl_fft_bin_hz, fbh);
        lv_obj_set_style_text_color(s_lbl_fft_bin_hz, ui_col_dim(), 0);
        lv_obj_set_style_text_font(s_lbl_fft_bin_hz, &lv_font_montserrat_14, 0);
        lv_obj_set_width(s_lbl_fft_bin_hz, 100);
        lv_obj_add_flag(s_lbl_fft_bin_hz, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Baris LO span (250 Hz .. 10 kHz). */
    {
        const int32_t sh = sdr_config_span_half_get();
        const int32_t sh_eff = sdr_config_span_half_effective();
        lv_obj_t *row = ui_cfg_row(body, "LO span", 96);
        s_lbl_span_val = lv_label_create(row);
        char sv[40];
        fmt_span_hz(sv, sizeof(sv), sh_eff, sh_eff != sh);
        lv_label_set_text(s_lbl_span_val, sv);
        lv_obj_set_style_text_color(s_lbl_span_val, ui_col_dim(), 0);
        lv_obj_set_style_text_font(s_lbl_span_val, &lv_font_montserrat_14, 0);
        lv_obj_set_width(s_lbl_span_val, 90);
        lv_obj_add_flag(s_lbl_span_val, LV_OBJ_FLAG_CLICKABLE);

        size_t sc = 0;
        const int32_t *sopts = sdr_config_span_options(&sc);
        s_btn_span_n = 0;
        for (size_t i = 0; i < sc; i++) {
            char stxt[12];
            if (sopts[i] >= 1000) {
                snprintf(stxt, sizeof(stxt), "%ldK", (long)(sopts[i] / 1000));
            } else {
                snprintf(stxt, sizeof(stxt), "%ld", (long)sopts[i]);
            }
            lv_obj_t *b = ui_cfg_button(row, stxt, 30);
            lv_obj_add_event_cb(b, span_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)sopts[i]);
            if (sopts[i] == sh) {
                lv_obj_add_state(b, LV_STATE_CHECKED);
            }
            if (s_btn_span_n < UI_SPAN_BTN_MAX) {
                s_btn_span[s_btn_span_n] = b;
                s_btn_span_val[s_btn_span_n] = sopts[i];
                s_btn_span_n++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Step picker modal (10 Hz .. 1 MHz)                                  */
/* ------------------------------------------------------------------ */

static void step_open(void)
{
    if (s_modal != NULL) {
        return; /* already open */
    }

    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, 300, 196);
    lv_obj_center(s_modal);
    lv_obj_set_style_bg_color(s_modal, ui_col_modal(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 8, 0);
    lv_obj_set_scrollable(s_modal, false);

    lv_obj_t *title = lv_label_create(s_modal);
    lv_label_set_text(title, "Tuning step");
    lv_obj_set_style_text_color(title, ui_col_dim(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *grid = lv_obj_create(s_modal);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 284, 150);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);

    size_t count = 0;
    const int32_t *steps = sdr_config_step_options(&count);
    sdr_config_t cfg;
    sdr_config_get(&cfg);

    for (size_t i = 0; i < count; i++) {
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_set_size(b, 62, 40);
        lv_obj_set_style_bg_color(b, ui_col_btn(), 0);
        lv_obj_set_style_bg_color(b, ui_col_btn_press(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(b, ui_col_btn_checked(), LV_STATE_CHECKED);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        if (steps[i] == cfg.nco_step_hz) {
            lv_obj_add_state(b, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(b, step_item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        char txt[16];
        fmt_step(txt, sizeof(txt), steps[i]);
        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
    }
}

/* ------------------------------------------------------------------ */
/* Bottom bar buttons                                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *btn_create(lv_obj_t *parent, const char *txt, int32_t w,
                            lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, UI_BTN_H);
    lv_obj_set_style_bg_color(btn, ui_col_btn(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, ui_col_btn_press(), LV_PART_MAIN |
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, ui_col_btn_checked(), LV_PART_MAIN |
                              LV_STATE_CHECKED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, ui_col_fg(), 0);
    lv_obj_center(lbl);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

static void vol_down_cb(lv_event_t *e);
static void vol_up_cb(lv_event_t *e);

/* Speaker icon (font 24) + small +/- sign (font 14) with spacing. */
static lv_obj_t *vol_btn_create(lv_obj_t *parent, bool is_up)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BTN_VOL_W, UI_BTN_H);
    lv_obj_set_style_bg_color(btn, ui_col_btn(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, ui_col_btn_press(), LV_PART_MAIN |
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 6, 0);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(ic, ui_col_fg(), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);

    lv_obj_t *sign = lv_label_create(btn);
    lv_label_set_text(sign, is_up ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(sign, ui_col_fg(), 0);
    lv_obj_set_style_text_font(sign, &lv_font_montserrat_10, 0);

    lv_obj_add_event_cb(btn, is_up ? vol_up_cb : vol_down_cb, LV_EVENT_CLICKED,
                        NULL);
    return btn;
}

static void mode_btn_cb(lv_event_t *e)
{
    (void)e;
    sdr_config_toggle_mode();
    update_freq_bar();
}

static void tune_down_cb(lv_event_t *e)
{
    (void)e;
    sdr_config_t cfg;
    sdr_config_get(&cfg);
    sdr_config_set_offset(cfg.nco_offset_hz - cfg.nco_step_hz);
    update_freq_bar();
}

static void tune_up_cb(lv_event_t *e)
{
    (void)e;
    sdr_config_t cfg;
    sdr_config_get(&cfg);
    sdr_config_set_offset(cfg.nco_offset_hz + cfg.nco_step_hz);
    update_freq_bar();
}

/* Triangle icon (14 px wide) drawn in tune buttons: right = +, left = -. */
static void tri_draw_cb(lv_event_t *e)
{
    const bool point_right = (bool)(uintptr_t)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *btn = lv_event_get_target(e);

    lv_area_t btn_coords;
    lv_obj_get_coords(btn, &btn_coords);

    const int32_t w = 14;
    const int32_t h = 16;
    const int32_t cx = btn_coords.x1 + (btn_coords.x2 - btn_coords.x1 + 1) / 2;
    const int32_t cy = btn_coords.y1 + (btn_coords.y2 - btn_coords.y1 + 1) / 2;

    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = ui_col_fg();
    tri.opa = LV_OPA_COVER;
    if (point_right) {
        tri.p[0] = (lv_point_precise_t){ cx - w / 2, cy - h / 2 };
        tri.p[1] = (lv_point_precise_t){ cx - w / 2, cy + h / 2 };
        tri.p[2] = (lv_point_precise_t){ cx + w / 2, cy };
    } else {
        tri.p[0] = (lv_point_precise_t){ cx + w / 2, cy - h / 2 };
        tri.p[1] = (lv_point_precise_t){ cx + w / 2, cy + h / 2 };
        tri.p[2] = (lv_point_precise_t){ cx - w / 2, cy };
    }
    lv_draw_triangle(layer, &tri);
}

static lv_obj_t *tri_btn_create(lv_obj_t *parent, bool point_right,
                                lv_event_cb_t click_cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BTN_TUNE_W, UI_BTN_H);
    lv_obj_set_style_bg_color(btn, ui_col_btn(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, ui_col_btn_press(), LV_PART_MAIN |
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, tri_draw_cb, LV_EVENT_DRAW_MAIN,
                        (void *)(uintptr_t)point_right);
    lv_obj_add_event_cb(btn, click_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void step_btn_cb(lv_event_t *e)
{
    (void)e;
    step_open();
}

static void keypad_btn_cb(lv_event_t *e)
{
    (void)e;
    keypad_open();
}

static void vol_down_cb(lv_event_t *e)
{
    (void)e;
    sdr_config_add_volume(-5);
    update_freq_bar();
}

static void vol_up_cb(lv_event_t *e)
{
    (void)e;
    sdr_config_add_volume(+5);
    update_freq_bar();
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

esp_err_t ui_init(void)
{
    if (!bsp_lvgl_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, ui_col_bg(), 0);

    /* ---- Top bar: LO display + NCO offset + mode chip + volume ---- */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, UI_W, UI_BAR_TOP_H);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, ui_col_topbar(), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 10, 0);
    lv_obj_set_style_pad_hor(top, UI_PAD_X, 0);

    /* LO frequency in a dark "LCD" window with 7-segment digits. */
    lv_obj_t *lcd = lv_obj_create(top);
    lv_obj_remove_style_all(lcd);
    lv_obj_set_size(lcd, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(lcd, ui_col_lcd_bg(), 0);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lcd, 1, 0);
    lv_obj_set_style_border_color(lcd, ui_col_lcd_border(), 0);
    lv_obj_set_style_radius(lcd, 4, 0);
    lv_obj_set_style_pad_hor(lcd, 8, 0);
    lv_obj_set_style_pad_ver(lcd, 2, 0);

    s_label_freq = lv_label_create(lcd);
    lv_obj_set_style_text_color(s_label_freq, ui_col_lcd_fg(), 0);
    lv_obj_set_style_text_font(s_label_freq, &font_dseg7_36, 0);
    lv_obj_set_style_text_letter_space(s_label_freq, 1, 0);

    /* NCO offset: "[+1.500 Hz]" */
    s_label_off = lv_label_create(top);
    lv_obj_set_style_text_color(s_label_off, ui_col_fg(), 0);
    lv_obj_set_style_text_font(s_label_off, &lv_font_montserrat_16, 0);

    /* Push the volume group to the right edge. */
    lv_obj_t *spacer = lv_obj_create(top);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    /* Volume indicator: speaker icon + 5 level bars + "xx%" (right side).
     * Flex row keeps spacing tight & consistent regardless of text width. */
    lv_obj_t *vol_row = lv_obj_create(top);
    lv_obj_remove_style_all(vol_row);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_row, 4, 0);
    lv_obj_set_size(vol_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    s_label_vol_icon = lv_label_create(vol_row);
    lv_obj_set_style_text_color(s_label_vol_icon, ui_col_fg(), 0);
    lv_obj_set_style_text_font(s_label_vol_icon, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_label_vol_icon, LV_SYMBOL_VOLUME_MID);

    const int32_t bar_w = 4;
    const int32_t bar_gap = 1;
    for (int i = 0; i < UI_VOL_BARS; i++) {
        s_vol_bars[i] = lv_obj_create(vol_row);
        lv_obj_remove_style_all(s_vol_bars[i]);
        lv_obj_set_size(s_vol_bars[i], bar_w, 14);
        lv_obj_set_style_radius(s_vol_bars[i], 1, 0);
        lv_obj_set_style_bg_color(s_vol_bars[i], ui_col_dim(), 0);
        lv_obj_set_style_bg_opa(s_vol_bars[i], LV_OPA_COVER, 0);
        if (i > 0) {
            lv_obj_set_style_margin_left(s_vol_bars[i], bar_gap -
                                         4 /* pad_column already 4 */, 0);
        }
    }

    s_label_vol_pct = lv_label_create(vol_row);
    lv_obj_set_style_text_color(s_label_vol_pct, ui_col_fg(), 0);
    lv_obj_set_style_text_font(s_label_vol_pct, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_label_vol_pct, "0%");

    /* Settings gear button (top bar kanan). */
    lv_obj_t *btn_settings = lv_button_create(top);
    lv_obj_set_size(btn_settings, 36, 36);
    lv_obj_set_style_bg_color(btn_settings, ui_col_btn(), 0);
    lv_obj_set_style_bg_color(btn_settings, ui_col_btn_press(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_settings, 6, 0);
    lv_obj_set_style_border_width(btn_settings, 0, 0);
    lv_obj_set_style_shadow_width(btn_settings, 0, 0);
    lv_obj_add_event_cb(btn_settings, settings_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_gear = lv_label_create(btn_settings);
    lv_label_set_text(lbl_gear, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(lbl_gear, ui_col_fg(), 0);
    lv_obj_set_style_text_font(lbl_gear, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_gear);

    /* ---- F6: spectrum bar + waterfall (FR-6) ---- */
    if (ui_scope_init(scr) != ESP_OK) {
        ESP_LOGW("ui", "ui_scope_init gagal — spectrum/waterfall tidak tampil");
    }

    /* ---- Bottom control bar (FR-9) ---- */
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_remove_style_all(bot);
    lv_obj_set_size(bot, UI_W, UI_BAR_BOT_H);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bot, ui_col_botbar(), 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);

    const int32_t y = (UI_BAR_BOT_H - UI_BTN_H) / 2;
    int32_t x = UI_PAD_X;

    lv_obj_t *btn = btn_create(bot, "USB", UI_BTN_MODE_W, mode_btn_cb);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    s_label_mode_btn = lv_obj_get_child(btn, 0);
    x += UI_BTN_MODE_W + UI_BTN_GAP;

    btn = tri_btn_create(bot, false, tune_down_cb);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    x += UI_BTN_TUNE_W + UI_BTN_GAP;

    btn = lv_button_create(bot);
    lv_obj_set_size(btn, UI_BTN_STEP_W, UI_BTN_H);
    lv_obj_set_style_bg_color(btn, ui_col_btn(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, ui_col_btn_press(), LV_PART_MAIN |
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 4, 0);
    lv_obj_set_style_pad_left(btn, 10, 0);
    lv_obj_set_style_pad_right(btn, 10, 0);
    {
        /* |-| icon: vertical — horizontal — vertical (max 18 px wide). */
        lv_obj_t *ic = lv_obj_create(btn);
        lv_obj_remove_style_all(ic);
        lv_obj_set_size(ic, 18, 14);
        const struct {
            int32_t x, y, w, h;
        } segs[] = {
            {  0, 0,  2, 14 },  /* tegak kiri   */
            {  2, 6, 14,  2 },  /* lurus tengah */
            { 16, 0,  2, 14 },  /* tegak kanan  */
        };
        for (int i = 0; i < 3; i++) {
            lv_obj_t *ln = lv_obj_create(ic);
            lv_obj_remove_style_all(ln);
            lv_obj_set_size(ln, segs[i].w, segs[i].h);
            lv_obj_set_style_bg_color(ln, ui_col_fg(), 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
            lv_obj_align(ln, LV_ALIGN_TOP_LEFT, segs[i].x, segs[i].y);
        }
    }
    s_label_step_btn = lv_label_create(btn);
    lv_label_set_text(s_label_step_btn, "100 Hz");
    lv_obj_set_style_text_color(s_label_step_btn, ui_col_fg(), 0);
    lv_obj_add_event_cb(btn, step_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    x += UI_BTN_STEP_W + UI_BTN_GAP;

    btn = tri_btn_create(bot, true, tune_up_cb);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    x += UI_BTN_TUNE_W + UI_BTN_GAP;

    btn = btn_create(bot, LV_SYMBOL_KEYBOARD, UI_BTN_KB_W, keypad_btn_cb);
    lv_obj_set_style_text_font(lv_obj_get_child(btn, 0),
                               &lv_font_montserrat_16, 0);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    x += UI_BTN_KB_W + UI_BTN_GAP;

    btn = vol_btn_create(bot, false);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    x += UI_BTN_VOL_W + UI_BTN_GAP;

    btn = vol_btn_create(bot, true);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);

    update_freq_bar();

    bsp_lvgl_unlock();
    return ESP_OK;
}
