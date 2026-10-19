/*
 * SPDX-License-Identifier: MIT
 *
 * Central UI palette (docs/CONVENTIONS.md §6): no raw color literals
 * outside this header. Colormap LUT for the waterfall joins in F6.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

/* Screen & chrome */
static inline lv_color_t ui_col_bg(void)       { return lv_color_hex(0x001020); }
static inline lv_color_t ui_col_topbar(void)   { return lv_color_hex(0x001830); }
static inline lv_color_t ui_col_botbar(void)   { return lv_color_hex(0x001428); }
static inline lv_color_t ui_col_modal(void)    { return lv_color_hex(0x001C38); }

/* Buttons */
static inline lv_color_t ui_col_btn(void)      { return lv_color_hex(0x0A3A5C); }
static inline lv_color_t ui_col_btn_press(void){ return lv_color_hex(0x0E5A8C); }
static inline lv_color_t ui_col_btn_checked(void) { return lv_color_hex(0x1E7AB0); }

/* Text */
static inline lv_color_t ui_col_fg(void)       { return lv_color_hex(0xFFFFFF); }
static inline lv_color_t ui_col_dim(void)      { return lv_color_hex(0x708898); }
static inline lv_color_t ui_col_accent(void)   { return lv_color_hex(0xFF9E2C); }

/* Keypad right column: matches the theme default keyboard
 * (light grey panel, white keys, near-black labels). */
static inline lv_color_t ui_col_pad_panel(void)  { return lv_palette_lighten(LV_PALETTE_GREY, 4); }
static inline lv_color_t ui_col_pad_btn(void)    { return lv_color_white(); }
static inline lv_color_t ui_col_pad_press(void)  { return lv_palette_lighten(LV_PALETTE_GREY, 3); }
static inline lv_color_t ui_col_pad_fg(void)     { return lv_palette_darken(LV_PALETTE_GREY, 4); }

/* "7-segment" LO display window (dark glass, green digits). */
static inline lv_color_t ui_col_lcd_bg(void)     { return lv_color_hex(0x060A06); }
static inline lv_color_t ui_col_lcd_border(void) { return lv_color_hex(0x203020); }
static inline lv_color_t ui_col_lcd_fg(void)     { return lv_color_hex(0x3CFF70); }

/* Top-bar mode chip/badge. */
static inline lv_color_t ui_col_chip(void)       { return lv_color_hex(0x1E7AB0); }
static inline lv_color_t ui_col_chip_press(void) { return lv_color_hex(0x2E8FC6); }

/* Waterfall colormap (F6): LUT 32 entri RGB565 mentah (uint16_t) —
 * black → biru → cyan → kuning → merah; didefinisikan di ui_colors.c.
 * Nilai mentah dipakai langsung sebagai piksel lv_canvas RGB565. */
#define UI_WF_LUT_N        32
extern const uint16_t ui_wf_lut[UI_WF_LUT_N];

static inline uint16_t ui_wf_color(int idx)
{
    return ui_wf_lut[idx & (UI_WF_LUT_N - 1)];
}

/* Spectrum line chart + peak marker (F6). */
static inline lv_color_t ui_col_scope_line(void) { return lv_color_hex(0x66D9FF); }
static inline lv_color_t ui_col_peak(void)       { return lv_color_hex(0xFF2A2A); }
static inline lv_color_t ui_col_peak_fg(void)    { return lv_color_hex(0x000000); }
static inline lv_color_t ui_col_grid(void)       { return lv_color_hex(0x33414D); }
static inline lv_color_t ui_col_grid_dc(void)    { return lv_color_hex(0x4C6473); }
static inline lv_color_t ui_col_grid_text(void)  { return lv_color_hex(0x9FB2C0); }
/* Garis floor riil (minimum spektrum, bisa di bawah skala). */
static inline lv_color_t ui_col_floor(void)      { return lv_color_hex(0xFF8C00); }
