/* SPDX-License-Identifier: GPL-3.0-only */
/* (Skiff #29) THEME H1N's widgets (skiff-tasks/lcd-mockups.png, H1N GREY, the PO's pick): a reflective monochrome LCD,
 * the Zoom H1n's. Four tones and the dot grid are gfx.c's (lcd_tones, cv_lcd_rows); here what an LCD draws in
 * segments: the gauges, the dialog's window. On the canvas (cv_*) when ux.h1n; CRYPT's pixel faces.
 * Included by ui_graph.c. */

#define HN_SEG 4                                      /* a segment and its gap */

/* a horizontal bar of segments, lit up to ratio (0..1000; anything above 0 lights one), the rest unlit (off) */
static void hn_meter(int32_t x, int32_t y, int32_t w, int32_t h, int32_t ratio, uint16_t on, uint16_t off)
{
    int32_t n = (w + 1) / HN_SEG, lit = (n * (ratio > 1000 ? 1000 : ratio) + 500) / 1000, k;
    if (ratio > 0 && !lit)
        lit = 1;
    for (k = 0; k < n; k++)
        cv_rect(x + k * HN_SEG, y, HN_SEG - 1, h, k < lit ? on : off);
}

/* the dialog's window: a double ink frame, an LCD's printed border */
static void hn_window(int32_t w, int32_t h)
{
    cv_frame(0, 0, w, h, T_TEXT);
    cv_frame(2, 2, w - 4, h - 4, T_TEXT);
}

