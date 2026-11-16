/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PMD 4.7g, the music driver of M. Kajihara for the PC-98, as its build for the YM2203 (OPN:
 * the PC-9801-26K board) plays: an optional module (LRVNE_PMD=1, mk/lrvne.mk).
 *
 * What PMD.COM does, register write for register write, reimplemented: the part interpreter of
 * parts A-I (FM 1-3, the FM3 parts D-F on the slots of channel 3, SSG G-I), the FM effect part
 * (an effect bank, the .EFC file), PMD's built-in SSG effects, the fade, the two timers of the
 * chip that clock it all, and the INT 60h functions an engine calls. docs/pmd.md says what is
 * covered and how it is checked against the real driver; addresses in comments are PMD.COM's
 * CS offsets.
 *
 * The driver writes registers through a callback and counts output samples: it is a
 * frame-driven sequencer for the write queue (lrvne/opnq.h, lrv_pmd_seq; writes with
 * lrv_opnq_write). The engine keeps the music buffer (a song file is copied into it, as the
 * original engines did into PMD's) and, when it uses them, the effect bank and the SSG-effect
 * table (tools/lrvne/pmd.py takes it out of the player's PMD.COM). The driver reads and writes
 * these in place and keeps pointers to them: they must stay where they are while it plays.
 *
 * Calls into the driver from the game are made under the audio lock (lrvne/mixer.h). */
#ifndef LRVNE_PMD_H
#define LRVNE_PMD_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/opn.h"
#include "lrvne/opnq.h"

#define LRV_PMD_PARTS       9       /* A B C D E F G H I */
#define LRV_PMD_PART_X      9       /* the FM effect part (work 3594) */
#define LRV_PMD_NPARTS      10

struct lrv_pmd_lfo {
    uint8_t w[8];        /* delay_ctr, speed_ctr, step, times_ctr, delay, speed, step0, times */
    uint8_t md[3];       /* delta, speed ctr, speed */
    uint8_t mdc[2];      /* count, reload */
    uint8_t wave;
    int16_t val;         /* current LFO value */
};

struct lrv_pmd_part {
    uint8_t kind_fm;     /* 1 = FM, 0 = SSG */
    uint8_t ch;          /* 1..3 */
    uint8_t is_x;        /* effect part */
    uint16_t addr, loop; /* stream pointer, L point (0 = none) */
    uint8_t len, gate;
    uint16_t freq;
    int16_t detune, porta, pstep, prem;
    uint8_t vol, trans, flags, tmpvol, ext, mix, voice, lstat, car, slot, vmask, mask, kst;
    uint8_t tl[4];
    uint8_t lslot[2];
    uint8_t q, Q;
    uint8_t note;
    uint8_t kd, kdc, kdm;
    uint8_t fbalg;
    uint8_t e[13];       /* envelope fields +21..+2D */
    struct lrv_pmd_lfo lfo[2];
};

typedef void (*lrv_pmd_write_fn)(void *ctx, uint8_t reg, uint8_t val);

struct lrv_pmd_ssgfx {
    int active;
    int priority;
    int number;
    const uint8_t *frame;    /* current frame in the table blob */
    int count;
    uint16_t tone;
    int8_t tone_delta;
    uint8_t noise;
    uint8_t sweep;           /* high nibble delta, low nibble interval */
    int sweep_ctr;
    uint8_t mixer_bits;      /* bit0 tone off, bit3 noise off */
};

struct lrv_pmd {
    lrv_pmd_write_fn write;
    void *ctx;

    /* music data (mutable: loop counters and F9 live in the data) */
    uint8_t *m;
    uint32_t msize;
    uint16_t voicetab;
    int has_voices;
    struct lrv_pmd_part part[LRV_PMD_NPARTS];

    /* globals (work area 30A0..) */
    uint8_t ks[3];           /* 30A5.. key-on slot bits per FM channel */
    uint8_t reg27;           /* 30AD */
    uint8_t reg27_saved;     /* 30AE */
    uint16_t sd[4];          /* slot detunes */
    uint8_t sd_any, fm3need, fb3, restore, tie, tvc;
    uint8_t TB, t, TBc;
    uint8_t status1, status2;
    uint8_t fade, fadesp;
    uint8_t voldown[2];
    uint8_t noise, noisec, reg7;
    uint8_t na;
    uint16_t rnd;
    uint8_t a3119, a30B7;
    uint8_t allst;
    int playing;
    int stop_after_fade;     /* 0 with the resident driver's /F option */

    /* FM effect (EFC) */
    uint8_t *efc;
    uint32_t efcsize;
    uint16_t efc_voicetab;
    int fm_eff_active;
    int fm_eff_number;

    /* built-in SSG effects */
    const uint8_t *ssgtab;   /* bytes of PMD.COM 2CE9h.. (entries + frames) */
    uint32_t ssgtab_size;
    struct lrv_pmd_ssgfx sfx;

    /* OPN timers (master clocks) */
    uint64_t clock;          /* current time */
    uint64_t ta_next, tb_next;
    int ta_on, tb_on;
    uint8_t reg24, reg25, reg26;
    uint8_t status;          /* timer flags */
    uint32_t cps_int;        /* master clocks per output sample, 16.16 fixed */
    uint32_t cps_frac;
    uint32_t cps_acc;
};

/* The driver as installed, counting time in output samples at out_rate (0 = 44100); writes
 * the installer's register initialisation through `write`. */
void lrv_pmd_init(struct lrv_pmd *p, lrv_pmd_write_fn write, void *ctx, uint32_t out_rate);
/* The effect bank (mutable: loop counters live in it) and the SSG-effect table. */
void lrv_pmd_set_efc(struct lrv_pmd *p, uint8_t *efc, uint32_t size);
void lrv_pmd_set_ssgtab(struct lrv_pmd *p, const uint8_t *tab, uint32_t size);
/* Another output rate from now on. */
void lrv_pmd_set_rate(struct lrv_pmd *p, uint32_t out_rate);

/* The INT 60h functions. The music buffer holds the song from its data on (file byte 1; byte
 * 0, the sound-source type, is not passed); it is mutable (loop counters live in it). */
void lrv_pmd_load_music(struct lrv_pmd *p, uint8_t *mdata, uint32_t size);
void lrv_pmd_mstart(struct lrv_pmd *p);                                        /* AH=00h */
void lrv_pmd_mstop(struct lrv_pmd *p);                                         /* AH=01h */
void lrv_pmd_fade(struct lrv_pmd *p, uint8_t speed);                           /* AH=02h */
uint8_t lrv_pmd_get_fadevol(const struct lrv_pmd *p);                          /* AH=08h */
void lrv_pmd_set_fadevol(struct lrv_pmd *p, uint8_t v);                        /* AH=19h */
void lrv_pmd_get_status(const struct lrv_pmd *p, uint8_t *s1, uint8_t *s2);    /* AH=0Ah */
void lrv_pmd_fm_effect_on(struct lrv_pmd *p, int n);                           /* AH=0Ch */
void lrv_pmd_fm_effect_off(struct lrv_pmd *p);                                 /* AH=0Dh */
void lrv_pmd_ssg_effect_start(struct lrv_pmd *p, int n, int pps_flags);        /* AH=03h */
void lrv_pmd_ssg_effect_stop(struct lrv_pmd *p);                               /* AH=04h */

/* Advances time by `frames` output samples, firing the timer interrupts (and their register
 * writes) at the samples they fall on; render(rctx, n) is called for each run of n samples
 * between them, so that the caller renders the chip in between. */
void lrv_pmd_advance(struct lrv_pmd *p, int frames, void (*render)(void *ctx, int nframes),
                     void *rctx);

/* The driver as the queue's frame-driven sequencer (struct lrv_opnq_cfg.seq, or struct
 * lrv_music_cfg.seq): its writes must then go to lrv_opnq_write. */
void lrv_pmd_seq(struct lrv_pmd *p, struct lrv_opnq_seq *seq);

/* Runs every timer interrupt up to and including master clock `clock` (counted from
 * lrv_pmd_init), each at its own clock (p->clock is that clock while it runs), and leaves the
 * driver's time at `clock`. A clock-driven alternative to lrv_pmd_advance; the tests compare
 * the driver with the real one this way. */
void lrv_pmd_run_until(struct lrv_pmd *p, uint64_t clock);

/* One Timer-B (music clock) / Timer-A (effect clock) interrupt's work, without the timers. */
void lrv_pmd_timer_b(struct lrv_pmd *p);
void lrv_pmd_timer_a(struct lrv_pmd *p);

#endif
