/* SPDX-License-Identifier: GPL-3.0-only */
/* Skiff on the SMK-37 Pro (BOARD_SMK37): the controls the FM-1 has no counterpart for, as design/SMK37_UX.md has them
 * (picked 2026-10-19, design/mockups/smk37_*.png):
 *   pads C   always a grid, never a second keyboard: where the FM-1's keys show a map of their own (a layer, the DRUM
 *            grid, NAME, SLICES; or a layer's button held) the pads are the 16 white keys in text order (smk_input.h),
 *            elsewhere the selected track's 16 steps of the page shown, a tap setting or clearing a step;
 *   colours A as the screen's step tiles: empty faint white, a step in the track's colour (a tie half), the edited step
 *            white, the one playing brighter (half way to white: a pad is one LED, it has no ring);
 *   faders A the LEVEL of the four tracks the screen shows, SHIFT + FADER BANK stepping LEVEL PAN REVERB DELAY,
 *            each fader taking over once it reaches the value; (#56) on HOME, while one moves, a bar over each
 *            lane (ui_home.c hl_fader_bar);
 *   wheels A, the pedal: as MIDI in on the selected track's channel (bend, CC1: the MOD matrix's MODW; CC64 sustain);
 *            the pedal holds the track's keys too (seq.c);
 *   < >      the selected track's entry before and after (as PERFORM's keys: at once stopped, at LAUNCH playing);
 *            the same in PERFORM and GLO, held or locked (#44), nothing in the other layers;
 *   MASTER   K1, kept with the settings; KEY VELOCITY (MENU > CONTROL).
 * KNOB BANK is SHIFT, its tap UNDO, FADER BANK's REDO (ui_input.c); the button LEDs on or off (ui_input.c
 * smk_leds_out). Main loop only. */

/* (SMK_KV_BYTE, SMK_MASTER_BYTE: ui.c, settings bytes) */

/* ------------------------------------------------------------------------------------------------- pads --- */
static int smk_pads_keys(void)                     /* 1: the pads are the white keys (the keys' own map, a layer's) */
{
    return keys_own() || (fm1_in.buttons & kb_mask) || kb_lock;
}

#define SMK_PAD_FULL 120u                           /* a lit pad: /256 of the LED's full (they glare) */
#define SMK_PAD_WHITE 90u                           /* the edited step's white, per channel */
#define SMK_PAD_FAINT 0x0A0A0Au                     /* an empty step */
#define SMK_PAD_OPTION (SMK_PAD_FULL / 2u)          /* (#43) a layer's options breathe up to half a lit pad: at full
                                                     * they read as the one chosen */
/* (PO 31.10) a pad takes only the hue from the theme's track colour, the brightness from the logic: the colour scaled so
 * its brightest component is q /256 of full. A near-grey track colour has no hue to give, nor has the LCD look (every
 * track its ink, navy since the H1n palette): the track's SKIFF colour instead, so the tracks stay apart */
#define SMK_PAD_GREY 32u                            /* a colour's components this close: grey */
static uint32_t smk_rgb(uint16_t c565, uint32_t q)  /* a screen colour as a pad's 0xRRGGBB */
{
    uint32_t r = (c565 >> 11) * 255u / 31u, g = ((c565 >> 5) & 63u) * 255u / 63u, b = (c565 & 31u) * 255u / 31u;
    uint32_t hi = r > g ? (r > b ? r : b) : (g > b ? g : b), scale;
    if (!hi)
        return 0;
    scale = (q * 255u + hi - 1u) / hi;              /* (rounded up: a full component scales by q exactly, as before) */
    return (r * scale >> 8) << 16 | (g * scale >> 8) << 8 | (b * scale >> 8);
}
static uint32_t smk_trk_rgb(uint32_t k, uint32_t q)
{
    uint16_t c = T_TRK(k);
    uint32_t r = c >> 11 << 1, g = (c >> 5) & 63u, b = (c & 31u) << 1;
    uint32_t hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (ux.h1n || (hi - lo) * 4u < SMK_PAD_GREY)      /* (6-bit components here: 255 / 63 ~ 4) */
        c = UI_TRACKS[UI_SKIFF_INDEX][k % UI_NTRK];
    return smk_rgb(c, q);
}
static uint32_t smk_mixw(uint32_t c, uint32_t t)   /* c toward the pads' white by t /256 */
{
    uint32_t i, o = 0;
    for (i = 0; i < 24u; i += 8u) {
        uint32_t v = (c >> i) & 255u;
        o |= (v + ((SMK_PAD_WHITE - v) * t >> 8)) << i;
    }
    return o;
}
static uint32_t smk_breath(uint32_t c)               /* a breathing pad: dark .. its colour and back, ~1.1 s */
{
    uint32_t ph = fm1_ms % 1100u, t = ph < 550u ? ph : 1100u - ph, q = 40u + t * 216u / 550u, i, o = 0;
    for (i = 0; i < 24u; i += 8u)
        o |= ((((c >> i) & 255u) * q) >> 8) << i;
    return o;
}
/* (wishlist #5, after Snoop37's PAD LIGHT by genkigenki) MENU > PAD LIGHT: a colour's brightest component v times
 * s + (1 - s) K / (v + K), s the setting, K SMK_PAD_KNEE. A lit pad follows the setting, a dim one keeps most of its
 * light (25 %: lit 119 -> 43, empty 10 -> 8), so empty and lit stay apart; the hue kept, a lit component never 0 */
#define SMK_PAD_KNEE 20u
static uint32_t smk_pad_light(uint32_t c)
{
    uint32_t s = 256u - 64u * smk_padl(), v = 0, num, den, i, o = 0;
    for (i = 0; i < 24u; i += 8u)
        if (((c >> i) & 255u) > v)
            v = (c >> i) & 255u;
    if (s == 256u || !v)
        return c;
    num = s * (v + SMK_PAD_KNEE) + (256u - s) * SMK_PAD_KNEE;
    den = 256u * (v + SMK_PAD_KNEE);
    for (i = 0; i < 24u; i += 8u) {
        uint32_t x = (c >> i) & 255u, y = (x * num + den / 2u) / den;
        o |= (x && !y ? 1u : y) << i;
    }
    return o;
}
static void smk_pad_put(uint32_t p, uint32_t c)
{
    c = smk_pad_light(c);
    padled_set(p, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
}

/* (wishlist #42) MENU > PAD FOLLOW: the steps the pads show while the transport plays, as two rows of 8 (smk_pf_row:
 * each row's first step). OFF the page on the screen (ui.bank), as before; PAGE the 16 the playhead is in; ROW the 8
 * it is in on top and below the 8 it plays next, which move up when it gets there: FWD and RAND the row after (the
 * first after the last), REV the row before, PING the way it is going (back at either end); a pattern of 16 steps or
 * fewer: as PAGE, the pads hold it all. Stopped, every mode the screen's page; and where the pads are the white keys
 * (the DRUM grid: its keys edit the screen's page). While a pad is held as a step tap the view stays (not OFF's), so
 * the step under the finger is the one its tap is about */
#define SMK_PF_NONE 0xFFFFu
static uint16_t smk_pf_row[2] = {0, 8};
static void smk_pf_rows(uint32_t mode, uint32_t at, uint32_t len, uint32_t dir, uint32_t down, uint16_t *row)
{
    uint32_t r = at / 8u, rows = (len + 7u) / 8u, back, nx;
    if (mode == SMK_PF_OFF || at >= len) {
        row[0] = (uint16_t)(ui.bank * 16u);
        row[1] = (uint16_t)(row[0] + 8u);
        return;
    }
    if (mode == SMK_PF_PAGE || len <= 16u) {
        row[0] = (uint16_t)(at / 16u * 16u);
        row[1] = (uint16_t)(row[0] + 8u);
        return;
    }
    back = dir == DIR_REV || (dir == DIR_PING && (down ? r > 0u : r + 1u == rows));
    nx = (back ? r + rows - 1u : r + 1u) % rows;
    row[0] = (uint16_t)(r * 8u);
    row[1] = (uint16_t)(nx == r ? SMK_PF_NONE : nx * 8u);
}
static int smk_pads_held(void)                       /* a pad down as a step tap (smk_input.h smk__padq) */
{
    uint32_t p;
    for (p = 0; p < SMK_NPAD; p++)
        if (smk_pad_note[p] == SMK_PAD_TAP)
            return 1;
    return 0;
}
static uint32_t smk_pad_step(uint32_t p) { return (uint32_t)smk_pf_row[p >> 3 & 1u] + (p & 7u); }

/* the selected track's steps the pads show (colours A): the DRUM grid's selected lane, or the notes */
static void smk_pads_steps(void)
{
    const track_t *t = TSEL;
    uint32_t p, len = (uint32_t)t->p[P_SLEN], col = smk_trk_rgb(song.sel, SMK_PAD_FULL);
    uint32_t mode = smk_pad_steps ? smk_pf_mode() : SMK_PF_OFF;
    uint32_t ph = song.playing && t->seq_idx < len ? t->seq_idx : SMK_PF_NONE;
    int edit = !ui.home && !ui.menu && step_page(cur_page()->graph), drum = drum_track(t);
    if (mode == SMK_PF_OFF || !smk_pads_held())
        smk_pf_rows(mode, ph, len, (uint32_t)t->p[P_DIR], t->seq_down, smk_pf_row);
    for (p = 0; p < 16u; p++) {
        uint32_t i = smk_pad_step(p), c;
        const step_t *st = &t->step[i % NSTEP];
        if (i >= len) {
            smk_pad_put(p, 0);
            continue;
        }
        if (drum)
            c = (step_lanes(st) >> ui.lane) & 1u ? col : SMK_PAD_FAINT;
        else
            c = step_on(st) ? col : step_tie(st) ? smk_trk_rgb(song.sel, SMK_PAD_FULL / 2u) : SMK_PAD_FAINT;
        if (edit && i >= sel_lo() && i <= sel_hi() && !song.playing)   /* ((2.0) the selection's range) */
            c = SMK_PAD_WHITE << 16 | SMK_PAD_WHITE << 8 | SMK_PAD_WHITE;
        if (i == ph)
            c = smk_mixw(c, 128u);
        smk_pad_put(p, c);
    }
}

/* ui_input.c ui_leds: the pads' picture for the frame. The keys' own map (a layer's, NAME's, SLICES'): its levels for
 * the white keys (lit, breathing up to half, the mid level, the glow) in the selected track's colour; the DRUM grid
 * and the rest: the steps */
static void smk_pads_out(const uint8_t *nl, const uint8_t *nd, const uint8_t *nb, const uint8_t *nm)
{
    uint32_t w, k, col = smk_trk_rgb(song.sel, SMK_PAD_FULL), option_col = smk_trk_rgb(song.sel, SMK_PAD_OPTION);
    if (!smk_pads_keys() || grid_on()) {
        smk_pads_steps();
        return;
    }
    for (w = 0, k = 0; k < 27u && w < 16u; k++) {
        uint32_t q = LED_KEY0 + k, b = 1u << (q & 7u), c;
        if (key_black(k))
            continue;
        c = (nl[q >> 3] & b) ? col : (nb[q >> 3] & b) ? smk_breath(option_col) :
            (nm[q >> 3] & b) ? smk_trk_rgb(song.sel, SMK_PAD_FULL / 3u) : (nd[q >> 3] & b) ? SMK_PAD_FAINT : 0u;
        smk_pad_put(w++, c);
    }
}

/* a step tap (steps mode): the selected track's step pad p shows (smk_pad_step). A DRUM track: the selected lane's hit
 * there, toggled (as the grid's white keys); a melodic one: a step with notes cleared, an empty one set to the last
 * note played (at the pad's velocity). A run of taps is one undo (SMK_TAPS); on the step pages the cursor goes there */
static const char SMK_TAPS[] = "PAD STEPS";
static void smk_pad_tap(uint32_t p, uint32_t vel)
{
    track_t *t = TSEL;
    uint32_t i = smk_pad_step(p);
    step_t *st;
    if (ui.menu || ui.confirm || name_on() || i >= (uint32_t)t->p[P_SLEN] || i >= NSTEP)
        return;
    st = &t->step[i];
    if (!(undo_n && !undo_back && undo.name == SMK_TAPS && undo.trk == song.sel + 1u && track_sig(t) == undo.after &&
          fm1_ms - undo.t_ms < 1500u))
        tl_undo_take(t, SMK_TAPS);
    if (drum_track(t))
        grid_hit(t, i, ui.lane, 2);
    else if (step_on(st))
        step_clear(st);
    else {
        step_clear(st);
        rec_put(t, st, last_note, smk_vel(vel));
    }
    undo.after = track_sig(t);
    undo.t_ms = fm1_ms;
    if (!ui.home && step_page(cur_page()->graph))
        cursor_set((int32_t)i);
    ui.force = 1;
}

static void smk_pads(void)
{
    int32_t e;
    smk_pad_steps = (uint8_t)!smk_pads_keys();
    while ((e = smk_padq_take()) >= 0)
        if (e & 0xFF) {
            smk_tap_spoil();
            smk_pad_tap(((uint32_t)e >> 8) & 15u, (uint32_t)e & 0xFFu);
        }
}

/* ----------------------------------------------------------------------------------------------- faders --- */
static uint8_t smk_fbank;                            /* FADER BANK: CHANNEL_P's */
static struct { uint8_t tgt, last, picked; int16_t wrote; } smk_fd[4] = {{0xFF}, {0xFF}, {0xFF}, {0xFF}};
static uint32_t smk_fd_moved_ms;                     /* (#56) any fader's last move; 0: none since a track or bank change */
static uint8_t smk_bars_id __attribute__((section(".pool")));   /* (#56b) the bars' parameter: the bank's, or a knob's */
static void smk_faders(void)
{
    uint32_t top = trk_top(), j;
    if (fm1_in.frames < 300u)                        /* (the first readings settle) */
        return;
    for (j = 0; j < 4u; j++) {
        uint32_t k = top + j, id = CHANNEL_P[smk_fbank % 4u], v = fm1_in.fader[j], tgt = k | smk_fbank << 4;
        track_t *t = &trk[k % NTRK];
        int32_t cur = t->p[id] - TP[id].min;
        if (smk_fd[j].tgt != tgt) {                  /* another track or bank: wait for it to reach the value */
            smk_fd[j].tgt = (uint8_t)tgt;
            smk_fd[j].picked = 0;
            smk_fd[j].last = (uint8_t)v;
            smk_fd_moved_ms = 0;
            continue;
        }
        if (v == smk_fd[j].last)
            continue;
        smk_fd_moved_ms = fm1_ms | 1u;
        smk_bars_id = (uint8_t)id;
        popup_input(0, 0, 1);                        /* (POPUP CLOSE ON INPUT: a move, even short of the value, as a turn) */
        if (smk_fd[j].picked && t->p[id] != smk_fd[j].wrote)
            smk_fd[j].picked = 0;                    /* (set elsewhere since: reach it again) */
        if (!smk_fd[j].picked && !((smk_fd[j].last <= cur && (int32_t)v >= cur) ||
                                   (smk_fd[j].last >= cur && (int32_t)v <= cur))) {
            smk_fd[j].last = (uint8_t)v;
            continue;
        }
        smk_fd[j].picked = 1;
        smk_fd[j].last = (uint8_t)v;
        smk_tap_spoil();
        sound_set(t, &t->p[id], TP[id].min + (int32_t)v, UNDO_MIX);
        smk_fd[j].wrote = t->p[id];
        say_track_value(k, id, CHANNEL_N[smk_fbank % 4u]);
        ui.force = 1;
    }
}
/* (#56) HOME's bars (ui_home.c): up while any fader moved within SMK_FD_SHOW_MS, the bank's parameter; (#56b) or a
 * knob turned the selected track's LEVEL, PAN .. (ui_input.c track_value_turn), that parameter */
#define SMK_FD_SHOW_MS 500u
static int smk_fader_bars(uint32_t *id)
{
    if (!smk_fd_moved_ms || fm1_ms - smk_fd_moved_ms >= SMK_FD_SHOW_MS)
        return 0;
    *id = smk_bars_id;
    return 1;
}
static void smk_bars_show(uint32_t id)
{
    smk_fd_moved_ms = fm1_ms | 1u;
    smk_bars_id = (uint8_t)id;
}
/* track k has a fader: where it stands, 0..127, while the bars are its bank's (a knob's other parameter: left as is) */
static int smk_fader_place(uint32_t k, uint32_t *place)
{
    uint32_t j = k - trk_top();
    if (j >= 4u)
        return 0;
    if (smk_bars_id == CHANNEL_P[smk_fbank % 4u])
        *place = smk_fd[j].last;
    return 1;
}
static void smk_fader_bank_next(void)                /* (#39) SHIFT + FADER BANK (ui_input.c) */
{
    smk_fbank = (uint8_t)((smk_fbank + 1u) % 4u);
    ui_say("FADERS ", CHANNEL_N[smk_fbank]);
}

/* ------------------------------------------------------------------------------------- wheels, pedal --- */
static void smk_midi(uint32_t st, uint32_t ch, uint32_t d1, uint32_t d2)   /* as if it came in over USB-MIDI */
{
    fm1_irq_off();                                   /* (usb_poll fills the queue from the timer) */
    midi_enqueue((st >> 4) | (st | ch) << 8 | (d1 & 127u) << 16 | (d2 & 127u) << 24, 1);
    fm1_irq_on();
}
/* the wheels, self-calibrating (after genkigenki's layer: the pitch wheel's rest is its centre, taken at power-on and
 * followed while it rests; each range grows to what it reached; the travel is short, raw ~445..640 pitch, ~474..629 mod,
 * up = lower). Sent on the selected track's channel when they change: a bend, CC1; the track before gets them back to
 * rest when another is selected. The pedal (mux C 6, ~985 open): its rest at power-on, a press well away from it;
 * down, it holds the selected track's keys too (smk_pedal: seq.c kb_up) */
static void smk_wheels(void)
{
    enum { DEAD = 12, SPAN0 = 80, MOD_LO = 485, MOD_HI = 615 };
    static int32_t pc, plo, phi, mlo, mhi, rest, prest, sb = 0, sm = 0, sp = 0, sch = -1;
    int32_t p = fm1_in.wheel[0], m = fm1_in.wheel[1], d, b, mw, pd, ch = (int32_t)trk_midi_ch(song.sel);
    if (fm1_in.frames < 300u)
        return;
    if (!pc) {
        pc = p;
        plo = p - SPAN0;
        phi = p + SPAN0;
        mlo = m < MOD_LO ? m : MOD_LO;
        mhi = m > MOD_HI ? m : MOD_HI;
        prest = fm1_in.pedal;
    }
    plo = p < plo ? p : plo;
    phi = p > phi ? p : phi;
    mlo = m < mlo ? m : mlo;
    mhi = m > mhi ? m : mhi;
    d = p - pc;
    if (d > -DEAD && d < DEAD) {
        b = 0;
        if (++rest > 200) {                          /* resting: the centre follows a slow drift */
            pc += d > 0 ? 1 : d < 0 ? -1 : 0;
            rest = 0;
        }
    } else {
        rest = 0;
        b = d > 0 ? (d - DEAD) * 8191 / (phi - pc - DEAD > 1 ? phi - pc - DEAD : 1)
                  : (d + DEAD) * 8192 / (pc - plo - DEAD > 1 ? pc - plo - DEAD : 1);
        b = clamp(b, -8192, 8191);
    }
    mw = mhi - m <= DEAD ? 0 : clamp((mhi - m - DEAD) * 127 / (mhi - mlo - DEAD > 1 ? mhi - mlo - DEAD : 1), 0, 127);
    pd = sp ? ((int32_t)fm1_in.pedal - prest > 150 || prest - (int32_t)fm1_in.pedal > 150)
            : ((int32_t)fm1_in.pedal - prest > 250 || prest - (int32_t)fm1_in.pedal > 250);
    if (ch != sch) {                                 /* another track: the one before back to rest */
        if (sch >= 0) {
            if (sb)
                smk_midi(0xE0u, (uint32_t)sch, 0, 64);
            if (sm)
                smk_midi(0xB0u, (uint32_t)sch, 1, 0);
            if (sp)
                smk_midi(0xB0u, (uint32_t)sch, 64, 0);
        }
        sch = ch;
        sb = sm = sp = 0;
    }
    if (b != sb) {
        uint32_t v = (uint32_t)(b + 8192);
        smk_midi(0xE0u, (uint32_t)ch, v & 127u, v >> 7);
        sb = b;
    }
    if (mw != sm) {
        smk_midi(0xB0u, (uint32_t)ch, 1, (uint32_t)mw);
        sm = mw;
    }
    if (pd != sp) {
        smk_midi(0xB0u, (uint32_t)ch, 64, pd ? 127u : 0u);
        sp = pd;
    }
    smk_pedal = (uint8_t)pd;                         /* (the keys: seq.c kb_up) */
}

/* ------------------------------------------------------------------------------------------ < > , MASTER --- */
static void smk_buttons(void)
{
    static uint32_t prev;
    uint32_t now = fm1_in.buttons, pr = now & ~prev;
    prev = now;
    if (ui.menu || ui.confirm || name_on())
        return;
    if (ui.layer)
        pr &= layer_keeps_arrows(ui.layer) ? SMK_ARROWS : 0u;
    if (pr & (1u << SMK_BTN_LEFT | 1u << SMK_BTN_RIGHT))
        entry_step((pr >> SMK_BTN_RIGHT) & 1u ? 1 : -1);
}

static void smk_master_poll(void)                    /* K1: its level said while it moves, kept once it rests */
{
    static int32_t last = -1;
    static uint32_t moved;
    if (last < 0)
        last = smk_master;
    if (smk_master != last) {
        char b[8];
        int32_t pct = smk_master * 100 / 1023;
        b[0] = (char)(pct >= 100 ? '1' : ' ');
        b[1] = (char)('0' + pct / 10 % 10);
        b[2] = (char)('0' + pct % 10);
        b[3] = '%';
        b[4] = 0;
        ui_say("MASTER ", pct >= 100 ? b : pct >= 10 ? b + 1 : b + 2);
        last = smk_master;
        moved = fm1_ms | 1u;
    } else if (moved && fm1_ms - moved > 1500u) {
        moved = 0;
        SMK_MASTER_BYTE = (uint8_t)(smk_master / 4 + 1 > 255 ? 255 : smk_master / 4 + 1);
        settings_save();                             /* (queued: written while stopped) */
    }
}

#define MASTER_SAFE_POT 205u                         /* (MENU > AUDIO > MASTER START SAFE) K1's 20 % */
/* power-on, the settings read (persist_boot), before master_boot: MASTER where it was left */
static void smk_ui_boot(void)
{
    if (SMK_MASTER_BYTE)
        smk_master = ((int32_t)SMK_MASTER_BYTE - 1) * 4;
    if (MASTER_SAFE_SET && smk_master > (int32_t)MASTER_SAFE_POT)
        smk_master = MASTER_SAFE_POT;
    smk_kv_mode = (uint8_t)(SMK_KV_BYTE % SMK_KV_N);
}

/* main.c: each UI frame, after ui_input */
static void smk_ui_frame(void)
{
    smk_kv_mode = (uint8_t)(SMK_KV_BYTE % SMK_KV_N);
    smk_pads();
    smk_faders();
    smk_wheels();
    smk_buttons();
    smk_master_poll();
}
