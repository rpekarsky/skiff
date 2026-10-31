/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* MASTER (#137): the pot read before the audio starts, so the first block plays at its level (it was 2048, about
 * 70 % of the knob, until the main loop's first read after the splash, then a 1/8-per-frame glide from mid-scale:
 * the keys, live from the audio ISR at once, sounded loud under the splash with the knob turned down).
 * master_knob: the pot x16, smoothed per UI frame (master_poll) */
static int32_t master_knob = 512 * 16;
/* (SMK-37) MENU > AUDIO > MASTER START SAFE (master_rise): after power-on the level rises from silence to K1's over
 * MASTER_RISE_MS; smk_ui_boot has put K1 at most MASTER_SAFE_POT */
#define MASTER_RISE_MS 1000u
static uint8_t master_rising;
static uint32_t master_rise_t0;
static uint32_t master_risen(uint32_t q12)
{
    uint32_t t = fm1_ms - master_rise_t0;
    if (master_rising && t < MASTER_RISE_MS)
        return q12 * t / MASTER_RISE_MS;
    master_rising = 0;
    return q12;
}
static void master_poll(void)
{
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a >= 0) {
        master_knob += (a * 16 - master_knob) / 8;
        song.master_q12 = master_risen(master_of_pot((uint32_t)(master_knob / 16)));
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
/* power-on (main.c), after master_boot: silent, master_poll brings the level up */
static void master_rise(void)
{
    master_rising = 1;
    master_rise_t0 = fm1_ms;
    song.master_q12 = 0;
}
