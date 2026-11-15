/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The once-per-frame OS work of the main task, batched into one call (frameos.c): the frame
 * clock's timer wait, the IDCMP input gather and the free memory sample. On WarpOS every OS
 * call from the PPC is a context switch to the 68k (README.md "WarpOS"), and frameos.c is
 * 68k code reached with one Run68K per frame (mk/target/warpos.mk compiles it with the 68k
 * compiler); everywhere else it is called directly (amsys.h am_frameos_call).
 *
 * Only ULONG, LONG and pointer fields, so both WarpOS compilers lay it out the same way
 * (checked by the layout guard, warpos/oslayout.h). The C side allocates it aligned to 32
 * bytes (a PPC cache line) and owns it between calls; frameos.c only runs inside
 * am_frame_call. */
#ifndef LRVNE_BACKENDS_AMIGA_FRAMEOS_H
#define LRVNE_BACKENDS_AMIGA_FRAMEOS_H

#include "amcompat.h"           /* AM_REG; needs <exec/types.h> first */

/* ops */
#define AM_F_WAIT   1UL             /* step the deadline one tick and wait for it */
#define AM_F_INPUT  2UL             /* gather IDCMP messages, the front screen, Ctrl-C */
#define AM_F_MEM    4UL             /* AvailMem(MEMF_CHIP), AvailMem(MEMF_FAST) */

#define AM_F_EVENTS 32

struct am_frame_ev {                /* one IntuiMessage, copied before ReplyMsg */
    ULONG cls;
    ULONG code;
    ULONG qual;
    LONG mx, my;
};

struct am_frameos {
    /* set by the C side (timer.c, input.c) */
    struct Device *timer_base;
    struct timerequest *req;        /* timer.device UNIT_WAITUNTIL */
    struct IntuitionBase *ibase;
    struct Window *win;             /* 0: AM_F_INPUT does nothing */
    struct Screen *scr;             /* ours, for the front check */
    ULONG relative;                 /* req is UNIT_MICROHZ: wait for the time left, not until */
    ULONG ops;
    /* the frame clock: the period (the video mode's frame: tick_us microseconds and tick_ns
     * nanoseconds, 0..999) and the next deadline, kept between calls (timer.c sets both) */
    ULONG tick_us, tick_ns;
    ULONG dl_secs, dl_micro;
    ULONG dl_frac;                  /* nanoseconds carried */
    /* AM_F_WAIT results */
    LONG late_us;                   /* how late the call came for the deadline */
    ULONG resynced;                 /* late by more than ten frames: the deadline restarted now */
    ULONG waited;                   /* the deadline was ahead: waited for it */
    ULONG wake_us;                  /* how late the wait returned */
    /* AM_F_INPUT results (the C side consumes ev[] and clears gathered) */
    ULONG gathered;                 /* ev[] holds a gather the C side has not consumed */
    ULONG n_ev;
    ULONG more;                     /* ev[] filled up: messages are left in the port */
    ULONG front;                    /* our screen is the frontmost */
    ULONG brk;                      /* Ctrl-C was signalled (the C side clears it) */
    /* AM_F_MEM results */
    ULONG avail_chip, avail_fast;
    struct am_frame_ev ev[AM_F_EVENTS];
};

/* frameos.c: a0 = the struct when WarpOS calls it (Run68K, only its address is used on the
 * PPC side), a plain C call everywhere else */
void am_frameos(AM_REG(a0) struct am_frameos *f);

/* frame.c: the struct and the call */
extern struct am_frameos *am_fr;
int  am_frame_init(void);
void am_frame_free(void);
void am_frame_call(ULONG ops);

#endif
