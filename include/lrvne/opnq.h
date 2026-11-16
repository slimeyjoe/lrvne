/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The path from a music driver to samples: a queue of register writes in front of an OPN core
 * (lrvne/opn.h), rendered in sample time.
 *
 * On the original machines a music driver ran from the chip's timer interrupts, independent of
 * the game. Here the driver (the "sequencer": the engine's or an lrvne module) advances with
 * the samples rendered: lrv_opnq_render() runs it as far as the block it was asked for and
 * renders the chip in between its register writes, each write taking effect before the sample
 * it falls on. Two kinds of sequencer are served, and a queue is set up for one of them:
 *
 *   clock-driven (seq.run_until)   The sequencer counts master clocks of the chip. Before a
 *       block is rendered it is run up to the clock of the block's end and stamps every write
 *       with the clock it happens at (lrv_opnq_write_at). The clock of output sample n is
 *       clock0 + n * clock_hz / rate, computed in 64 bits without accumulated error.
 *   frame-driven (seq.advance)     The sequencer counts output samples. It is asked to advance
 *       by the frames of the block and has the runs of samples between its events rendered
 *       through the callback it is given (lrv_opnq_span); its writes (lrv_opnq_write) take
 *       effect at the sample the rendering has reached.
 *
 * The bus: a driver makes all the writes of one interrupt at one instant, but on the board
 * they reached the chip one after another (the drivers wait for the chip's busy flag). The
 * queue delivers them at least bus_clocks master clocks apart; with one FM sample (72) a
 * key-off followed at once by a key-on of the same channel reaches the core in separate
 * samples and re-attacks the note, as on hardware. Times are kept in 16.16 output samples, and
 * a write is applied before sample floor(max(its own time, the time the bus is free)).
 *
 * The queue has no lock of its own: everything here is called with the audio lock held where
 * there is one (lrvne/mixer.h), lrv_opnq_render by the mixer. */
#ifndef LRVNE_OPNQ_H
#define LRVNE_OPNQ_H

#include <stdint.h>
#include "lrvne/core.h"
#include "lrvne/opn.h"

struct lrv_opnq;

/* The sequencer in front of the queue: one of run_until / advance, or neither (the caller
 * makes the writes itself between renders). */
struct lrv_opnq_seq {
    void *ctx;
    /* clock-driven: perform everything up to and including master clock `clock` */
    void (*run_until)(void *ctx, uint64_t clock);
    /* frame-driven: advance by `frames` output samples, calling span(q, n) for every run of n
     * samples that passes between events, so that the n add up to `frames` */
    void (*advance)(void *ctx, int frames, void (*span)(void *q, int n), void *q);
};

/* flags */
#define LRV_OPNQ_FLUSH_ON_FULL 1u   /* a write arriving at a full queue applies everything
                                     * queued at once and frees the bus; without the flag only
                                     * the oldest write is applied early */

struct lrv_opnq_cfg {
    const struct lrv_opn_ops *ops;  /* the core; NULL = no chip: time passes, output is silence */
    unsigned core_flags;            /* LRV_OPN_* for its creation */
    uint32_t clock_hz;              /* the chip's master clock */
    uint32_t rate;                  /* the output rate wanted; the core answers with its own */
    uint32_t bus_clocks;            /* master clocks between two writes on the bus, e.g.
                                     * LRV_OPN_FM_DIV; 0 = writes are not spaced */
    uint32_t queue_size;            /* writes the queue holds, plus one */
    unsigned flags;                 /* LRV_OPNQ_* */
    struct lrv_opnq_seq seq;
};

struct lrv_opnq_entry {
    uint64_t t;                     /* its time: a master clock, or a 16.16 output sample */
    uint8_t reg, val;
    uint8_t clocked;                /* t is a master clock (lrv_opnq_write_at) */
};

struct lrv_opnq {
    const struct lrv_hal *hal;
    struct lrv_opnq_cfg cfg;
    struct lrv_opn *core;           /* NULL without a chip */
    uint32_t rate;                  /* output rate: the core's, or cfg.rate without a chip */
    uint64_t clock0;                /* clock-driven: the master clock of output sample 0 */
    uint64_t samples;               /* output samples rendered since the last reset */
    uint64_t bus;                   /* 16.16 output sample from which the bus is free */
    uint32_t cost;                  /* 16.16 output samples a write occupies the bus */
    struct lrv_opnq_entry *wq;      /* cfg.queue_size entries */
    uint32_t head, tail;
    int skip;                       /* rendering silence instead of running the core */
    int16_t *out;                   /* frame-driven: where the block being advanced goes */
    int out_left;
};

/* Creates the core and the queue. Returns LRV_OK, LRV_ERR_NOMEM, or LRV_ERR_RANGE for a
 * configuration that cannot work (no clock, a queue smaller than 2, both kinds of sequencer).
 * Without a core (cfg.ops NULL or cfg.rate 0) writes are dropped and rendering gives silence;
 * a frame-driven sequencer still advances, a clock-driven one only when the rate is not 0. */
int  lrv_opnq_init(struct lrv_opnq *q, const struct lrv_hal *hal, const struct lrv_opnq_cfg *cfg);
void lrv_opnq_free(struct lrv_opnq *q);

/* The output rate; 0 when there is none. */
uint32_t lrv_opnq_rate(const struct lrv_opnq *q);

/* The backend took another rate than the core's: creates the core again for `rate` (its
 * registers start from reset), keeps what is queued. Returns the rate the new core produces,
 * 0 when it could not be created. */
uint32_t lrv_opnq_set_rate(struct lrv_opnq *q, uint32_t rate);

/* The chip's reset: the core reset, the queue emptied, sample 0 and clock0 = 0 again. */
void lrv_opnq_reset(struct lrv_opnq *q);
/* Clock-driven: the sequencer's clock that output sample 0 (the next one after init or reset)
 * stands for; set it after the sequencer was initialised, queued writes keep their clocks. */
void lrv_opnq_set_clock0(struct lrv_opnq *q, uint64_t clock);

/* A write that reaches the core at once, past the queue (what was on the chip before the
 * driver started). */
void lrv_opnq_poke(struct lrv_opnq *q, uint8_t reg, uint8_t val);
/* A write stamped with the sequencer's clock (clock-driven). */
void lrv_opnq_write_at(struct lrv_opnq *q, uint64_t clock, uint8_t reg, uint8_t val);
/* A write happening now, at the sample the rendering has reached (frame-driven). */
void lrv_opnq_write(struct lrv_opnq *q, uint8_t reg, uint8_t val);
/* Writes waiting in the queue. */
uint32_t lrv_opnq_pending(const struct lrv_opnq *q);

/* Renders the next `frames` samples: runs the sequencer, applies the writes that fall into the
 * block, renders the core between them. The signature is the mixer's main source. */
void lrv_opnq_render(void *q, int16_t *mono, int frames);
/* The same in time, but the core is not run and the block is silence: the sequencer advances
 * and every write reaches the core at its place, so the music goes on exactly where it would
 * have been. For a backend that cannot keep up and drops a block rather than fall behind; the
 * mixer's skip hook (lrv_mixer_cfg.skip). */
void lrv_opnq_skip(void *q, int16_t *mono, int frames);
/* Renders a run of n samples of the block lrv_opnq_render is advancing: the callback a
 * frame-driven sequencer is given. */
void lrv_opnq_span(void *q, int n);

#endif
