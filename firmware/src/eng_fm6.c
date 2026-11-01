/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM6: classic 6-operator FM. The synthesis is msfa (Dexed's core), ported to integer C in fm6_core.c
 * (Apache-2.0); this file is the Felucca engine around it.
 *
 * The patch is the sound: every track has one (fm6_patch, the generic 155-byte single-voice layout: six
 * operators with their 4-rate / 4-level envelopes, keyboard level and rate scaling, velocity, ratio or
 * fixed frequency, detune; 32 algorithms, feedback, LFO, pitch envelope, transpose). It is edited in the
 * web editor (EDITOR_PROTOCOL.md FM6_*: a .syx voice sent to a track), loaded from the factory slots, and saved inside
 * projects (FUN8) and user presets (up_fm6.c). Whatever is loaded becomes the track's patch.
 * On the device the eight EDIT values are macros on top of it:
 *   ALG   PAT = the patch's algorithm, 1..32 another one
 *   FB    added to the patch's feedback (0..7)
 *   MLVL  the output level of every operator that is not a carrier (-36 .. +36 dB): the brightness
 *   MRAT  added to the coarse ratio of the modulators (ratio mode only)
 *   MEG   the modulators' envelope times: + slower (rates down to 40 steps), - faster
 *   VMOD  added to the modulators' velocity sensitivity (0..7)
 *   DTUN  spreads the carriers apart in pitch (up to about +-36 cents between the outer ones)
 *   SLOT  F1..F8 load a factory patch; OWN (8) is the track's own patch: what a project, a user preset or the
 *         editor put there. A load shows F n when the patch is that factory one unchanged, else OWN (fm6_adopt).
 *         Turning SLOT from OWN to F n keeps the own patch aside; back to OWN brings it back (fm6_poll). Editing
 *         F n on the device meanwhile keeps it aside too and makes F n* (fm6_edited): its edits stay when SLOT
 *         turns away, and come back with F n; one F n* a track, editing another drops it. Both in RAM only: a power
 *         cycle, a load, an undo or the editor's patch drops them.
 *         Before 1.0.3 the values 8..34 were F1..F8 then B1..B27, a flash bank (retired: its patches moved into
 *         the user presets, up_fm6.c); a stored 8..34 is clamped to 8, OWN, and its patch comes with the record
 * The operator envelopes are the voice's amplitude and end it (engine_t.ownenv / done): the track's ADSR,
 * ENV DEST and the matrix's ENV do nothing here. The track's FLT moves MLVL (ENV / LFO -> FLT, the
 * matrix's CUT), SHP the feedback, PIT the pitch (bend, glide, tune: the voice's pitch and fine factor).
 * Six voices per part at most (engine_t.poly).
 *
 * State: per voice an fm6_note_t (284 bytes, in the part's shared engine area as PHYS's slots: engines.c eng_mem;
 * voice_t.s[] is too small), per part the patch, the patch through the macros (fm6_eff, rebuilt in the audio ISR
 * when either changes) and the LFO (once a block). */
#include "fm6_core.c"
#include "felucca_fm6.h"         /* tools/gen_fm6_patches.py: FM6_INIT, FM6_FACTORY[] */

#define ENGI_FM6 12u             /* engines.c ENGINES[] (append-only) */
#define FM6_POLY 6               /* engine_t.poly */
#define FM6_OWN FM6_NFACTORY     /* SLOT: the track's own patch (F1..F8 before it) */
#define FM6_PACKED 128u

static uint8_t fm6_patch[NTRK][FP_SIZE + 1u];   /* the tracks' patches (main loop writes, then fm6_pgen) */
static volatile uint8_t fm6_pgen[NTRK];          /* +1 after each write of fm6_patch[t] */
static uint8_t fm6_slot[NTRK];                   /* the SLOT value last loaded (main loop); 0xFF = none */
static uint8_t fm6_own[NTRK][FM6_PACKED];        /* the own patch while SLOT shows a factory one (fm6_poll) */
static uint8_t fm6_own_ok;                       /* bit tr: fm6_own[tr] holds it */
static uint8_t fm6_edit[NTRK][FM6_PACKED];       /* (#30) F n* while SLOT is not F n (fm6_poll) */
static volatile uint8_t fm6_edit_f[NTRK];        /* n + 1: track tr has F n*, 0 none. A byte a track: the audio ISR
                                                  * clears one (launch.c esnd_put) */
static struct {                                  /* the patch through the macros: the audio ISR's copy */
    uint8_t p[FP_SIZE + 1u];
    uint8_t gen, alg, fb, ok;
    int16_t e[7];                                /* P_E0..P_E6 it was made with */
    int32_t dt[6];                               /* DTUN: per operator, Q24 log2 */
} fm6_eff[NTRK];
static fm6_lfo_t fm6_lfo[NTRK];
typedef struct {                                 /* (#20w) the last note a track played, for the editor's waterfall */
    uint8_t note, vel, voice, down;              /* (vel 0: none yet) */
    uint32_t on_ms, held_ms;                     /* (fm1_ms) held_ms: how long it was held; while down, the note before's */
} fm6_last_t;
static fm6_last_t fm6_last[NTRK] __attribute__((section(".pool")));   /* (the voices write it, the main loop reads) */
static int32_t fm6_lfo_v[NTRK], fm6_lfo_d[NTRK]; /* this block's LFO value and delay (Q24) */
/* (#72) each operator's phase, % of a cycle (FM6_ALG's order: the sixth first; shown as -50 .. +49): a fresh note
 * starts there, the sounding ones glide there ((#72b) fm6_phase_glide). (#76) Part of the sound, as WAVE: see fm6x_t */
static uint8_t fm6_op_phase[NTRK][6] __attribute__((section(".pool")));
/* (#72b) bumped by the main loop at a PHASE change; the ISR's copy once every sounding note has reached it */
static uint8_t fm6_phase_gen[NTRK] __attribute__((section(".pool")));
static uint8_t fm6_phase_seen[NTRK] __attribute__((section(".pool")));
/* (#73) each operator's waveform (FM6_W_*, FM6_ALG's order), as PHASE */
static uint8_t fm6_op_wave[NTRK][6] __attribute__((section(".pool")));

/* ------------------------------------------------------- patch formats --- */
/* the highest value of each byte of the 155-byte voice */
static uint32_t fm6_max(uint32_t i)
{
    static const uint8_t OPMAX[21] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 3, 3, 7, 3, 7, 99, 1, 31, 99, 14};
    static const uint8_t VMAX[19] = {99, 99, 99, 99, 99, 99, 99, 99, 31, 7, 1, 99, 99, 99, 99, 1, 5, 7, 48};
    if (i < 126u)
        return OPMAX[i % 21u];
    if (i < FP_NAME)
        return VMAX[i - 126u];
    return 126u;
}

/* every value inside its range (a name byte outside 32..126 becomes a space) */
static void fm6_sanitize(uint8_t *v)
{
    uint32_t i;
    for (i = 0; i < FP_SIZE; i++) {
        if (i >= FP_NAME)
            v[i] = v[i] < 32u || v[i] > 126u ? ' ' : v[i];
        else if (v[i] > fm6_max(i))
            v[i] = (uint8_t)fm6_max(i);
    }
    v[FP_SIZE] = 0;
}

/* 128-byte packed record (the 32-voice bank's) -> 155-byte voice; bits a record does not use are ignored */
static void fm6_unpack(const uint8_t *b, uint8_t *v)
{
    uint32_t k, i;
    for (k = 0; k < 6u; k++) {
        const uint8_t *o = b + k * 17u;
        uint8_t *d = v + k * FP_OP;
        for (i = 0; i < 11u; i++)
            d[i] = o[i] & 0x7Fu;
        d[FP_LC] = o[11] & 3u;
        d[FP_RC] = (o[11] >> 2) & 3u;
        d[FP_RS] = o[12] & 7u;
        d[FP_DET] = (o[12] >> 3) & 15u;
        d[FP_AMS] = o[13] & 3u;
        d[FP_KVS] = (o[13] >> 2) & 7u;
        d[FP_OL] = o[14] & 0x7Fu;
        d[FP_MODE] = o[15] & 1u;
        d[FP_FC] = (o[15] >> 1) & 31u;
        d[FP_FF] = o[16] & 0x7Fu;
    }
    for (i = 0; i < 9u; i++)
        v[FP_PR1 + i] = b[102 + i] & 0x7Fu;              /* pitch EG, algorithm */
    v[FP_ALG] &= 31u;
    v[FP_FB] = b[111] & 7u;
    v[FP_OKS] = (b[111] >> 3) & 1u;
    for (i = 0; i < 4u; i++)
        v[FP_LFS + i] = b[112 + i] & 0x7Fu;
    v[FP_LKS] = b[116] & 1u;
    v[FP_LFW] = (b[116] >> 1) & 7u;
    v[FP_LPMS] = (b[116] >> 4) & 7u;
    v[FP_TRNSP] = b[117] & 0x7Fu;
    for (i = 0; i < 10u; i++)
        v[FP_NAME + i] = b[118 + i] & 0x7Fu;
    fm6_sanitize(v);
}

/* 155-byte voice -> 128-byte packed record (7-bit bytes: it travels in SysEx as it is) */
static void fm6_pack(const uint8_t *v, uint8_t *b)
{
    uint32_t k, i;
    for (k = 0; k < 6u; k++) {
        const uint8_t *o = v + k * FP_OP;
        uint8_t *d = b + k * 17u;
        for (i = 0; i < 11u; i++)
            d[i] = o[i] & 0x7Fu;
        d[11] = (uint8_t)((o[FP_LC] & 3u) | (o[FP_RC] & 3u) << 2);
        d[12] = (uint8_t)((o[FP_RS] & 7u) | (o[FP_DET] & 15u) << 3);
        d[13] = (uint8_t)((o[FP_AMS] & 3u) | (o[FP_KVS] & 7u) << 2);
        d[14] = o[FP_OL] & 0x7Fu;
        d[15] = (uint8_t)((o[FP_MODE] & 1u) | (o[FP_FC] & 31u) << 1);
        d[16] = o[FP_FF] & 0x7Fu;
    }
    for (i = 0; i < 9u; i++)
        b[102 + i] = v[FP_PR1 + i] & 0x7Fu;
    b[110] &= 31u;
    b[111] = (uint8_t)((v[FP_FB] & 7u) | (v[FP_OKS] & 1u) << 3);
    for (i = 0; i < 4u; i++)
        b[112 + i] = v[FP_LFS + i] & 0x7Fu;
    b[116] = (uint8_t)((v[FP_LKS] & 1u) | (v[FP_LFW] & 7u) << 1 | (v[FP_LPMS] & 7u) << 4);
    b[117] = v[FP_TRNSP] & 0x7Fu;
    for (i = 0; i < 10u; i++)
        b[118 + i] = v[FP_NAME + i] & 0x7Fu;
}

/* ---------------------------------------------------- the track's patch --- */
/* track tr's patch = v (155 bytes, sanitized). Main loop: the ISR takes it at its next block */
static void fm6_set_patch(uint32_t tr, const uint8_t *v)
{
    uint8_t s[FP_SIZE + 1u];
    tr %= NTRK;
    memcpy(s, v, FP_SIZE);
    fm6_sanitize(s);                                /* not in place: fm6_sync must never copy an unsanitized byte */
    memcpy(fm6_patch[tr], s, FP_SIZE + 1u);
    RING_PUBLISH();
    fm6_pgen[tr]++;
}

/* (#73) operator k's waveform on track tr = w. Main loop: the ISR's next block plays it, the sounding notes too */
static void fm6_set_wave(uint32_t tr, uint32_t k, uint32_t w)
{
    tr %= NTRK;
    fm6_op_wave[tr][k % 6u] = (uint8_t)(w % FM6_NWAVE);
    RING_PUBLISH();
    fm6_pgen[tr]++;
}
static uint32_t fm6_waves_of(const uint8_t *w)
{
    uint32_t k, waves = 0;
    for (k = 0; k < 6u; k++)
        waves |= (uint32_t)(w[k] % FM6_NWAVE) << (4u * k);
    return waves;
}
/* (#72b) operator k's PHASE on track tr = pct %. Main loop: the sounding notes glide there from the ISR's next block */
static void fm6_set_phase(uint32_t tr, uint32_t k, uint32_t pct)
{
    tr %= NTRK;
    fm6_op_phase[tr][k % 6u] = (uint8_t)(pct % 100u);
    RING_PUBLISH();
    fm6_phase_gen[tr]++;
}

/* ------------------------------------------- (#76) the patch's extension --- */
/* A sound is its 155-byte patch and, beyond the DX7's voice, each operator's PHASE and WAVE (all 0: the patch as a DX7
 * plays it). The tracks' are fm6_op_phase and fm6_op_wave. A copy of a sound (an undo copy, the own patch and F n* kept
 * aside, a song entry's, a project's) carries them in the top bit of its first FM6X_BITS bytes, packed or not: no byte
 * there uses it (fm6_unpack drops it). So no copy grows, and a copy with any of them set never equals a factory patch.
 * Stored (the user sounds' records, the projects) they are a block of fields, FM6X_MARK below */
#define FM6X_BITS 66u                            /* 6 x (PHASE 7 bits, WAVE 4) */
typedef struct {
    uint8_t phase[6], wave[6];
} fm6x_t;

static void fm6x_hide(uint8_t *p, const fm6x_t *x)
{
    uint32_t k, j;
    for (k = 0; k < 6u; k++) {
        uint32_t bits = (x->phase[k] & 127u) | (x->wave[k] & 15u) << 7;
        for (j = 0; j < 11u; j++, p++)
            *p = (uint8_t)((*p & 127u) | ((bits >> j) & 1u) << 7);
    }
}
/* .. out of p (its bytes 7-bit again) into x */
static void fm6x_take(uint8_t *p, fm6x_t *x)
{
    uint32_t k, j;
    for (k = 0; k < 6u; k++) {
        uint32_t bits = 0;
        for (j = 0; j < 11u; j++, p++) {
            bits |= (uint32_t)(*p >> 7) << j;
            *p &= 127u;
        }
        x->phase[k] = (uint8_t)((bits & 127u) % 100u);
        x->wave[k] = (uint8_t)((bits >> 7) % FM6_NWAVE);
    }
}

static void fm6x_of(uint32_t tr, fm6x_t *x)
{
    memcpy(x->phase, fm6_op_phase[tr % NTRK], 6);
    memcpy(x->wave, fm6_op_wave[tr % NTRK], 6);
}
/* track tr's PHASE and WAVE = x: the sounding notes take them at the ISR's next block */
static void fm6x_set(uint32_t tr, const fm6x_t *x)
{
    tr %= NTRK;
    memcpy(fm6_op_phase[tr], x->phase, 6);
    memcpy(fm6_op_wave[tr], x->wave, 6);
    RING_PUBLISH();
    fm6_pgen[tr]++;
    fm6_phase_gen[tr]++;
}
/* (#72, #73) a sound load without them: every operator back to PHASE 0 and SINE */
static void fm6_osc_reset(uint32_t tr)
{
    static const fm6x_t none;
    fm6x_set(tr, &none);
}

/* Stored, they are the FM6X block, written in the free bytes after a record's own: FM6X_MARK, then the fields (one
 * sound's: their count n, then n bytes; a project's: the count of sounds, then each sound's), then a check byte; every
 * byte 0..127. The fields, append-only: flags (bit 0 ROUTE, #75; bit 1 LOOP,
 * #74), PHASE x 6, OSC x 6 (bits 0..3 WAVE), ROUTE x 6 (#75), FM6_ALG's order. n stops after the last one not 0: a
 * reader takes the fields it knows, the ones past its own passed over, the missing ones 0. All 0 (n 0 for every sound):
 * no block, the record as firmware before writes it. Firmware before reads none of those bytes */
#define FM6X_MARK 0x36u                          /* ('6') another meaning of a field: another mark */
#define FM6X_PHASE 1u
#define FM6X_OSC 7u
#define FM6X_N 19u                               /* the fields: up to ROUTE's (#75: read, none of them used yet) */

static uint32_t fm6x_chk(const uint8_t *b, uint32_t n)
{
    uint32_t sum = 0;
    while (n--)
        sum += *b++;
    return (sum & 127u) ^ 0x2Au;
}
static uint32_t fm6x_field(const fm6x_t *x, uint32_t i)
{
    if (i >= FM6X_OSC + 6u)
        return 0;
    if (i >= FM6X_OSC)
        return x->wave[i - FM6X_OSC];
    return i >= FM6X_PHASE ? x->phase[i - FM6X_PHASE] : 0u;
}
static uint32_t fm6x_count(const fm6x_t *x)
{
    uint32_t n = FM6X_N;
    while (n && !fm6x_field(x, n - 1u))
        n--;
    return n;
}
/* one sound's fields at b: their bytes */
static uint32_t fm6x_put(uint8_t *b, const fm6x_t *x)
{
    uint32_t n = fm6x_count(x), i;
    b[0] = (uint8_t)n;
    for (i = 0; i < n; i++)
        b[1u + i] = (uint8_t)fm6x_field(x, i);
    return 1u + n;
}
/* .. read into x (0 none: they pass room) */
static uint32_t fm6x_get(const uint8_t *b, uint32_t room, fm6x_t *x)
{
    uint32_t n, i;
    memset(x, 0, sizeof *x);
    if (!room || (n = b[0]) >= room)
        return 0;
    for (i = FM6X_PHASE; i < n && i < FM6X_OSC; i++)
        x->phase[i - FM6X_PHASE] = (uint8_t)(b[1u + i] > 99u ? 99u : b[1u + i]);
    for (i = FM6X_OSC; i < n && i < FM6X_OSC + 6u; i++)
        x->wave[i - FM6X_OSC] = (uint8_t)((b[1u + i] & 15u) < FM6_NWAVE ? b[1u + i] & 15u : FM6_W_SINE);
    return 1u + n;
}
/* the block of nx sounds x at b (room bytes): its length, 0 when they are all 0 (or it would not fit: none) */
static uint32_t fm6x_block_put(uint8_t *b, uint32_t room, const fm6x_t *x, uint32_t nx)
{
    uint32_t len = 1u, i;
    for (i = 0; i < nx && !fm6x_count(&x[i]); i++)
        ;
    if (i == nx || room < 2u + nx * (1u + FM6X_N) + (nx > 1u))
        return 0;
    b[0] = FM6X_MARK;
    if (nx > 1u)
        b[len++] = (uint8_t)nx;
    for (i = 0; i < nx; i++)
        len += fm6x_put(b + len, &x[i]);
    b[len] = (uint8_t)fm6x_chk(b, len);
    return len + 1u;
}
/* .. read: x as stored, 0 for every sound when there is none (no mark, a bad check, past room) */
static void fm6x_block_get(const uint8_t *b, uint32_t room, fm6x_t *x, uint32_t nx)
{
    uint32_t len = 1u, i, n, stored = 1u;
    fm6x_t skip;
    memset(x, 0, nx * sizeof *x);
    if (room < 3u || b[0] != FM6X_MARK)
        return;
    if (nx > 1u)
        stored = b[len++];
    for (i = 0; i < stored; i++, len += n)
        if (!(n = fm6x_get(b + len, room - len, i < nx ? &x[i] : &skip)) || len + n >= room)
            break;
    if (i < stored || b[len] != fm6x_chk(b, len))
        memset(x, 0, nx * sizeof *x);
}

/* track tr's sound -> v (155 bytes + 1) */
static void fm6_sound_of(uint32_t tr, uint8_t *v)
{
    fm6x_t x;
    memcpy(v, fm6_patch[tr % NTRK], FP_SIZE + 1u);
    fm6x_of(tr, &x);
    fm6x_hide(v, &x);
}
/* track tr's sound = v (as fm6_sound_of made it; a bare patch: PHASE 0, SINE) */
static void fm6_set_sound(uint32_t tr, const uint8_t *v)
{
    uint8_t p[FP_SIZE + 1u];
    fm6x_t x;
    memcpy(p, v, sizeof p);
    fm6x_take(p, &x);
    fm6_set_patch(tr, p);
    fm6x_set(tr, &x);
}
/* .. packed (128 bytes) */
static void fm6_pack_sound(uint32_t tr, uint8_t *b)
{
    fm6x_t x;
    fm6_pack(fm6_patch[tr % NTRK], b);
    fm6x_of(tr, &x);
    fm6x_hide(b, &x);
}
static void fm6_set_packed(uint32_t tr, const uint8_t *b)
{
    uint8_t v[FP_SIZE + 1u];
    uint32_t i;
    fm6_unpack(b, v);
    for (i = 0; i < FM6X_BITS; i++)
        v[i] |= b[i] & 128u;
    fm6_set_sound(tr, v);
}

/* SLOT value s < FM6_NFACTORY: that factory patch into track tr (F n*: its edits) */
static void fm6_load_slot(uint32_t tr, uint32_t s)
{
    tr %= NTRK;
    s %= FM6_NFACTORY;
    fm6_set_packed(tr, fm6_edit_f[tr] == s + 1u ? fm6_edit[tr] : FM6_FACTORY[s]);
    fm6_slot[tr] = (uint8_t)s;
}

/* the SLOT sound p (fm6_sound_of) shows: the factory patch it equals (F n; looked for from SLOT s on), else OWN */
static int32_t fm6_match(const uint8_t *p, int32_t s)
{
    uint8_t v[FP_SIZE + 1u];
    uint32_t k, f = s >= 0 && s < (int32_t)FM6_NFACTORY ? (uint32_t)s : 0u;
    for (k = 0; k < FM6_NFACTORY; k++) {
        fm6_unpack(FM6_FACTORY[(f + k) % FM6_NFACTORY], v);
        if (!memcmp(v, p, FP_SIZE))
            return (int32_t)((f + k) % FM6_NFACTORY);
    }
    return FM6_OWN;
}
/* an FM6 track's SLOT shows the factory patch track tr's patch equals unchanged (F n; the SLOT it had first), else
 * OWN; nothing reloads over it (fm6_poll) */
static void fm6_reslot(uint32_t tr)
{
    track_t *t = &trk[tr % NTRK];
    int32_t s = t->p[P_E7];
    tr %= NTRK;
    if (t->eng_req == ENGI_FM6) {
        uint8_t v[FP_SIZE + 1u];
        fm6_sound_of(tr, v);
        t->p[P_E7] = (int16_t)(s = fm6_match(v, s));
    }
    fm6_slot[tr] = (uint8_t)s;
}

/* track tr's patch was just put in as its own (a project, a user preset, the editor, a conversion): fm6_reslot, the
 * own patch kept aside and F n* dropped */
static void fm6_adopt(uint32_t tr)
{
    tr %= NTRK;
    fm6_own_ok &= (uint8_t)~(1u << tr);
    fm6_edit_f[tr] = 0;
    fm6_reslot(tr);
}

/* (#30) track tr's patch edited on the device. While an own patch is kept aside (SLOT F n, turned from OWN) SLOT stays
 * F n and the patch is F n*; returns the F m* that drops for it (m + 1), 0 none. Else fm6_reslot */
static uint32_t fm6_edited(uint32_t tr)
{
    uint32_t was;
    tr %= NTRK;
    if (!(fm6_own_ok & (1u << tr))) {
        fm6_reslot(tr);
        return 0;
    }
    was = fm6_edit_f[tr];
    fm6_edit_f[tr] = (uint8_t)(fm6_slot[tr] + 1u);
    return was == fm6_edit_f[tr] ? 0u : was;
}

/* a factory sound load put a SLOT value in (a preset, an engine change): its patch (OWN: the patch stays) */
static void fm6_track_loaded(const track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK && t->eng_req == ENGI_FM6) {
        fm6_osc_reset(tr);
        fm6_own_ok &= (uint8_t)~(1u << tr);
        fm6_edit_f[tr] = 0;
        if (t->p[P_E7] >= 0 && t->p[P_E7] < (int32_t)FM6_NFACTORY)
            fm6_load_slot(tr, (uint32_t)t->p[P_E7]);
        else
            fm6_adopt(tr);
    }
}

/* power-on: every track the init voice (what a project stores for the tracks that never played FM6) */
static void fm6_init(void)
{
    uint8_t v[FP_SIZE + 1u];
    uint32_t tr;
    fm6_unpack(FM6_INIT, v);
    for (tr = 0; tr < NTRK; tr++) {
        fm6_osc_reset(tr);
        fm6_set_patch(tr, v);
        fm6_slot[tr] = 0xFFu;
    }
}

/* main loop: SLOT turned (a knob, the editor, MIDI, motion) -> that factory patch; from OWN the own patch is kept
 * aside (fm6_own), and turning back to OWN brings it back; from F n* its edits are kept aside (fm6_edit) */
static void fm6_poll(void)
{
    uint32_t tr, bit;
    for (tr = 0; tr < NTRK; tr++) {
        int32_t s = trk[tr].p[P_E7];
        if (trk[tr].eng_req != ENGI_FM6 || s == fm6_slot[tr])
            continue;
        bit = 1u << tr;
        if (fm6_edit_f[tr] == fm6_slot[tr] + 1u)
            fm6_pack_sound(tr, fm6_edit[tr]);
        if (s < 0 || s >= (int32_t)FM6_NFACTORY) {           /* OWN */
            if (fm6_own_ok & bit)
                fm6_set_packed(tr, fm6_own[tr]);
            fm6_own_ok &= (uint8_t)~bit;
            fm6_slot[tr] = FM6_OWN;
            if (s != FM6_OWN)
                trk[tr].p[P_E7] = FM6_OWN;
            continue;
        }
        if (fm6_slot[tr] == FM6_OWN && !(fm6_own_ok & bit)) {
            fm6_pack_sound(tr, fm6_own[tr]);
            fm6_own_ok |= (uint8_t)bit;
        }
        fm6_load_slot(tr, (uint32_t)s);
    }
}

/* -------------------------------------------------------------- macros --- */
/* RATIO, MOD ENV and VEL MOD (e: P_E0..) on the modulators of patch p in algorithm alg (0..31), as they play: the
 * audio ISR's copy (fm6_sync) and (FM6 LAB) the editor's graph (ui_fm6.c) */
static void fm6_macro(uint8_t *p, uint32_t alg, const int16_t *e)
{
    uint32_t car = fm6_carriers(alg), k, j;
    int32_t meg = clamp(e[4], -64, 63) * 40 / 64;           /* MEG: rate steps (+ slower) */
    for (k = 0; k < 6u; k++) {
        uint8_t *op = p + k * FP_OP;
        if ((car >> k) & 1u)
            continue;
        if (!op[FP_MODE])
            op[FP_FC] = (uint8_t)clamp(op[FP_FC] + e[3], 0, 31);
        for (j = 0; j < 4u; j++)
            op[FP_R1 + j] = (uint8_t)clamp(op[FP_R1 + j] - meg, 0, 99);
        op[FP_KVS] = (uint8_t)clamp(op[FP_KVS] + e[5], 0, 7);
    }
}

/* DTUN (0..127) on the carriers of algorithm alg, dt[k] per operator (Q24 log2): OP1 first, the carriers 0, +1, -1, +2,
 * -2, +3 steps apart, up to 12 cents a step (1 cent = 13981); the modulators 0. The audio ISR's copy (fm6_sync) and
 * (#20w) the editor's waterfall (ui_fm6.c) */
static void fm6_spread(uint32_t alg, int32_t dtun, int32_t *dt)
{
    static const int8_t SPREAD[6] = {0, 1, -1, 2, -2, 3};
    uint32_t car = fm6_carriers(alg), n = 0, k;
    int32_t step = clamp(dtun, 0, 127) * (12 * 13981) / 127;
    for (k = 0; k < 6u; k++) {
        uint32_t ki = 5u - k;
        dt[ki] = (car >> ki & 1u) ? SPREAD[n++] * step : 0;
    }
}

/* the patch through E0..E6 (audio ISR; cheap when nothing changed). 1: it changed */
static int fm6_sync(const track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk), k;
    const int16_t *e = &t->p[P_E0];
    if (tr >= NTRK)
        return 0;
    if (fm6_eff[tr].ok && fm6_eff[tr].gen == fm6_pgen[tr]) {
        for (k = 0; k < 7u && fm6_eff[tr].e[k] == e[k]; k++)
            ;
        if (k == 7u)
            return 0;
    }
    fm6_eff[tr].gen = fm6_pgen[tr];
    memcpy(fm6_eff[tr].p, fm6_patch[tr], FP_SIZE);
    for (k = 0; k < 7u; k++)
        fm6_eff[tr].e[k] = e[k];
    {
        uint8_t *p = fm6_eff[tr].p;
        uint32_t alg = e[0] >= 1 && e[0] <= 32 ? (uint32_t)e[0] - 1u : p[FP_ALG] & 31u;
        fm6_eff[tr].alg = (uint8_t)alg;
        fm6_eff[tr].fb = (uint8_t)clamp(p[FP_FB] + e[1], 0, 7);
        fm6_spread(alg, e[6], fm6_eff[tr].dt);
        fm6_macro(p, alg, e);
        fm6_lfo_reset(&fm6_lfo[tr], p);
    }
    fm6_eff[tr].ok = 1;
    return 1;
}

/* --------------------------------------------------------------- voice --- */
static fm6_note_t *fm6_note_of(track_t *t, voice_t *v)
{
    uint32_t i;
    if (t < &trk[0] || t >= &trk[NPART])
        return 0;
    i = (uint32_t)(v - t->v);
    return i < FM6_POLY ? &((fm6_note_t *)eng_part(t))[i] : 0;   /* (Skiff: the part's shared area, engines.c) */
}

static void fm6_note_on(track_t *t, voice_t *v)
{
    fm6_note_t *n = fm6_note_of(t, v);
    uint32_t tr = (uint32_t)(t - trk);
    const uint8_t *p;
    if (!n)
        return;
    (void)fm6_sync(t);
    p = fm6_eff[tr].p;
    fm6_note_init(n, p, (int32_t)v->note + p[FP_TRNSP] - 24, v->mvel ? v->mvel : v->vel,
                  (!v->env && !v->env_out) || !n->live, fm6_op_phase[tr]);   /* from silence */
    n->waves = fm6_waves_of(fm6_op_wave[tr]);
    fm6_lfo_key(&fm6_lfo[tr]);
    if (tr < NTRK)
        fm6_last[tr] = (fm6_last_t){v->note, (uint8_t)(v->mvel ? v->mvel : v->vel), (uint8_t)(v - t->v), 1, fm1_ms,
                                    fm6_last[tr].held_ms};
}

/* (out of line: inlined into fm6_render it moved the render's loops, +68 % on the ISR budget's count) */
static __attribute__((noinline)) void fm6_key_up(uint32_t tr, uint32_t voice)
{
    uint32_t held;
    if (tr >= NTRK || fm6_last[tr].voice != voice || !fm6_last[tr].down)
        return;
    held = fm1_ms - fm6_last[tr].on_ms;
    fm6_last[tr].held_ms = held ? held : 1u;
    fm6_last[tr].down = 0;
}

/* (#72b) a step a block at most: half a cycle, the farthest PHASE can be, in 24 blocks (17 ms), ~29 Hz above or below
 * the operator's pitch meanwhile */
#define FM6_GLIDE_STEP ((65536 + 47) / 48)

/* (#72b) each sounding note's operators a step closer to PHASE pct, the shorter way round. The step is the block's
 * slide, which fm6_note_compute adds to the operator's frequency: spread over the block's samples, a brief tiny bend,
 * never a jump. 1: one is still on its way */
static __attribute__((noinline)) int fm6_phase_glide(track_t *t, const uint8_t *pct)
{
    uint16_t target[6];
    uint32_t i, k;
    int gliding = 0;
    for (k = 0; k < 6u; k++)
        target[k] = fm6_phase_q16(pct[k]);
    for (i = 0; i < FM6_POLY; i++) {
        fm6_note_t *n = fm6_note_of(t, &t->v[i]);
        if (!n || !n->live)
            continue;
        for (k = 0; k < 6u; k++) {
            int32_t step = clamp((int16_t)(uint16_t)(target[k] - n->phase_at[k]), -FM6_GLIDE_STEP, FM6_GLIDE_STEP);
            n->phase_at[k] = (uint16_t)(n->phase_at[k] + step);
            n->slide[k] = step << (8 - FM6_LG_N);
            gliding |= step != 0;
        }
    }
    return gliding;
}

static void fm6_block(track_t *t)       /* once a block and part: the macros, the LFO, the PHASE glide */
{
    uint32_t tr = (uint32_t)(t - trk), i;
    uint8_t phase_gen;
    if (tr >= NTRK)
        return;
    phase_gen = fm6_phase_gen[tr];
    if (phase_gen != fm6_phase_seen[tr] && !fm6_phase_glide(t, fm6_op_phase[tr]))
        fm6_phase_seen[tr] = phase_gen;
    if (fm6_sync(t)) {
        uint32_t waves = fm6_waves_of(fm6_op_wave[tr]);
        for (i = 0; i < FM6_POLY; i++) {        /* (#69) the sounding notes follow the edit ((#73) a waveform too) */
            voice_t *v = &t->v[i];
            fm6_note_t *n = fm6_note_of(t, v);
            const uint8_t *p = fm6_eff[tr].p;
            if (n && n->live) {
                fm6_note_retune(n, p, (int32_t)v->note + p[FP_TRNSP] - 24, v->mvel ? v->mvel : v->vel);
                n->waves = waves;
            }
        }
    }
    fm6_lfo_v[tr] = fm6_lfo_sample(&fm6_lfo[tr]);
    fm6_lfo_d[tr] = fm6_lfo_delay(&fm6_lfo[tr]);
}

/* every carrier has gone silent for good: the voice ends (voice.c, engine_t.done) */
static int fm6_done(track_t *t, voice_t *v)
{
    fm6_note_t *n = fm6_note_of(t, v);
    uint32_t tr = (uint32_t)(t - trk);
    if (!n || !n->live)
        return 1;
    if (!fm6_note_done(n, fm6_eff[tr].p, fm6_eff[tr].alg))
        return 0;
    n->live = 0;
    return 1;
}

static void fm6_render(track_t *t, voice_t *v, int32_t *out, uint32_t len, const vmod_t *m)
{
    fm6_note_t *n = fm6_note_of(t, v);
    uint32_t tr = (uint32_t)(t - trk), i;
    const uint8_t *p;
    int32_t bus[FM6_N], lf, fb, lvl, k, a, da;
    if (!n || !n->live || len != FM6_N)
        return;
    p = fm6_eff[tr].p;
    if (!v->gate && n->down) {
        fm6_note_key(n, p, 0);
        fm6_key_up(tr, (uint32_t)(v - t->v));
    }
    lf = fm6_note_logfreq(m->pitch16 + (p[FP_TRNSP] - 24) * 16, m->fine);
    fb = clamp(fm6_eff[tr].fb + ((m->shape - (64 << 8)) >> 11), 0, 7);
    lvl = clamp(t->p[P_E2], -64, 63) * 24 + clamp(m->cutoff >> 8, -150, 150) * 16;   /* microsteps */
    if (!fm6_note_compute(n, p, bus, fm6_lfo_v[tr], fm6_lfo_d[tr], lf, fm6_eff[tr].alg, fb, fm6_eff[tr].dt, lvl << 16))
        return;                                         /* every carrier below the threshold */
    /* the voice's amplitude ramp x VOICE_FS (one carrier at full: VOICE_FS / 2), six voices in unison ~ one */
    k = t->p[P_VOICE] == V_UNISON ? VOICE_FS * 2 / 5 : VOICE_FS;
    a = mulq15(m->amp0, k);
    da = (mulq15(m->amp1, k) - a) >> FM6_LG_N;
    for (i = 0; i < FM6_N; i++) {
        int32_t x = bus[i] - n->dc;
        n->dc += x >> 10;                                 /* a one-pole high-pass, ~7 Hz: FM puts sidebands at 0 Hz */
        a += da;
        out[i] += (int32_t)(((int64_t)x * a) >> 26);
    }
}

/* --------------------------------------------------------- the engine --- */
static const char *const N_FM6_ALG[] = {"PAT", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
                                        "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25",
                                        "26", "27", "28", "29", "30", "31", "32", 0};
static const char *const N_FM6_PATCH[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "OWN", 0};
_Static_assert(NELEM(N_FM6_PATCH) == FM6_OWN + 2u, "a SLOT name per value");

/* {ALG, FB, MLVL, MRAT, MEG, VMOD, DTUN, SLOT}: the factory patch F1..F8 as it is, DTUN on the pad */
static const preset_t FM6_PRESETS[] = {
    {"TINE EP", {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 45, 25, 35), TAG(PT_KEYS)},
    {"GLASS BELL", {0, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 127, 0}, 0, 0, FX(0, 10, 30, 70), TAG(PT_KEYS)},
    {"FM BASS", {0, 0, 0, 0, 0, 0, 0, 2}, {0, 0, 127, 0}, 0, 1, FX(0, 0, 10, 10), TAG(PT_BASS)},
    {"FM BRASS", {0, 0, 0, 0, 0, 0, 0, 3}, {0, 0, 127, 0}, 0, 0, FX(0, 25, 20, 40), TAG(PT_LEAD)},
    {"PAD", {0, 0, 0, 0, 0, 0, 30, 4}, {0, 0, 127, 0}, 0, 0, FX(0, 60, 30, 70), TAG(PT_PAD)},
    {"FM MARIMBA", {0, 0, 0, 0, 0, 0, 0, 5}, {0, 0, 127, 0}, 0, 0, FX(0, 0, 25, 40), TAG(PT_KEYS)},
    {"FM ORGAN", {0, 0, 0, 0, 0, 0, 0, 6}, {0, 0, 127, 0}, 0, 0, FX(10, 40, 0, 30), TAG(PT_KEYS)},
    {"NYLON PICK", {0, 0, 0, 0, 0, 0, 0, 7}, {0, 0, 127, 0}, 0, 0, FX(0, 20, 35, 30), TAG(PT_PLUCK)},
    /* (Skiff) for the themes: CRYPT (dungeon synth), NEON (synthwave), QUARK (particles) */
    {"GRAVE BELL", {0, 0, 6, 0, 20, 0, 30, 1}, {0, 0, 127, 0}, 0, 0, FX(0, 10, 15, 100), TAG(PT_KEYS)},
    {"GLASS KEYS", {0, 0, 8, 0, 0, 0, 24, 0}, {0, 0, 127, 0}, 0, 0, FX(0, 75, 30, 45), TAG(PT_KEYS)},
    {"ISOTOPE", {0, 0, 14, 2, -10, 0, 50, 1}, {0, 0, 127, 0}, 0, 0, FX(0, 20, 50, 70), TAG(PT_KEYS)},
    {"DARK MATTER", {0, 4, 16, 0, 10, 0, 0, 2}, {0, 0, 127, 0}, 0, 1, FX(8, 0, 0, 10), TAG(PT_BASS)},
    /* (Skiff phase 9) more for every engine: basses, kits */
    {"PICK BASS", {0, 2, 12, 0, 16, 2, 0, 7}, {0, 0, 127, 0}, 0, 1, FX(0, 0, 5, 10), TAG(PT_BASS)},
    {"GROWL BASS", {0, 5, 8, 0, 20, 0, 8, 3}, {0, 0, 127, 0}, 20, 1, FX(30, 0, 5, 5), MORE({P_LD_FLT, 36}, {P_LRATE, 80}, {P_DTYPE, 2}), TAG(PT_BASS)},
};

static const engine_t ENG_FM6 = {
    .name = "FM6",
    .page_title = {"OPS", "PATCH"},
    .edit = {
        {"ALGO", N_FM6_ALG, 0, 32, 0, F_INT, 0},
        {"FEEDBACK", 0, -7, 7, 0, F_INT, 0},
        {"MOD LVL", 0, -64, 63, 0, F_BIPCT, 0},
        {"RATIO", 0, -16, 16, 0, F_INT, 0},
        {"MOD ENV", 0, -64, 63, 0, F_BIPCT, 0},
        {"VEL MOD", 0, -7, 7, 0, F_INT, 0},
        {"DETUNE", 0, 0, 127, 0, F_PCT, 0},
        {"PATCH", N_FM6_PATCH, 0, FM6_OWN, 0, F_INT, 0},
    },
    .presets = FM6_PRESETS,
    .npresets = NELEM(FM6_PRESETS),
    .note_on = fm6_note_on,
    .render = fm6_render,
    .knob = {P_E2, P_E3, P_E4, P_E7},
    .poly = FM6_POLY,
    .block = fm6_block,
    .ownenv = 1,
    .done = fm6_done,
};
