/*
 * SPDX-License-Identifier: MIT
 *
 * Central UI palette (docs/CONVENTIONS.md §6): no raw color literals
 * outside this header. Colormap LUT for the waterfall joins in F6.
 */
#pragma once

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
