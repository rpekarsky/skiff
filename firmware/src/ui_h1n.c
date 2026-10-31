/* SPDX-License-Identifier: GPL-3.0-only */
/* (Skiff #29) THEME LCD's widgets (skiff-tasks/lcd-mockups.png, H1N GREY, the PO's pick): a reflective monochrome LCD,
 * the Zoom H1n's. Four tones and the dot grid are gfx.c's (lcd_tones, cv_lcd_rows); here what an LCD draws in
 * segments: the gauges, the dialog's window, HOME's equalizer. On the canvas (cv_*) when ux.h1n; CRYPT's pixel faces.
 * Included by ui_graph.c. */

#define HN_SEG 4                                      /* a segment and its gap */

/* (#48) the rail at the screen's left edge, on every screen: HOME's lanes in miniature where they are on HOME, the
 * selected track's division ink and taller, the others ghost; the track selected told by its place alone. Columns
 * 0 .. HN_RAIL_X1 - 1 of the panel's rows are the rail's: its divisions, then a BG column; the pages start at x 3 */
#define HN_RAIL_W 2
#define HN_RAIL_X1 3
#define HN_RAIL_H ((int32_t)(H_GRAPH + FOOT_LINE))    /* (HOME's panel, MENU > LARGE or not) */
static int32_t hl_lane_y(uint32_t k, uint32_t sel, int32_t panel_h, int32_t *h);   /* (ui_home.c) */
static uint8_t hn_rail_sel __attribute__((section(".pool")));   /* the track the rail on the screen shows */

static uint16_t hn_rail_tone(int32_t sy)              /* screen row sy of the divisions' columns */
{
    uint32_t k;
    for (k = 0; k < NTRK; k++) {
        int32_t h, y = Y_GRAPH + hl_lane_y(k, song.sel, HN_RAIL_H, &h);
        if (sy >= y && sy < y + h)
            return k == song.sel ? T_TEXT : T_RAISE;
    }
    return T_BG;
}

/* the rail into the canvas about to go to the screen at (x, y), its rows r0.. (gfx.c cv_lcd_rows) */
static void hn_rail_rows(uint32_t x, uint32_t y, uint32_t r0)
{
    uint32_t r, i;
    if (x >= HN_RAIL_X1 || ui.uboot)
        return;
    for (r = r0; r < cv_h; r++) {
        int32_t sy = (int32_t)(y + r);
        uint16_t tone;
        if (sy < Y_GRAPH || sy >= Y_GRAPH + HN_RAIL_H)
            continue;
        tone = swap16(hn_rail_tone(sy));
        for (i = x; i < HN_RAIL_X1 && i - x < cv_w; i++)
            cv_px[r * cv_w + i - x] = i < HN_RAIL_W ? tone : swap16(T_BG);
    }
}

/* the rail alone to the screen: over what the page left there (ui_draw.c ui_draw: a full redraw, a track selected) */
static void hn_rail_paint(void)
{
    cv_fill_bg(0, Y_GRAPH, HN_RAIL_X1, (uint32_t)HN_RAIL_H);
    hn_rail_sel = song.sel;
}

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

/* HOME's scope as the LCD's equalizer: the output's spectrum in HN_BANDS bands, log-spaced from 86 Hz to 11 kHz (bins
 * of a 512-point FFT of the scope's 22.05 kHz snapshot, in the main loop at redraw), each a column of HN_ROWS segments
 * 3 dB apart, the loudest band of a pure tone at the top (the snapshot at MASTER full, as the scope's, then scaled to
 * its peak). A bar falls a segment a redraw; its peak stays lit HN_HOLD redraws, then falls too */
#define HN_FFT 512u
#define HN_BANDS 16u
#define HN_ROWS 14
#define HN_HOLD 16u
static const uint16_t HN_EDGE[HN_BANDS + 1u] = {2, 3, 4, 5, 7, 9, 12, 17, 23, 30, 41, 56, 76, 103, 139, 188, 256};
static int16_t hn_re[HN_FFT], hn_im[HN_FFT];
static uint8_t hn_bar[HN_BANDS], hn_peak[HN_BANDS], hn_peak_t[HN_BANDS];

/* in place, radix 2, halved at each of its 9 stages (the magnitudes never grow: no overflow); the twiddles from SINE */
static void hn_fft(void)
{
    uint32_t i, j, k, half;
    for (i = 1, j = 0; i < HN_FFT; i++) {
        uint32_t b = HN_FFT >> 1;
        for (; j & b; b >>= 1)
            j ^= b;
        j |= b;
        if (i < j) {
            int16_t t = hn_re[i];
            hn_re[i] = hn_re[j];
            hn_re[j] = t;
        }
    }
    for (half = 1; half < HN_FFT; half <<= 1)
        for (k = 0; k < half; k++) {
            uint32_t a = k * (512u / half);           /* (SINE: 1024 a turn; the angle 2 pi k / (2 half)) */
            int32_t c = SINE[(a + 256u) & 1023u], s = -SINE[a & 1023u];
            for (i = k; i < HN_FFT; i += 2u * half) {
                int32_t tr = (hn_re[i + half] * c - hn_im[i + half] * s) >> 15;
                int32_t ti = (hn_re[i + half] * s + hn_im[i + half] * c) >> 15;
                hn_re[i + half] = (int16_t)((hn_re[i] - tr) >> 1);
                hn_im[i + half] = (int16_t)((hn_im[i] - ti) >> 1);
                hn_re[i] = (int16_t)((hn_re[i] + tr) >> 1);
                hn_im[i] = (int16_t)((hn_im[i] + ti) >> 1);
            }
        }
}

/* each band's level in segments (0 .. HN_ROWS) from the scope's last HN_FFT samples, Hann-windowed */
static void hn_spectrum(uint8_t *lv)
{
    uint32_t i, k;
    int32_t peak = 1500, gain;
    scope_snapshot(hn_re);
    for (i = 0; i < HN_FFT; i++) {
        int32_t v = hn_re[i];
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
    }
    gain = (16384 << 15) / peak;
    for (i = 0; i < HN_FFT; i++) {
        int32_t x = hn_re[i] * gain >> 15, hann = 32767 - SINE[(2u * i + 256u) & 1023u];
        hn_re[i] = (int16_t)(x * hann >> 16);
        hn_im[i] = 0;
    }
    hn_fft();
    for (k = 0; k < HN_BANDS; k++) {
        uint32_t pw = 0;                              /* the power / 64: a band's 68 bins at full scale fit */
        int32_t l2 = 0;
        for (i = HN_EDGE[k]; i < HN_EDGE[k + 1u]; i++)
            pw += (uint32_t)(hn_re[i] * hn_re[i] + hn_im[i] * hn_im[i]) >> 6;
        for (; pw >= 2u; pw >>= 1)                    /* log2 of the power: 3 dB a step */
            l2++;
        l2 -= 24 - 6 - HN_ROWS;                       /* (a full-scale tone: 4096 in its bin, 2^24) */
        lv[k] = (uint8_t)(l2 < 0 ? 0 : l2 > HN_ROWS ? HN_ROWS : l2);
    }
}

static void hn_eq(void)
{
    static const char *const MARK[HN_BANDS] = {[0] = "100", [8] = "1K", [15] = "10K"};
    uint8_t lv[HN_BANDS];
    int marks = cv_h >= H_GRAPH;                      /* (MENU > LARGE's strip: no room for them) */
    int32_t pitch = ((int32_t)cv_h - (marks ? 30 : 6)) / HN_ROWS, gap = pitch > 4 ? 2 : 1;
    int32_t top = marks ? (int32_t)cv_h - 22 - pitch * HN_ROWS : ((int32_t)cv_h - pitch * HN_ROWS) / 2;
    int32_t bw = PANEL_W / (int32_t)HN_BANDS, x0 = PANEL_X0 + (PANEL_W - bw * (int32_t)HN_BANDS) / 2 + 1, r;
    uint32_t k;
    hn_spectrum(lv);
    for (k = 0; k < HN_BANDS; k++) {
        int32_t x = x0 + (int32_t)k * bw;
        if (hn_bar[k] > lv[k])
            lv[k] = (uint8_t)(hn_bar[k] - 1u);
        hn_bar[k] = lv[k];
        if (lv[k] >= hn_peak[k]) {
            hn_peak[k] = lv[k];
            hn_peak_t[k] = HN_HOLD;
        } else if (hn_peak_t[k])
            hn_peak_t[k]--;
        else
            hn_peak[k]--;
        for (r = 0; r < HN_ROWS; r++)
            cv_rect(x, top + (HN_ROWS - 1 - r) * pitch, bw - 3, pitch - gap,
                    r < lv[k] || r + 1 == hn_peak[k] ? T_TEXT : T_RAISE);
        if (marks && MARK[k])
            cv_text(x, (int32_t)cv_h - 16, &AF_S, MARK[k], T_MID);
    }
}
