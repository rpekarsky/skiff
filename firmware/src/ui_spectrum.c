/* SPDX-License-Identifier: GPL-3.0-only */
/* (#29, #71) the scope's spectrum: a fixed-point FFT of its snapshot (22.05 kHz), read by THEME LCD's equalizer
 * (ui_h1n.c) and drawn by HOME's SPECTRUM as a live waterfall. In the main loop at redraw, never in an ISR. Included by
 * ui_graph.c */

/* log2 x, Q16 (x > 0): the exponent, the mantissa's log2 ~ f + 0.34 f (1 - f) (within 0.004) */
static int32_t log2_q16(uint32_t x)
{
    int32_t e = 0;
    uint32_t f;
    while (x >= 2u << e && e < 31)
        e++;
    f = (e > 16 ? x >> (e - 16) : x << (16 - e)) - 65536u;
    return (e << 16) + (int32_t)f + (int32_t)(((f >> 1) * ((65536u - f) >> 1) >> 14) * 22u >> 6);
}

#define FFT_N 512u
static int16_t fft_re[FFT_N], fft_im[FFT_N];

/* in place, radix 2, halved at each of its 9 stages (the magnitudes never grow: no overflow); the twiddles from SINE */
static void fft_run(void)
{
    uint32_t i, j, k, half;
    for (i = 1, j = 0; i < FFT_N; i++) {
        uint32_t b = FFT_N >> 1;
        for (; j & b; b >>= 1)
            j ^= b;
        j |= b;
        if (i < j) {
            int16_t t = fft_re[i];
            fft_re[i] = fft_re[j];
            fft_re[j] = t;
        }
    }
    for (half = 1; half < FFT_N; half <<= 1)
        for (k = 0; k < half; k++) {
            uint32_t a = k * (512u / half);           /* (SINE: 1024 a turn; the angle 2 pi k / (2 half)) */
            int32_t c = SINE[(a + 256u) & 1023u], s = -SINE[a & 1023u];
            for (i = k; i < FFT_N; i += 2u * half) {
                int32_t tr = (fft_re[i + half] * c - fft_im[i + half] * s) >> 15;
                int32_t ti = (fft_re[i + half] * s + fft_im[i + half] * c) >> 15;
                fft_re[i + half] = (int16_t)((fft_re[i] - tr) >> 1);
                fft_im[i + half] = (int16_t)((fft_im[i] - ti) >> 1);
                fft_re[i] = (int16_t)((fft_re[i] + tr) >> 1);
                fft_im[i] = (int16_t)((fft_im[i] + ti) >> 1);
            }
        }
}

static int32_t fft_peak(void)
{
    int32_t peak = 0;
    uint32_t i;
    for (i = 0; i < FFT_N; i++) {
        int32_t v = fft_re[i] < 0 ? -fft_re[i] : fft_re[i];
        if (v > peak)
            peak = v;
    }
    return peak;
}

/* the snapshot in fft_re times gain (Q15; |sample x gain| within 16 bits), Hann-windowed, through the FFT */
static void fft_windowed(int32_t gain_q15)
{
    uint32_t i;
    for (i = 0; i < FFT_N; i++) {
        int32_t x = fft_re[i] * gain_q15 >> 15, hann = 32767 - SINE[(2u * i + 256u) & 1023u];
        fft_re[i] = (int16_t)(x * hann >> 16);
        fft_im[i] = 0;
    }
    fft_run();
}

/* (#71) HOME's SPECTRUM (HOME tapped on HOME past the scopes): every SPEC_SLICE_MS a slice, the newest FFT_N samples of
 * the scope's sound (the mix at MASTER full, or the selected track) as SPEC_COLS levels on a log frequency axis from
 * bin 1 (43 Hz) to bin 240 (10.3 kHz), 8 columns an octave: a column narrower than a bin read between the two either
 * side of it, a wider one its loudest bin. A level is 64 dB under a full-scale sine, SPEC_Q_DB steps a dB. The newest
 * slice in front, the older crawling back a slice a redraw, SPEC_SLICES of them (a second), drawn as WAVE's (ui_wf.c);
 * kept in the FM6 panel's room (ui_fm6.c fm_room_lend): after an FM6 page, or a second away, the history starts over */
#define SPEC_COLS 64u
#define SPEC_SLICES 25u
#define SPEC_SLICE_MS 40u
#define SPEC_Q_DB 4
#define SPEC_FS_L2 (26 << 16)                         /* a full-scale sine's bin power, log2 Q16: (32767 / 4)^2 */
#define SPEC_Q_PER_L2 3083                           /* levels a unit of that log2, Q24: SPEC_Q_DB x 3.01 dB */
#define SPEC_EDGE0_Q16 62746                         /* column 0's low edge in bins, Q16: 240^(-1/126) */
#define SPEC_COL_Q16 71493                           /* a column's high edge over its low one, Q16: 240^(1/63) */
#define SPEC_X0 12                                   /* the front slice from x 12, the back one's end at x 228 */
#define SPEC_X1 228
#define SPEC_STEP 2                                  /* px a slice further back moves right and up */
#define SPEC_AMP 50                                  /* a slice's 64 dB, px: the back one up to y 4 */
#define SPEC_FILL 30
static struct {
    uint32_t tick;                                    /* fm1_ms / SPEC_SLICE_MS when the newest slice was taken */
    uint8_t front;                                    /* its row in the room */
} spec __attribute__((section(".pool")));
static uint8_t *fm_room_lend(int *kept);

/* bin k's level; the snapshot was raised by shift bits */
static int32_t spec_bin(uint32_t k, int32_t shift)
{
    uint32_t power = (uint32_t)(fft_re[k] * fft_re[k]) + (uint32_t)(fft_im[k] * fft_im[k]);
    int32_t under_fs;
    if (!power)
        return 0;
    under_fs = SPEC_FS_L2 + (shift << 17) - log2_q16(power);
    return clamp(255 - (((under_fs >> 8) * SPEC_Q_PER_L2) >> 16), 0, 255);
}

/* the column from bin lo to bin hi (Q16) */
static uint8_t spec_col(uint32_t lo, uint32_t hi, int32_t shift)
{
    uint32_t k = (lo >> 16) + 1u, mid = (lo + hi) >> 1;
    int32_t loudest = 0, a, b;
    if (k << 16 > hi) {
        a = spec_bin(mid >> 16, shift);
        b = spec_bin((mid >> 16) + 1u, shift);
        return (uint8_t)(a + (((b - a) * (int32_t)(mid & 0xFFFFu)) >> 16));
    }
    for (; k << 16 <= hi; k++) {
        int32_t v = spec_bin(k, shift);
        if (v > loudest)
            loudest = v;
    }
    return (uint8_t)loudest;
}

/* the snapshot raised by whole bits towards full scale (the FFT's 16 bits kept busy), the levels taken back down */
static void spec_slice(uint8_t *col)
{
    int32_t peak, shift = 0;
    uint32_t c, lo = SPEC_EDGE0_Q16, hi;
    scope_snapshot(fft_re);
    peak = fft_peak();
    while (peak && peak << (shift + 1) <= 32767)
        shift++;
    fft_windowed(32768 << shift);
    for (c = 0; c < SPEC_COLS; c++, lo = hi) {
        hi = (uint32_t)((uint64_t)lo * SPEC_COL_Q16 >> 16);
        col[c] = spec_col(lo, hi, shift);
    }
}

/* slice j back from the newest */
static const uint8_t *spec_row(const uint8_t *room, uint32_t j)
{
    return room + (spec.front + j) % SPEC_SLICES * SPEC_COLS;
}

/* the history, a new slice in front once a tick */
static const uint8_t *spec_history(void)
{
    int kept;
    uint8_t *room = fm_room_lend(&kept);
    uint32_t tick = fm1_ms / SPEC_SLICE_MS;
    if (kept && tick == spec.tick)
        return room;
    if (!kept || tick - spec.tick > SPEC_SLICES)
        memset(room, 0, SPEC_SLICES * SPEC_COLS);
    spec.front = (uint8_t)((spec.front + SPEC_SLICES - 1u) % SPEC_SLICES);
    spec_slice(room + spec.front * SPEC_COLS);
    spec.tick = tick;
    return room;
}

static int32_t spec_floor_y(void) { return (int32_t)cv_h - 20; }   /* the front slice's: its axis under it */
static int32_t spec_width(void) { return SPEC_X1 - SPEC_X0 - ((int32_t)SPEC_SLICES - 1) * SPEC_STEP; }
static int32_t spec_y256(const void *v, uint32_t j, int32_t col, int32_t w)
{
    const uint8_t *lv = v;
    uint32_t pos = (uint32_t)col * (SPEC_COLS - 1u) * 256u / (uint32_t)w, i = pos >> 8;
    int32_t a = lv[i], b = lv[i < SPEC_COLS - 1u ? i + 1u : i], level256 = (a << 8) + (b - a) * (int32_t)(pos & 255u);
    return ((spec_floor_y() - (int32_t)j * SPEC_STEP) << 8) - level256 * SPEC_AMP / 255;
}

/* the frequencies under the front slice */
static void spec_axis(void)
{
    static const struct { const char *hz; uint8_t col; } MARK[3] = {{"100", 10}, {"1K", 36}, {"10K", 63}};
    int32_t y = spec_floor_y() + 4, k;
    for (k = 0; k < 3; k++) {
        int32_t x = SPEC_X0 + MARK[k].col * spec_width() / ((int32_t)SPEC_COLS - 1);
        cv_rect(x, spec_floor_y() + 1, 1, 2, T_MID);
        cv_text_in(x - 15, y, 30, &AF_S, MARK[k].hz, T_MID, T_SURF);
    }
}

static void graph_spectrum(uint16_t c)
{
    const uint8_t *room = spec_history();
    uint32_t j;
    for (j = SPEC_SLICES; j-- > 0;) {
        const wf_slice_t s = {spec_row(room, j), j, SPEC_X0 + (int32_t)j * SPEC_STEP, spec_width(),
                              spec_floor_y() - (int32_t)j * SPEC_STEP + 2, SPEC_FILL, spec_y256};
        wf_draw_slice(&s, ux.h1n ? (j ? T_MID : T_TEXT) : ux_mix(T_SURF, c, wf_fade(j, SPEC_SLICES)));
    }
    spec_axis();
}
