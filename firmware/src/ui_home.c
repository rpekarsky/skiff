/* SPDX-License-Identifier: GPL-3.0-only
 * Skiff: HOME's panel, the tracks at a glance (DESIGN.md rule 10). Included by ui_graph.c (draw_graph). ((Skiff U3, N1)
 * all six: the selected one at today's size, the others compact; phase 6 showed four with a scroll bar)
 * A lane per track: its number (never the colour alone), its sound's name over its engine's, and its whole pattern:
 * cells in fours, a note or a hit filled in the track's colour, a tie half filled; the step playing marked. ((#41, as
 * Melodee) LEN 16 a row of 16; to 32 two rows of 16; to 64 two rows of 32 narrower cells; on two rows the bar playing
 * underlined.)
 * STYLE BLOCKS (direction B): each lane a tile on BG, the number big in the track's colour, an empty step a block of
 * LINE, the step playing a line before its cell; the selected track a band of its colour, all on it INK (its empty
 * steps the colour half way to INK). Else in the panel: the number on its cushion, an empty step a stub, the step
 * playing a bar under its cell, the selected track a RAISE row. A muted track's cells DIM. HOME tapped on HOME shows
 * the scope instead (ui.home_scope). Redrawn only when what it shows changes (home_sig), not every other frame. */
#define HL_H 31                                       /* a lane: the B mockup's 31, 3 between them from the panel's top */
#define HL_NUM_X 9                                    /* the number's ink (BLOCKS), the cushion's cell */
#define HL_NAME_X 28                                  /* the sound's name (M) over its engine's (S), to the steps */
#define HL_NAME_CY 2                                  /* their capitals' tops in the lane: 2..13, 16..25 */
#define HL_ENG_CY 16
#define HL_STEP_X 100                                 /* 16 cells a row: 6 px, 2 between, 2 more between the fours */
#define HL_CELL_W 6
#define HL_CELL_H 16
#define HL_NCELL_W 3                                  /* 32 a row: 3 px, 1 between, 1 more between the fours, 2 the bars */
#define HL_ROW_GAP 3                                  /* two rows: the bar playing's line in it */
static void sound_name(const track_t *t, char *b);   /* (ui_draw.c) */
static int32_t hl_cell_x(uint32_t j, uint32_t per_row)
{
    if (per_row > 16u)
        return HL_STEP_X + (int32_t)j * (HL_NCELL_W + 1) + (int32_t)(j / 4u) + (int32_t)(j / 16u);
    return HL_STEP_X + (int32_t)j * (HL_CELL_W + 2) + (int32_t)(j / 4u) * 2;
}
static uint32_t hl_len(const track_t *t) { return t->p[P_SLEN] > 0 ? (uint32_t)clamp(t->p[P_SLEN], 1, NSTEP) : 16u; }
static uint32_t hl_per_row(uint32_t len) { return len > 32u ? 32u : 16u; }

/* (#34) the pattern track k is on, a chain's part playing ("06"); "" none (nothing saved yet, a REST) */
static void hl_pat(uint32_t k, char *b)
{
    b[0] = 0;
    if (trk_pat(k) != PAT_NONE)
        pat_num(b, trk_pat(k));
}

/* (#59) each lane's level meter: the peak fx.c's mix_part keeps of the track as it enters the mix (t->peak: after its
 * LEVEL, DRIVE, INSERT, SLICER, mute and DUCK, before PAN and MASTER; nothing more in the audio ISR), read and cleared
 * every HM_TICK_MS (the panel redrawn at most that often for it), shown in 3/4 dB steps from 48 dB under full scale
 * (0) to full scale (HM_STEPS), falling HM_FALL steps a tick. SKIFF_HOME_METER: 1 a bar at the lane's left edge, 2 a
 * faint fill under the lane as wide as the level, 0 none */
#ifndef SKIFF_HOME_METER
#define SKIFF_HOME_METER 1
#endif
enum { HM_NONE, HM_BAR, HM_UNDER };
#define HM_TICK_MS 45u                                /* (every third frame: ~20 Hz) */
#define HM_STEPS 64u
#define HM_FULL_L8 (15u * 8u)                         /* log2 of full scale (32768: the mix at MASTER full), in eighths */
#define HM_FALL 2u                                    /* ~33 dB/s */
#define HM_BAR_W 3
static uint8_t hm_style = SKIFF_HOME_METER;
static uint8_t hm_lv[NTRK];
static uint32_t hm_t;

/* lane k's meter (y, h: the lane; lane its background, col the track's colour, band: the selected lane, INK on it) */
static void hm_draw(uint32_t k, int32_t y, int32_t h, uint16_t lane, uint16_t col, int band)
{
    int32_t lv = hm_lv[k];
    if (!lv)
        return;
    if (hm_style == HM_BAR) {
        int32_t bh = lv * (h - 4) / (int32_t)HM_STEPS;
        cv_rect(3, y + h - 2 - bh, HM_BAR_W, bh, band ? T_INK : col);
    } else if (ux.neon && band)
        return;                                       /* (NEON's sign: its gradient, no room for a tint) */
    else if (ux.crypt && !band)
        cr_stone(3, y, lv * 234 / (int32_t)HM_STEPS, h, ux_mix(lane, col, 18));
    else
        cv_rect(3, y, lv * 234 / (int32_t)HM_STEPS, h, ux_mix(lane, band ? T_INK : col, 18));
}

/* a peak in meter steps: its log2 in eighths (the top bit's place, then the next three bits) */
static uint32_t hm_steps(int32_t peak)
{
    uint32_t p = peak > 0 ? (uint32_t)peak : 0u, top = 0, l8;
    if (!p)
        return 0;
    while (p >> (top + 1u))
        top++;
    l8 = top * 8u + ((top >= 3u ? p >> (top - 3u) : p << (3u - top)) & 7u);
    return l8 + HM_STEPS <= HM_FULL_L8 ? 0u : l8 >= HM_FULL_L8 ? HM_STEPS : l8 + HM_STEPS - HM_FULL_L8;
}

static void home_meters_tick(void)
{
    uint32_t k, back = fm1_ms - hm_t > 4u * HM_TICK_MS;   /* (back on HOME: not the peaks since it left) */
    if (!hm_style || fm1_ms - hm_t < HM_TICK_MS)
        return;
    hm_t = fm1_ms;
    for (k = 0; k < NTRK; k++) {
        uint32_t now = hm_steps(trk[k].peak), fell = hm_lv[k] > HM_FALL ? hm_lv[k] - HM_FALL : 0u;
        trk[k].peak = 0;                              /* (racing the audio: at worst a block's peak lost or kept) */
        hm_lv[k] = (uint8_t)(back ? 0u : now > fell ? now : fell);
    }
}

#if BOARD_SMK37
/* (#56) a fader moving: over every lane's steps a bar filled to the bank's parameter (PAN from the centre). A fader's
 * track: the fader's place a tick in a gap, apart from the fill's end until it takes over (smk_ui.c smk_faders); the two
 * tracks without one: a thin bar in DIM on RAISE */
static int smk_fader_bars(uint32_t *id);              /* (smk_ui.c) */
static int smk_fader_place(uint32_t k, uint32_t *place);
#define HL_BAR_W 132                                  /* under the 16 cells */
#define HL_THIN_H 3
static int32_t hl_bar_x(int32_t v) { return HL_STEP_X + v * HL_BAR_W / 127; }
static uint32_t hl_fader_sig(uint32_t k)
{
    uint32_t id, place = 255u;                       /* (255: a lane without a fader) */
    if (!smk_fader_bars(&id))
        return 0;
    smk_fader_place(k, &place);
    return 1u + id * 3u + place * 389u + (uint32_t)trk[k].p[id] * 52511u;
}
static int hl_fader_bar(uint32_t k, int32_t y, int32_t h, uint16_t lane, uint16_t fill, uint16_t groove, uint16_t tick)
{
    uint32_t id, place;
    int has_fader;
    int32_t bh, by, from, to, tick_x;
    if (!smk_fader_bars(&id))
        return 0;
    has_fader = smk_fader_place(k, &place);
    if (!has_fader) {
        fill = T_DIM;
        groove = T_RAISE;
    }
    bh = has_fader ? h / 3 : HL_THIN_H;
    by = y + (h - bh) / 2;
    from = hl_bar_x(id == P_PAN ? -TP[id].min : 0);
    to = hl_bar_x(trk[k].p[id] - TP[id].min);
    cv_rect(HL_STEP_X - 3, y + 1, HL_BAR_W + 6, h - 2, lane);
    cv_rect(HL_STEP_X, by, HL_BAR_W, bh, groove);
    cv_rect(from < to ? from : to, by, from < to ? to - from : from - to, bh, fill);
    if (id == P_PAN)
        cv_rect(from, by - 1, 1, bh + 2, fill);
    if (!has_fader)
        return 1;
    tick_x = clamp(hl_bar_x((int32_t)place), HL_STEP_X + 1, HL_STEP_X + HL_BAR_W - 1);
    cv_rect(tick_x - 2, by - 3, 4, bh + 6, lane);
    cv_rect(tick_x - 1, by - 3, 2, bh + 6, tick);
    return 1;
}
#endif

/* what the overview shows: a change of it redraws the panel (draw_graph's signature) */
static uint32_t home_sig(void)
{
    uint32_t h = 2166136261u ^ song.sel * 7919u ^ (uint32_t)song.playing * 104729u ^ ux.gen * 131u, k, i, n;
    for (k = 0; k < NTRK; k++) {
        const track_t *t = &trk[k];
        const step_t *s = t->step;
        n = hl_len(t);
        h = (h ^ trk_pat(k) ^ (uint32_t)hm_lv[k] << 16) * 16777619u;
#if BOARD_SMK37
        h = (h ^ hl_fader_sig(k)) * 16777619u;
#endif
        h = (h ^ (977u * n + 7u * (uint32_t)t->p[P_MUTE] + 31u * t->eng_req + 257u * t->preset + 65537u * t->user +
                  (uint32_t)midi_mark(k) * 0x40000000u)) * 16777619u;
        if (song.playing)
            h = (h ^ (t->seq_idx % n + 1u)) * 16777619u;
        for (i = 0; i < n; i++)
            h = (h ^ (s[i].n | (uint32_t)s[i].hit << 8 | (uint32_t)s[i].time << 16)) * 16777619u;
    }
    return h;
}

/* (Skiff U3, the review's N1; design/mock_u3.py, the user's pick A) every track at once: the selected one's lane as
 * before (HL_H, its name over its engine's), the others compact (HL_C_H: the number in M, the name in S, the cells
 * HL_C_CELL_H); where all fit at today's size (four tracks) all as before */
#define HL_C_H 18
#define HL_C_CELL_H 12
#define HL_C_PAT_X 20                                 /* ((#34) the pattern after the number; the names after it */
#define HL_C_NAME_X 37                                /* when any track shows one) */
#define HL_ROW_H 11                                   /* (#41) a cell on two rows: the selected lane's, the compact's */
#define HL_C_ROW_H 5
static void graph_tracks(void)
{
    uint32_t k, i;
    int32_t tx = -1, ty = 0;                          /* (Skiff C3) CRYPT: the torch's place (the selected track's playhead) */
    int32_t ry = 0;                                   /* .. (Skiff N3) NEON: the car's, its row's middle */
    int32_t qx = -1, qy = 0;                          /* (Skiff Q3) QUARK: the collision's (the same) */
    int six = (int32_t)NTRK * (HL_H + 3) - 3 > (int32_t)graph_h();
    int32_t y = six ? ((int32_t)graph_h() - (HL_H + ((int32_t)NTRK - 1) * (HL_C_H + 3))) / 2 : 0, name_x = HL_NAME_X;
    for (k = 0; k < NTRK; k++)                        /* ((#34) a pattern anywhere: the compact names make room for it) */
        if (trk_pat(k) != PAT_NONE)
            name_x = HL_C_NAME_X;
    cv_rect(0, 0, 240, (int32_t)graph_h(), T_BG);      /* (over the panel: the lanes are its tiles) */
    for (k = 0; k < NTRK; k++) {
        const track_t *t = &trk[k];
        const step_t *s = t->step;
        uint32_t len = hl_len(t), per_row = hl_per_row(len), cur = t->seq_idx % len;
        int sel = k == song.sel, muted = t->p[P_MUTE] != 0, big = sel || !six, two_rows = len > 16u;
        int32_t h = big ? HL_H : HL_C_H, b[4], pb[4];
        int32_t ch = two_rows ? (big ? HL_ROW_H : HL_C_ROW_H) : big ? HL_CELL_H : HL_C_CELL_H;
        int32_t cw = per_row > 16u ? HL_NCELL_W : HL_CELL_W, top = y + (h - (two_rows ? 2 * ch + HL_ROW_GAP : ch)) / 2;
        int ib = sel && !ux.quark;                    /* (the selected lane a band, INK on it; (Skiff Q3) QUARK: a beam,
                                                       * its colour faint, the colours as on the others) */
        uint16_t col = muted ? T_DIM : T_TRK(k), lane = sel ? (ux.quark ? ux_mix(col, T_BG, 80) : col) : ux.quark ? T_BG : T_SURF;
        uint16_t ink = ib ? T_INK : T_TEXT, on = ib ? T_INK : col, off = ib ? ux_mix(col, T_INK, 55) : T_LINE;
        char name[20], pat[3], d[2] = {(char)('1' + k), 0};
        const aafont_t *nf;
        int stacked;
        if (ux.crypt && !sel)                         /* (Skiff C3) CRYPT: the lanes stone */
            cr_stone(3, y, 234, h, lane);
        else if (ux.neon && sel) {                    /* (Skiff N3) NEON: the selected lane a neon sign (its colour into
                                                       * violet), the others asphalt, the lane's paint between them */
            uint16_t st[2] = {col, ux_mix(col, NE_VIOLET, 70)};
            ne_hgrad(3, y, 234, h, st, 2);
        } else
            cv_rect(3, y, 234, h, lane);
        hm_draw(k, y, h, lane, col, ib);
        if (ux.quark && sel) {                        /* (Skiff Q3) QUARK: the beam's walls lit; the others two rails */
            cv_rect(3, y, 234, 1, col);
            cv_rect(3, y + h - 1, 234, 1, col);
        } else if (ux.quark)
            for (i = 0; i < 1u + (uint32_t)two_rows; i++) {
                int32_t rc = top + (int32_t)i * (ch + HL_ROW_GAP) + ch / 2;
                cv_rect(HL_STEP_X - 2, rc - 3, 236 - HL_STEP_X + 2, 1, T_LINE);
                cv_rect(HL_STEP_X - 2, rc + 3, 236 - HL_STEP_X + 2, 1, T_LINE);
            }
        if (ux.neon && k + 1u < NTRK)
            for (i = 6; i < 234u; i += 10u)
                cv_rect((int32_t)i, y + h + 1, 5, 1, ux_mix(NE_YELLOW, T_BG, 65));
        hl_pat(k, pat);
        stacked = big && pat[0];                      /* ((#34) over its pattern, as the name over the engine) */
        nf = big && !stacked ? &AF_V : &AF_M;
        text_ink(nf, d, b);                           /* the number itself: the colour on SURF, INK on the band */
        if (!stacked)
            GFX_HOOK_ALIGN(0, y, 0, y + h, AL_V, "home lane number on its middle");
        cv_text_on(HL_NUM_X - b[0], y + (stacked ? HL_NAME_CY - AF_M_CAP_Y : big ? CAP_IN(V, HL_H) : CAP_IN(M, HL_C_H)),
                   nf, d, ib ? T_INK : col, lane);
        if (pat[0]) {                                 /* ((#34) the pattern; a compact lane: after the number) */
            text_ink(&AF_S, pat, pb);
            cv_text_on((stacked ? HL_NUM_X : HL_C_PAT_X) - pb[0],
                       y + (stacked ? HL_ENG_CY - AF_S_CAP_Y : CAP_IN(S, HL_C_H)), &AF_S, pat,
                       ib ? T_INK : muted ? T_DIM : T_MID, lane);
        }
        if (midi_mark(k))                             /* ((Skiff U4, F4) MIDI IN playing it: a mark after the number;
                                                       * (#34) before it where its pattern is after it) */
            cv_rect(pat[0] && !big ? 4 : HL_NUM_X + (nf == &AF_V ? 14 : 10),
                    stacked ? y + HL_NAME_CY + AF_M_CAP_H / 2 - 2 : y + h / 2 - 2, 4, 4, ib ? T_INK : T_THEME);
        sound_name(t, name);
        if (big) {
            cv_free_text(HL_NAME_X, y + HL_NAME_CY - AF_M_CAP_Y, &AF_M, name[0] ? name : "--", muted && !sel ? T_DIM : ink,
                         lane, HL_STEP_X - 4 - HL_NAME_X);
            cv_free_text(HL_NAME_X, y + HL_ENG_CY - AF_S_CAP_Y, &AF_S, ENGINES[t->eng_req % NENGINES]->name,
                         ib ? T_INK : T_MID, lane, HL_STEP_X - 4 - HL_NAME_X);
        } else {
            GFX_HOOK_ALIGN(0, y, 0, y + h, AL_V, "home compact lane name on its middle");
            cv_free_text(name_x, y + CAP_IN(S, HL_C_H), &AF_S, name[0] ? name : "--", muted ? T_DIM : T_TEXT, lane,
                         HL_STEP_X - 4 - name_x);
        }
        for (i = 0; i < len; i++) {                   /* (past the track's length: nothing) */
            const step_t *st = &s[i];
            int32_t x = hl_cell_x(i % per_row, per_row), cy = top + (int32_t)(i / per_row) * (ch + HL_ROW_GAP);
            int32_t rc = cy + ch / 2;
            if (ux.quark) {                           /* (Skiff Q3) QUARK: a note a bunch of particles in the pipe (played
                                                       * this pass: spent, a ring), a tie its trail, nothing a dot */
                int32_t bx = x + cw / 2, r = per_row > 16u ? 1 : 2;
                int spent = song.playing && i < cur;
                if (step_on(st) || st->hit) {
                    if (spent && r > 1)
                        qk_ellipse(bx, rc, r, r, 0, ux_mix(col, T_BG, 45), 8);
                    else {                            /* ((#41) a narrow cell's ring a dot: spent, faint) */
                        qk_disc(bx, rc, big && !two_rows ? 3 : r, spent ? ux_mix(col, T_BG, 45) : col);
                        if (!spent)
                            cv_rect(bx - 1, rc - 1, 1, 1, ux_mix(col, T_TEXT, 60));
                    }
                } else if (step_tie(st))
                    cv_rect(x - 1, rc, cw + 2, 1, ux_mix(col, T_BG, 40));
                else
                    cv_rect(bx, rc, 1, 1, T_DIM);
            } else if (ux.crypt && !two_rows) {       /* (Skiff) CRYPT: the steps in ASCII, # a note, - a tie, . nothing */
                int32_t base = rc + 4;                /* (S's capitals centred on the cell) */
                if (step_on(st) || st->hit)
                    cr_ch(x, base, &AF_S, '#', on);
                else if (step_tie(st))
                    cr_ch(x + 1, base, &AF_S, '-', on);
                else
                    cr_ch(x + 2, base, &AF_S, '.', sel ? off : T_DIM);
            } else if (ux.crypt) {                    /* ((#41) two rows, no room for S: the same three in pixels) */
                if (step_on(st) || st->hit)
                    cv_rect(x, cy, cw, ch, on);
                else if (step_tie(st))
                    cv_rect(x, rc, cw, 1, on);
                else
                    cv_rect(x + cw / 2, cy + ch - 1, 1, 1, sel ? off : T_DIM);
            } else if (ux.neon && !sel && (step_on(st) || st->hit)) {   /* (Skiff N3) NEON: a lit tube: its glow (none */
                if (per_row == 16u)                                           /* between narrow cells), its hot top */
                    cv_rect(x - 1, cy - 1, cw + 2, ch + 2, ux_mix(on, lane, 65));
                cv_rect(x, cy, cw, ch, on);
                cv_rect(x + 1, cy + 1, cw - 2, 1, ux_mix(on, T_TEXT, 60));
            } else if (ux.neon && !sel && !step_tie(st)) {
                cv_rect(x + cw / 2 - 1, rc, 2, 1, off);   /* (nothing: a dim dash) */
            } else if (step_on(st) || st->hit) {
                cv_rect(x, cy, cw, ch, on);
            } else {
                cv_rect(x, cy, cw, ch, off);
                if (step_tie(st))               /* a tie: the note held on, half the cell */
                    cv_rect(x, cy + ch / 4, cw, ch / 2, on);
            }
            if (song.playing && i == cur) {   /* the step playing: a line before it (two rows: its row's) */
                int32_t px = x - (per_row > 16u ? 1 : 2);
                int32_t py = two_rows ? cy - 1 : y + 2, ph = two_rows ? ch + 2 : h - 4;
                if (ux.neon && per_row == 16u)            /* ((Skiff N3) NEON: a laser, its glow) */
                    cv_rect(x - 3, py, 3, ph, ux_mix(T_ACCENT, lane, 60));
                if (ux.quark) {                       /* ((Skiff Q3) QUARK: the beam line, dotted gold; on the selected
                                                       * lane a collision) */
                    int32_t yy;
                    for (yy = two_rows ? py : y; yy < (two_rows ? py + ph : y + h); yy += 2)
                        cv_rect(px, yy, 1, 1, T_ACCENT);
                    if (sel && !ui_ph_line) {     /* ((Skiff #27) MENU > PLAYHEAD LINE: no collision) */
                        qx = px;
                        qy = rc;
                    }
                } else
                cv_rect(px, py, 1, ph, ux.neon ? T_ACCENT : sel ? T_INK : ux.crypt ? CR_GOLD : T_TEXT);
                if (sel && !ui_ph_line) {         /* ((Skiff #27) PLAYHEAD LINE: no torch, no car) */
                    tx = x - 7;
                    ty = clamp(rc - 12, y, y + h - 24);
                    ry = rc;
                }
            }
        }
        if (two_rows && song.playing) {                    /* (#41) the bar playing: a line under it */
            uint32_t b0 = cur / 16u * 16u, b1 = b0 + 16u < len ? b0 + 16u : len;
            int32_t x0 = hl_cell_x(b0 % per_row, per_row), x1 = hl_cell_x((b1 - 1u) % per_row, per_row) + cw;
            cv_rect(x0, top + (int32_t)(b0 / per_row) * (ch + HL_ROW_GAP) + ch + 1, x1 - x0, 1, ux_mix(on, lane, 55));
        }
#if BOARD_SMK37
        if (hl_fader_bar(k, y, h, lane, ux.h1n && !ib ? T_MID : on, off, ink) && sel)
            tx = qx = -1;                             /* (no torch, car or collision over the bar) */
#endif
        y += h + 3;
    }
    if (ux.crypt && tx >= 0)                          /* (Skiff C3) CRYPT: a torch on it, flickering a step to the next */
        cr_torch(tx, ty, ui.frame / 6u, 2);
    if (ux.quark && qx >= 0)                          /* (Skiff Q3) QUARK: its tracks a new way each step */
        qk_burst(qx, qy, 8, TSEL->seq_idx * 2654435761u + 7u, 24, T_BG);
    if (ux.neon && tx >= 0) {                         /* (Skiff N3) NEON: the car, its nose at the playhead (on the steps
                                                       * just played), its light trails back to the lane's first step */
        int32_t cx = tx + 7 - 24;
        if (cx < HL_STEP_X - 6)
            cx = HL_STEP_X - 6;
        ne_car(cx, ry - 4, SPR_CAR24, 1, T_TEXT, cx - (HL_STEP_X - 4), T_TRK(song.sel));
    }
}
