/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* MASTER (#137): the pot read before the audio starts, so the first block plays at its level (it was 2048, about
 * 70 % of the knob, until the main loop's first read after the splash, then a 1/8-per-frame glide from mid-scale:
 * the keys, live from the audio ISR at once, sounded loud under the splash with the knob turned down).
 * master_knob: the pot x16, smoothed per UI frame (master_poll) */
static int32_t master_knob = 512 * 16;
/* MENU > AUDIO > MASTER START SAFE (master_rise): after power-on the level rises from silence over MASTER_RISE_MS, and
 * stays at most MASTER_SAFE_POT's until the pot is picked up: turned MASTER_CATCH steps up from where it stood (smoothed,
 * so no ADC noise gets that far) or down to that level (a turn down above it changes nothing: never louder for turning
 * down); then it glides to the pot. The SMK's K1 starts at that level */
#define MASTER_RISE_MS 1000u
#define MASTER_CATCH 32
enum { MASTER_FREE, MASTER_HOLD_FIRST, MASTER_HOLD };
static uint8_t master_rising, master_hold;
static uint32_t master_rise_t0;
static int32_t master_held_knob;
static uint32_t master_risen(uint32_t q12)
{
    uint32_t t = fm1_ms - master_rise_t0;
    if (master_rising && t < MASTER_RISE_MS)
        return q12 * t / MASTER_RISE_MS;
    master_rising = 0;
    return q12;
}
static int master_picked_up(void)
{
    return master_knob - master_held_knob >= MASTER_CATCH * 16 || master_knob / 16 <= (int32_t)MASTER_SAFE_POT;
}
static void master_poll(void)
{
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a >= 0) {
        if (master_hold == MASTER_HOLD_FIRST) {
            master_knob = master_held_knob = a * 16;
            master_hold = MASTER_HOLD;
        }
        master_knob += (a * 16 - master_knob) / 8;
        if (master_hold == MASTER_HOLD && master_picked_up()) {
            master_hold = MASTER_FREE;
            if (master_knob > (int32_t)MASTER_SAFE_POT * 16)
                master_knob = (int32_t)MASTER_SAFE_POT * 16;
        }
        song.master_q12 = master_risen(master_of_pot(master_hold ? MASTER_SAFE_POT : (uint32_t)(master_knob / 16)));
    }
}
/* power-on (main.c), before audio_init: the knob as it stands, no glide */
static void master_boot(void)
{
    uint32_t i;
    (void)fm1_adc_read(FM1_ADC_MASTER);                 /* (the first conversion after the pin setup: dropped) */
    for (i = 0; i < 3u; i++) {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            master_knob = a * 16;
            song.master_q12 = master_of_pot((uint32_t)a);
            return;
        }
    }                                                   /* (no reading: felucca_init's level, the loop glides on) */
}
/* power-on (main.c), after master_boot: silent, master_poll brings the level up and holds the pot from its first
 * reading (master_boot may have had none) */
static void master_rise(void)
{
    master_rising = 1;
    master_hold = MASTER_HOLD_FIRST;
    master_rise_t0 = fm1_ms;
    song.master_q12 = 0;
}
