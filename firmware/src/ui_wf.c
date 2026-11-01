/* SPDX-License-Identifier: GPL-3.0-only */
/* (#20w, #71) the waterfalls' slices: FM6's WAVE (ui_fm6.c) and HOME's SPECTRUM (ui_spectrum.c), drawn back to front.
 * Included by ui_graph.c */

typedef struct {
    const void *v;                                      /* its values, as y256 reads them */
    uint32_t j;                                         /* how far back: 0 the front one */
    int32_t left, w;                                    /* its columns: left + 1 .. left + w */
    int32_t foot;                                       /* its fog reaches down to here */
    int32_t fill;                                       /* % its fog veils the lines behind (0: they show through) */
    int32_t (*y256)(const void *v, uint32_t j, int32_t col, int32_t w);   /* its line at column col of w, 1/256 px */
} wf_slice_t;

/* a pixel of the line covering cover / 256 of it: the ink mixed into the panel under it, a
 * quarter at a time (mostly the panel there); (#29) the LCD looks' four tones take no mix: half covered or not at all */
static void wf_dot(int32_t x, int32_t y, int32_t cover, const uint16_t *ink)
{
    int32_t q = ux.h1n ? (cover >= 128 ? 4 : 0) : (cover * 4 + 128) >> 8;
    if (q > 0)
        cv_rect(x, y, 1, 1, ink[(q > 4 ? 4 : q) - 1]);
}

/* a slice, a pixel column at a time: the line smoothed across the rows it falls between, a steep step a solid run; the
 * front one 2 px thick. Under it the lines behind veiled into the panel */
static void wf_draw_slice(const wf_slice_t *s, uint16_t c)
{
    int32_t half = s->j ? 128 : 256, col, ya, yb;
    uint16_t ink[4];
    for (col = 0; col < 4; col++)
        ink[col] = ux_mix(T_SURF, c, 25 * (col + 1));
    for (col = 1; s->fill && col <= s->w; col++) {
        yb = s->y256(s->v, s->j, col, s->w) >> 8;
        cv_veil(s->left + col, yb + 1, s->foot, T_SURF, s->fill);
    }
    for (ya = s->y256(s->v, s->j, 0, s->w), col = 1; col <= s->w; col++, ya = yb) {
        int32_t x = s->left + col, top, f, lo, hi;
        yb = s->y256(s->v, s->j, col, s->w);
        lo = (ya < yb ? ya : yb) >> 8;
        hi = (ya < yb ? yb : ya) >> 8;
        if (hi - lo > 1)
            cv_rect(x, lo + 1, 1, hi - lo - 1, ink[3]);
        top = yb - half;
        f = top & 255;
        wf_dot(x, top >> 8, 256 - f, ink);
        if (!s->j)
            cv_rect(x, (top >> 8) + 1, 1, 1, ink[3]);
        wf_dot(x, (top >> 8) + (s->j ? 1 : 2), f, ink);
    }
}

/* slice j of n's share of its ink: 100 % in front, 30 % at the back */
static int32_t wf_fade(uint32_t j, uint32_t n) { return 100 - 70 * (int32_t)j / ((int32_t)n - 1); }
