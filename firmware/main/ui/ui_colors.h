/*
 * SPDX-License-Identifier: MIT
 *
 * Central UI palette (docs/CONVENTIONS.md §6): no raw color literals
 * outside this header. Colormap LUT for the waterfall joins in F6.
 *
 * Theme "dark olive": anchor #8B8D08 (olive) + #E5E793 (cream).
 * Latar diturunkan lebih gelap, aksen/teks memakai cream terang.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

/* Screen & chrome */
static inline lv_color_t ui_col_bg(void)       { return lv_color_hex(0x101103); }
static inline lv_color_t ui_col_topbar(void)   { return lv_color_hex(0x1C1E06); }
static inline lv_color_t ui_col_botbar(void)   { return lv_color_hex(0x161805); }
static inline lv_color_t ui_col_modal(void)    { return lv_color_hex(0x262808); }

/* Buttons */
static inline lv_color_t ui_col_btn(void)      { return lv_color_hex(0x3A3C0B); }
static inline lv_color_t ui_col_btn_press(void){ return lv_color_hex(0x70720A); }
static inline lv_color_t ui_col_btn_checked(void) { return lv_color_hex(0x8B8D08); }

/* Text */
static inline lv_color_t ui_col_fg(void)       { return lv_color_hex(0xE5E793); }
static inline lv_color_t ui_col_dim(void)      { return lv_color_hex(0xA0A05C); }
static inline lv_color_t ui_col_accent(void)   { return lv_color_hex(0xE5E793); }

/* Keypad right column (olive panel, cream keys, dark labels). */
static inline lv_color_t ui_col_pad_panel(void)  { return lv_color_hex(0x262808); }
static inline lv_color_t ui_col_pad_btn(void)    { return lv_color_hex(0xE5E793); }
static inline lv_color_t ui_col_pad_press(void)  { return lv_color_hex(0xC9CA56); }
static inline lv_color_t ui_col_pad_fg(void)     { return lv_color_hex(0x161704); }

/* "7-segment" LO display window (dark glass, cream digits). */
static inline lv_color_t ui_col_lcd_bg(void)     { return lv_color_hex(0x0A0B02); }
static inline lv_color_t ui_col_lcd_border(void) { return lv_color_hex(0x4C4E10); }
static inline lv_color_t ui_col_lcd_fg(void)     { return lv_color_hex(0xE5E793); }

/* Top-bar mode chip/badge. */
static inline lv_color_t ui_col_chip(void)       { return lv_color_hex(0x8B8D08); }
static inline lv_color_t ui_col_chip_press(void) { return lv_color_hex(0xA7A90D); }

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
static inline lv_color_t ui_col_scope_line(void) { return lv_color_hex(0xE5E793); }
static inline lv_color_t ui_col_peak(void)       { return lv_color_hex(0xFF5A3D); }
static inline lv_color_t ui_col_peak_fg(void)    { return lv_color_hex(0x151605); }
static inline lv_color_t ui_col_grid(void)       { return lv_color_hex(0x3E4011); }
static inline lv_color_t ui_col_grid_dc(void)    { return lv_color_hex(0x5C5E1B); }
static inline lv_color_t ui_col_grid_text(void)  { return lv_color_hex(0xB7B86C); }
