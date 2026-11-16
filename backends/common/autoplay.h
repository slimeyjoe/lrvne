/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Scripted input for tests and automation, shared by the backends (--autoplay SPEC).
 *
 * A spec is a list of words separated by blanks or newlines; "@path" in place of a spec reads
 * it from a file (some hosts' command lines are too short for a long schedule).
 *   at:T=KEY[+KEY..],..  press the keys once at tick T, in the order written
 *   KEY:N                press KEY every N ticks (ignored when the spec has at: or held keys)
 *   KEY:T0-T1,..         hold KEY during ticks T0 <= t < T1; shift, ctrl, alt, caps and kana
 *                        also count as modifiers of the keys pressed meanwhile
 *   button_l:T0-T1,..    hold the left mouse button (button_r: the right one)
 *   rand:SEED            a random press of return, up, down or esc every 20 ticks
 *   snap:T,T,..          ticks at which the backend takes a snapshot (lrv_autoplay_snap_due)
 *
 * Words driven by the engine's input waits instead of ticks (the LRV_TR_WAIT events of the
 * kinds lrv_app.wait_is_input accepts, the waits the control protocol stops at). A run
 * driven by them does not depend on how long anything took: a wait's point is the end of
 * the first whole tick the engine spends in it, whatever tick that is.
 *   KEY:wait             press KEY once per input wait, at the first poll after its point
 *                        (several KEY:wait words press their keys in the order written)
 *   snap:wait            a snapshot at each input wait's point, named w1, w2, ..
 *   waits:N              after the N-th input wait's point, an emergency quit at every poll
 *                        (the engine shuts down normally; MAXTICKS stays the backstop)
 * The backend feeds the waits in (lrv_autoplay_wait) when lrv_autoplay_uses_waits says so,
 * and calls lrv_autoplay_end_tick at the end of every tick before asking for snapshots.
 *
 * KEY is a key name of lrvne/input.h ("return", "esc", "kp8", "f1", "a", ...), or "ok" for
 * return and "cancel" for esc. In an at: entry it may also be an event: quit, emergency, reset,
 * mouse_l, mouse_r (a click).
 *
 * Presses behave like the pending presses of the test automation protocol: an at: entry is
 * delivered by the first poll at or after its tick, once, and input_flush drops the entries
 * that are due (their quit, emergency and reset survive, as the HAL requires). So a transcript
 * of a protocol session (a press at the stop of tick T) replays tick for tick as at:T=... */
#ifndef LRVNE_BACKENDS_AUTOPLAY_H
#define LRVNE_BACKENDS_AUTOPLAY_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/input.h"

#define LRV_AUTOPLAY_CHORD 8    /* keys of one at: entry */

struct lrv_autoplay_at {
    uint32_t tick, seq;         /* seq: the entry's position in the spec */
    uint32_t events;            /* LRV_EV_* */
    uint8_t nkeys;
    uint8_t keys[LRV_AUTOPLAY_CHORD];
};

struct lrv_autoplay_hold {
    uint32_t t0, t1;
    uint8_t key;                /* enum lrv_key, or LRV_K_NONE for a button */
    uint8_t buttons;            /* LRV_BTN_* */
};

struct lrv_autoplay_every {
    uint32_t every;
    uint8_t key;
};

struct lrv_autoplay {
    unsigned rnd;                       /* rand: seed (0 = off) */
    unsigned rnd_state;
    struct lrv_autoplay_at *at;         /* sorted by tick */
    int at_count, at_next;
    struct lrv_autoplay_hold *hold;
    int hold_count;
    struct lrv_autoplay_every *every;
    int every_count;
    uint32_t *snap;                     /* sorted */
    int snap_count;
    uint32_t last_tick;                 /* periodic presses are delivered once per tick */
    int polled;
    uint32_t kept;                      /* sticky events of entries a flush dropped */
    /* the words driven by input waits */
    uint8_t wait_keys[LRV_AUTOPLAY_CHORD];
    int wait_nkeys;
    int snap_wait;                      /* snap:wait */
    uint32_t waits_limit;               /* waits:N, 0 = none */
    int in_wait;                        /* the engine is in an input wait */
    int wait_ticks;                     /* tick ends since it was entered */
    uint32_t ended_tick;                /* the last tick lrv_autoplay_end_tick saw */
    int point;                          /* ended_tick was a wait's point */
    uint32_t waits_done;                /* points so far: the last one's snapshot is w<waits_done> */
    int press_due;                      /* the wait keys go out at the next poll */
};

/* The key or the event of a name of `len` characters, as specs and protocol commands spell
 * them: lrv_key_by_name plus "ok", "cancel" and the event names. Returns 1 and sets one of
 * *key (else LRV_K_NONE) and *event (else 0), or 0 when the name means nothing. */
int lrv_input_by_name(const char *name, size_t len, int *key, uint32_t *event);
/* The LRV_MOD_* bit a key stands for while it is held (shift, ctrl, alt) or locked (caps,
 * kana), 0 for every other key. */
unsigned lrv_input_key_mod(int key);

/* Parses `spec` into *ap (cleared first). Returns 0, or LRV_ERR_FORMAT / LRV_ERR_NOMEM /
 * LRV_ERR_IO with a message in err. */
int lrv_autoplay_parse(struct lrv_autoplay *ap, const char *spec, char *err, size_t errlen);
void lrv_autoplay_free(struct lrv_autoplay *ap);

/* Adds the presses due at `tick` to *in and sets the held levels. */
void lrv_autoplay_poll(struct lrv_autoplay *ap, uint32_t tick, struct lrv_input *in);
/* input_flush: drops the presses due at `tick`; their sticky events go to the next poll. */
void lrv_autoplay_flush(struct lrv_autoplay *ap, uint32_t tick);
/* 1 when the spec has words driven by input waits (the backend then feeds them in). */
int lrv_autoplay_uses_waits(const struct lrv_autoplay *ap);
/* The engine entered (enter = 1) or left an input wait. */
void lrv_autoplay_wait(struct lrv_autoplay *ap, int enter);
/* The end of `tick` (every tick, before lrv_autoplay_snap_due for it). */
void lrv_autoplay_end_tick(struct lrv_autoplay *ap, uint32_t tick);
/* 1 when the end of `tick` takes a snapshot: it is in the snap: list, or it is (or, asked
 * during that tick, will be) an input wait's point with snap:wait. */
int lrv_autoplay_snap_due(const struct lrv_autoplay *ap, uint32_t tick);
/* The snapshot's name at `tick`: t<tick> for a snap: tick, else w<k> for the k-th wait. */
void lrv_autoplay_snap_name(const struct lrv_autoplay *ap, uint32_t tick, char *buf, size_t len);
/* The last tick of the at:, held and snap: lists (0 when they are empty). */
uint32_t lrv_autoplay_last_tick(const struct lrv_autoplay *ap);

#endif
