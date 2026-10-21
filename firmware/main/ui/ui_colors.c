/*
 * SPDX-License-Identifier: MIT
 *
 * Central UI palette (docs/CONVENTIONS.md §6) — ekstensi F6:
 * LUT colormap waterfall (RGB565, 32 entri), black → biru → cyan →
 * kuning → merah, interpolasi linear per segmen (rata-rata 8 entri).
 */
#include "ui/ui_colors.h"

const uint16_t ui_wf_lut[UI_WF_LUT_N] = {
    /*  0 */ 0x0000,  /* hitam                    */
    /*  1 */ 0x0004,
    /*  2 */ 0x0009,
    /*  3 */ 0x000D,
    /*  4 */ 0x0012,
    /*  5 */ 0x0016,
    /*  6 */ 0x001B,
    /*  7 */ 0x001D,  /* biru pekat               */
    /*  8 */ 0x001F,
    /*  9 */ 0x013F,
    /* 10 */ 0x025F,
    /* 11 */ 0x037F,
    /* 12 */ 0x049F,
    /* 13 */ 0x05BF,
    /* 14 */ 0x06DF,
    /* 15 */ 0x075F,  /* cyan                     */
    /* 16 */ 0x07FF,
    /* 17 */ 0x27FB,
    /* 18 */ 0x4FF6,
    /* 19 */ 0x6FF2,
    /* 20 */ 0x97ED,
    /* 21 */ 0xB7E9,
    /* 22 */ 0xDFE4,
    /* 23 */ 0xEFE2,  /* kuning                   */
    /* 24 */ 0xFFE0,
    /* 25 */ 0xFEC0,
    /* 26 */ 0xFDA0,
    /* 27 */ 0xFC80,
    /* 28 */ 0xFB60,
    /* 29 */ 0xFA40,
    /* 30 */ 0xF920,
    /* 31 */ 0xF800,  /* merah                    */
};
