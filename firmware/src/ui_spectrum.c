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

/* n points (up to 1024: SINE's turn) in place, radix 2, halved at each stage (the magnitudes never grow: no overflow);
 * the twiddles from SINE */
static void fft_run(int16_t *re, int16_t *im, uint32_t n)
{
    uint32_t i, j, k, half;
    for (i = 1, j = 0; i < n; i++) {
        uint32_t b = n >> 1;
        for (; j & b; b >>= 1)
            j ^= b;
        j |= b;
        if (i < j) {
            int16_t t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (half = 1; half < n; half <<= 1)
        for (k = 0; k < half; k++) {
            uint32_t a = k * (512u / half);           /* (SINE: 1024 a turn; the angle 2 pi k / (2 half)) */
            int32_t c = SINE[(a + 256u) & 1023u], s = -SINE[a & 1023u];
            for (i = k; i < n; i += 2u * half) {
                int32_t tr = (re[i + half] * c - im[i + half] * s) >> 15;
                int32_t ti = (re[i + half] * s + im[i + half] * c) >> 15;
                re[i + half] = (int16_t)((re[i] - tr) >> 1);
                im[i + half] = (int16_t)((im[i] - ti) >> 1);
                re[i] = (int16_t)((re[i] + tr) >> 1);
                im[i] = (int16_t)((im[i] + ti) >> 1);
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
    fft_run(fft_re, fft_im, FFT_N);
}

/* (#71) the SPECTRUM waterfall: HOME's (HOME tapped on HOME past the scopes) and (#71b) the FM6 sheet's panel (ui_fm6.c).
 * Every SPEC_SLICE_MS a slice, the newest FFT_N samples of the scope's sound (the mix at MASTER full, or the selected
 * track) as SPEC_COLS levels on a log frequency axis from bin 1 (43 Hz) to bin 240 (10.3 kHz), 16 columns an octave: a
 * column narrower than a bin read between the two either side of it, a wider one its loudest bin. A level is down to
 * 85 dB under a full-scale sine, SPEC_Q_DB steps a dB; the height shows RANGE of them (spec_tune). The newest slice in front, the older crawling back a slice a redraw,
 * SPEC_SLICES of them (a second), drawn as WAVE's (ui_wf.c); kept in the FM6 panel's room (ui_fm6.c fm_room_lend): after
 * an FM6 page's envelopes or WAVE, another sound, or a second away, the history starts over */
#define SPEC_COLS 128u                               /* (x 25 slices: 3200 B of the room's 4608) */
#define SPEC_SLICES 25u
#define SPEC_SLICE_MS 40u
#define SPEC_Q_DB 3                                  /* (255 levels: 85 dB, room for RANGE 80) */
#define SPEC_FS_L2 (26 << 16)                         /* a full-scale sine's bin power, log2 Q16: (32767 / 4)^2 */
#define SPEC_Q_PER_L2 2312                           /* levels a unit of that log2, Q8: SPEC_Q_DB x 3.01 dB */
#define SPEC_TOP_FALL_Q8 307                         /* AUTO's top falls 10 dB a second: 1.2 levels a slice, Q8 */
#define SPEC_TOP_LOWEST (255 - 48 * SPEC_Q_DB)       /* and stops 48 dB under full scale: silence's hiss stays low */
#define SPEC_EDGE0_Q16 64137                         /* column 0's low edge in bins, Q16: 240^(-1/254) */
#define SPEC_COL_Q16 68426                           /* a column's high edge over its low one, Q16: 240^(1/127) */
#define SPEC_FILL 30
typedef struct {                                      /* where a waterfall is drawn */
    int16_t x0, x1;                                   /* the front slice from x0, the back one's end at x1 */
    int16_t floor_y;                                  /* the front slice's foot: its axis under it */
    int16_t top_y;                                    /* nothing drawn above */
    int16_t step;                                     /* px a slice further back moves right and up */
    int16_t amp;                                      /* a slice's RANGE at ZOOM Y 100 %, px */
} spec_geo_t;
/* (#71) the height: RANGE dB under the top (never under the levels' own floor), the top full scale or (AUTO) the loudest
 * column of late, falling 10 dB a second, so a quiet mix or track fills it as a loud one does; ZOOM Y stretches it
 * (clipped at the top). SHIFT + KNOB 1,
 * 2, 4 on the FM6 sheet's SPECTRUM (TONE), RANGE also SHIFT + SELECT on HOME's; shared by both, not saved */
static const uint8_t SPEC_RANGE_DB[3] = {48, 64, 80};
typedef struct {
    uint8_t range;                                    /* SPEC_RANGE_DB's */
    uint8_t top_full_scale;                           /* 0: AUTO */
    uint8_t zoom_q;                                   /* ZOOM Y in 25 % steps: 2 .. 8 */
} spec_tune_t;
static spec_tune_t spec_tune __attribute__((section(".pool")));
static struct {
    uint32_t tick;                                    /* fm1_ms / SPEC_SLICE_MS when the newest slice was taken */
    uint8_t front;                                    /* its row in the room */
    uint8_t sound;                                    /* what the history is of: 0 the mix, else the track + 1 */
    uint8_t holds_hd;                                 /* the room is SPECTRUM HD's, not the history */
    uint16_t top;                                     /* AUTO's top level, Q8 */
    uint16_t hd_n;                                    /* SPECTRUM HD: the samples gathered, */
    uint32_t hd_w;                                    /* the next one's place in the scope's count, */
    uint8_t hd_gen;                                   /* the spectra made (the panel redrawn for each) */
    spec_geo_t geo;                                   /* the one being drawn */
} spec __attribute__((section(".pool")));
static uint8_t *fm_room_lend(int *kept);

/* a level from how far under full scale a power is (log2 Q16) */
static int32_t spec_level(int32_t under_fs)
{
    return clamp(255 - (((under_fs >> 8) * SPEC_Q_PER_L2) >> 16), 0, 255);
}

/* bin k's level; the snapshot was raised by shift bits */
static int32_t spec_bin(uint32_t k, int32_t shift)
{
    uint32_t power = (uint32_t)(fft_re[k] * fft_re[k]) + (uint32_t)(fft_im[k] * fft_im[k]);
    return power ? spec_level(SPEC_FS_L2 + (shift << 17) - log2_q16(power)) : 0;
}

/* the column from bin lo to bin hi (Q16), the bins' levels from bin */
static uint8_t spec_col(uint32_t lo, uint32_t hi, int32_t shift, int32_t (*bin)(uint32_t k, int32_t shift))
{
    uint32_t k = (lo >> 16) + 1u, mid = (lo + hi) >> 1;
    int32_t loudest = 0, a, b;
    if (k << 16 > hi) {
        a = bin(mid >> 16, shift);
        b = bin((mid >> 16) + 1u, shift);
        return (uint8_t)(a + (((b - a) * (int32_t)(mid & 0xFFFFu)) >> 16));
    }
    for (; k << 16 <= hi; k++) {
        int32_t v = bin(k, shift);
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
        col[c] = spec_col(lo, hi, shift, spec_bin);
    }
}

/* slice j back from the newest */
static const uint8_t *spec_row(const uint8_t *room, uint32_t j)
{
    return room + (spec.front + j) % SPEC_SLICES * SPEC_COLS;
}

static uint8_t spec_sound(void) { return (uint8_t)(scope_track_view() ? song.sel + 1u : 0u); }

static const spec_tune_t *spec_cfg(void)
{
    if (!spec_tune.zoom_q)
        spec_tune = (spec_tune_t){1, 0, 4};
    return &spec_tune;
}
static int32_t spec_range(void) { return SPEC_RANGE_DB[spec_cfg()->range] * SPEC_Q_DB; }
static int32_t spec_top_q8(void) { return spec_cfg()->top_full_scale ? 255 << 8 : spec.top; }

/* AUTO's top after ticks: up to the loudest of the n new columns at once, else falling */
static void spec_follow_top(const uint8_t *col, uint32_t n, uint32_t ticks)
{
    int32_t top = (int32_t)spec.top - (int32_t)(ticks > SPEC_SLICES ? SPEC_SLICES : ticks) * SPEC_TOP_FALL_Q8;
    uint32_t c;
    for (c = 0; c < n; c++)
        if (col[c] << 8 > top)
            top = col[c] << 8;
    spec.top = (uint16_t)(top > SPEC_TOP_LOWEST << 8 ? top : SPEC_TOP_LOWEST << 8);
}

/* the history, a new slice in front once a tick */
static const uint8_t *spec_history(void)
{
    int kept;
    uint8_t *room = fm_room_lend(&kept);
    uint32_t tick = fm1_ms / SPEC_SLICE_MS;
    if (kept && !spec.holds_hd && tick == spec.tick && spec.sound == spec_sound())
        return room;
    if (!kept || spec.holds_hd || tick - spec.tick > SPEC_SLICES || spec.sound != spec_sound())
        memset(room, 0, SPEC_SLICES * SPEC_COLS);
    spec.holds_hd = 0;
    spec.sound = spec_sound();
    spec.front = (uint8_t)((spec.front + SPEC_SLICES - 1u) % SPEC_SLICES);
    spec_slice(room + spec.front * SPEC_COLS);
    spec_follow_top(room + spec.front * SPEC_COLS, SPEC_COLS, tick - spec.tick);
    spec.tick = tick;
    return room;
}

static int32_t spec_width(void) { return spec.geo.x1 - spec.geo.x0 - ((int32_t)SPEC_SLICES - 1) * spec.geo.step; }
static int32_t spec_foot_y(uint32_t j) { return spec.geo.floor_y - (int32_t)j * spec.geo.step; }
/* a level (Q8) on a line whose foot is at foot, in 1/256 px */
static int32_t spec_level_y256(int32_t foot, int32_t level256)
{
    int32_t top = spec_top_q8(), floor = top > spec_range() << 8 ? top - (spec_range() << 8) : 0;
    int32_t over_floor = level256 > floor ? level256 - floor : 0;
    int32_t y = (foot << 8) - over_floor * spec.geo.amp * spec_cfg()->zoom_q / ((top - floor) >> 6);
    return y > spec.geo.top_y << 8 ? y : spec.geo.top_y << 8;
}
static int32_t spec_y256(const void *v, uint32_t j, int32_t col, int32_t w)
{
    const uint8_t *lv = v;
    uint32_t pos = (uint32_t)col * (SPEC_COLS - 1u) * 256u / (uint32_t)w, i = pos >> 8;
    int32_t a = lv[i], b = lv[i < SPEC_COLS - 1u ? i + 1u : i];
    return spec_level_y256(spec_foot_y(j), (a << 8) + (b - a) * (int32_t)(pos & 255u));
}

/* the frequencies under the front slice */
static void spec_axis(void)
{
    static const struct { const char *hz; uint8_t col; } MARK[3] = {{"100", 20}, {"1K", 73}, {"10K", 126}};
    int32_t y = spec.geo.floor_y + 4, k;
    for (k = 0; k < 3; k++) {
        int32_t x = spec.geo.x0 + MARK[k].col * spec_width() / ((int32_t)SPEC_COLS - 1);
        cv_rect(x, spec.geo.floor_y + 1, 1, 2, T_MID);
        cv_text_in(x - 15, y, 30, &AF_S, MARK[k].hz, T_MID, T_SURF);
    }
}

static void spec_draw(const spec_geo_t *g, uint16_t c)
{
    const uint8_t *room = spec_history();
    uint32_t j;
    spec.geo = *g;
    for (j = SPEC_SLICES; j-- > 0;) {
        const wf_slice_t s = {spec_row(room, j), j, g->x0 + (int32_t)j * g->step, spec_width(), spec_foot_y(j) + 2,
                              SPEC_FILL, spec_y256};
        wf_draw_slice(&s, ux.h1n ? (j ? T_MID : T_TEXT) : ux_mix(T_SURF, c, wf_fade(j, SPEC_SLICES)));
    }
    spec_axis();
}

/* the top and the range, as the FM6 sheet's caption says them: "TOP -12 DB  64 DB" */
static void spec_scale_text(char *b, uint32_t n)
{
    str_cpy(b, "TOP ", n);
    fmt_int(b + str_len(b), -((255 << 8) - spec_top_q8() + (SPEC_Q_DB << 7)) / (SPEC_Q_DB << 8));
    str_cpy(b + str_len(b), " DB  ", n - str_len(b));
    fmt_int(b + str_len(b), SPEC_RANGE_DB[spec_cfg()->range]);
    str_cpy(b + str_len(b), " DB", n - str_len(b));
}

/* (#71) SHIFT + SELECT on HOME's SPECTRUM: RANGE. 0: not there */
static int spec_range_step(int32_t s)
{
    char b[8];
    if (!home_spectrum())
        return 0;
    spec_tune.range = (uint8_t)clamp((int32_t)spec_cfg()->range + (s > 0 ? 1 : -1), 0, 2);
    fmt_int(b, SPEC_RANGE_DB[spec_tune.range]);
    str_cpy(b + str_len(b), " DB", sizeof b - str_len(b));
    ui_say("SPECTRUM RANGE ", b);
    return 1;
}

/* (#71) SHIFT + KNOB 1..4 over the FM6 sheet's SPECTRUM: RANGE, TOP, (none), ZOOM Y */
static void spec_tune_turn(uint32_t k, int32_t s)
{
    (void)spec_cfg();
    if (k == 0u)
        spec_tune.range = (uint8_t)clamp((int32_t)spec_tune.range + s, 0, 2);
    else if (k == 1u)
        spec_tune.top_full_scale = s > 0;
    else if (k == 3u)
        spec_tune.zoom_q = (uint8_t)clamp((int32_t)spec_tune.zoom_q + s, 2, 8);
}
/* (#71) SPECTRUM HD (HOME's fifth tap: the mix): one spectrum at 4x the waterfall's resolution, no history. The room
 * holds SPEC_HD_N consecutive samples of the scope instead, gathered every frame (spec_hd_gather: the scope's ring holds
 * 23 ms, a frame comes every 15 ms or so; a longer tap would cost the audio ISR's buffer 3 KB the pool has not got),
 * two a complex point (the even ones real, the odd imaginary) for a SPEC_HD_PTS point FFT in place, the real spectrum
 * split out of it: bins of 10.8 Hz, a 93 ms window, ~10 spectra a second. A frame later than the ring (a gap) starts
 * the window over. SPEC_HD_COLS columns, a pixel each, on a log axis from bin 2 (21.5 Hz) to bin 960 (10.3 kHz), 24 an
 * octave, under a peak line held and falling ~10 dB a second; RANGE, TOP and ZOOM Y as the waterfall's */
#define SPEC_HD_N 2048u
#define SPEC_HD_PTS (SPEC_HD_N / 2u)
#define SPEC_HD_COLS 216u
#define SPEC_HD_FS_L2 (30 << 16)                      /* a full-scale sine's bin power, log2 Q16: 32767^2 (the split's 4x) */
#define SPEC_HD_EDGE0_Q16 129204                     /* column 0's low edge in bins, Q16: 2 x 480^(-1/430) */
#define SPEC_HD_COL_Q16 67445                        /* a column's high edge over its low one, Q16: 480^(1/215) */
#define SPEC_HD_PEAK_FALL 3                          /* levels a spectrum */
#define SPEC_HD_GAP (SCOPE_N - 64u)                  /* how far behind the ring's head its samples are still whole */
typedef struct {
    int16_t re[SPEC_HD_PTS], im[SPEC_HD_PTS];
    uint8_t level[SPEC_HD_COLS], peak[SPEC_HD_COLS];
} spec_hd_room_t;
_Static_assert(sizeof(spec_hd_room_t) <= 4608u, "SPECTRUM HD in the FM6 panel's room (ui_fm6.c FM_WF_BUF)");
static const spec_hd_room_t *spec_hd_fft __attribute__((section(".pool")));   /* (spec_hd_bin's, while analysed) */

/* SINE at half its steps (2048 a turn), the odd ones between two */
static int32_t sine_half_step(uint32_t a2) { return (SINE[(a2 >> 1) & 1023u] + SINE[((a2 + 1u) >> 1) & 1023u]) >> 1; }
static int32_t log2_q16_wide(uint64_t x)
{
    int32_t e = 0;
    for (; x >> 32; x >>= 2)
        e += 2;
    return log2_q16((uint32_t)x) + (e << 16);
}

/* bin k of the samples' real spectrum, 4x: Z[k] + conj Z[N-k] - i W^k (Z[k] - conj Z[N-k]), W = e^(-i pi / N) */
static int32_t spec_hd_bin(uint32_t k, int32_t shift)
{
    const spec_hd_room_t *r = spec_hd_fft;
    uint32_t m = (SPEC_HD_PTS - k) & (SPEC_HD_PTS - 1u);
    int32_t er = r->re[k] + r->re[m], ei = r->im[k] - r->im[m], dr = r->re[k] - r->re[m], di = r->im[k] + r->im[m];
    int32_t c = sine_half_step(k + 512u), s = sine_half_step(k);
    int64_t xr = er + ((c * di - s * dr) >> 15), xi = ei - ((c * dr + s * di) >> 15);
    uint64_t power = (uint64_t)(xr * xr + xi * xi);
    return power ? spec_level(SPEC_HD_FS_L2 + (shift << 17) - log2_q16_wide(power)) : 0;
}

/* the gathered samples raised (or halved) by whole bits to 16383 at most (a complex point's parts then stay inside 16
 * bits through the FFT), Hann-windowed, through the FFT; the columns, the peak line and AUTO's top from it */
static void spec_hd_analyse(spec_hd_room_t *r)
{
    int32_t loudest = 0, shift;
    uint32_t i, c, lo = SPEC_HD_EDGE0_Q16, hi, tick = fm1_ms / SPEC_SLICE_MS;
    for (i = 0; i < SPEC_HD_N; i++) {
        int32_t x = (i & 1u ? r->im : r->re)[i >> 1], size = x < 0 ? -x : x;
        if (size > loudest)
            loudest = size;
    }
    for (shift = loudest > 16383 ? -1 : 0; loudest && shift >= 0 && loudest << (shift + 1) <= 16383;)
        shift++;
    for (i = 0; i < SPEC_HD_N; i++) {
        int16_t *x = (i & 1u ? r->im : r->re) + (i >> 1);
        int32_t raised = shift < 0 ? *x >> 1 : *x << shift, hann = 32767 - sine_half_step(i + 512u);
        *x = (int16_t)(raised * hann >> 16);
    }
    fft_run(r->re, r->im, SPEC_HD_PTS);
    spec_hd_fft = r;
    for (c = 0; c < SPEC_HD_COLS; c++, lo = hi) {
        int32_t fallen;
        hi = (uint32_t)((uint64_t)lo * SPEC_HD_COL_Q16 >> 16);
        r->level[c] = spec_col(lo, hi, shift, spec_hd_bin);
        fallen = r->peak[c] - SPEC_HD_PEAK_FALL;
        r->peak[c] = (uint8_t)(r->level[c] > fallen ? r->level[c] : fallen);
    }
    spec_follow_top(r->level, SPEC_HD_COLS, tick - spec.tick);
    spec.tick = tick;
    spec.hd_gen++;
}

/* the room as SPECTRUM HD's: taken over, the window starts with what the scope's ring still holds */
static spec_hd_room_t *spec_hd_room(void)
{
    int kept;
    spec_hd_room_t *r = (spec_hd_room_t *)fm_room_lend(&kept);
    if (kept && spec.holds_hd)
        return r;
    memset(r->level, 0, sizeof r->level);
    memset(r->peak, 0, sizeof r->peak);
    spec.holds_hd = 1;
    spec.hd_n = 0;
    spec.hd_w = scope_head() - SPEC_HD_GAP;
    spec.top = SPEC_TOP_LOWEST << 8;
    spec.tick = fm1_ms / SPEC_SLICE_MS;
    return r;
}

/* every frame while SPECTRUM HD shows (ui_draw.c ui_draw): the scope's new samples into the window, a spectrum made
 * once it is full */
static void spec_hd_gather(void)
{
    spec_hd_room_t *r = spec_hd_room();
    uint32_t head = scope_head();
    int32_t unmaster = scope_unmaster_q8();
    if (head - spec.hd_w > SPEC_HD_GAP) {
        spec.hd_n = 0;
        spec.hd_w = head - SPEC_HD_GAP;
    }
    for (; spec.hd_w != head && spec.hd_n < SPEC_HD_N; spec.hd_w++, spec.hd_n++)
        (spec.hd_n & 1u ? r->im : r->re)[spec.hd_n >> 1] = scope_sample(spec.hd_w, unmaster);
    if (spec.hd_n < SPEC_HD_N)
        return;
    spec_hd_analyse(r);
    spec.hd_n = 0;
}

static int32_t spec_hd_y256(const void *v, uint32_t j, int32_t col, int32_t w)
{
    (void)j;
    (void)w;
    return spec_level_y256(spec.geo.floor_y, ((const uint8_t *)v)[col] << 8);
}

static void spec_hd_axis(void)
{
    static const struct { const char *hz; uint8_t col; } MARK[8] = {{"50", 29}, {"100", 53}, {"200", 78}, {"500", 110},
                                                                    {"1K", 134}, {"2K", 158}, {"5K", 190}, {"10K", 214}};
    int32_t k;
    for (k = 0; k < 8; k++) {
        int32_t x = spec.geo.x0 + MARK[k].col;
        cv_rect(x, spec.geo.floor_y + 1, 1, 2, T_MID);
        cv_text_in(x - 11, spec.geo.floor_y + 4, 22, &AF_S, MARK[k].hz, T_MID, T_SURF);
    }
}

/* the spectrum filled in under its line, the peak line over it */
static void spec_hd_draw(const spec_geo_t *g, uint16_t c)
{
    const spec_hd_room_t *r = spec_hd_room();
    const int32_t w = (int32_t)SPEC_HD_COLS - 1;
    const wf_slice_t peak = {r->peak, 1, g->x0, w, 0, 0, spec_hd_y256}, now = {r->level, 0, g->x0, w, 0, 0, spec_hd_y256};
    int32_t col;
    spec.geo = *g;
    for (col = 1; col <= w; col++) {
        int32_t y = spec_hd_y256(r->level, 0, col, w) >> 8;
        cv_rect(g->x0 + col, y + 1, 1, g->floor_y - y, ux.h1n ? T_DIM : ux_mix(T_SURF, c, 25));
    }
    wf_draw_slice(&peak, ux.h1n ? T_MID : ux_mix(T_SURF, c, 55));
    wf_draw_slice(&now, ux.h1n ? T_TEXT : c);
    spec_hd_axis();
}

/* HOME's: the panel's width; the waterfall's back slice up to y 4, HD's peaks clear of the caption */
static void graph_spectrum(uint16_t c)
{
    const spec_geo_t waterfall = {12, 228, (int16_t)(cv_h - 20), 4, 2, 50};
    const spec_geo_t hd = {11, 228, (int16_t)(cv_h - 20), 18, 0, (int16_t)(cv_h - 38)};
    if (home_spectrum_hd())
        spec_hd_draw(&hd, c);
    else
        spec_draw(&waterfall, c);
}
