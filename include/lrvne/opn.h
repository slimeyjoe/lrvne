/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The OPN (YM2203) chip core: three FM channels and the SSG.
 *
 * A music driver writes registers, the core renders mono 16-bit samples at the rate it answered
 * at creation. The chip's timers are not emulated: the drivers model them (register 27h is
 * still forwarded, its bits 6-7 select the FM3 special mode). Two implementations share the
 * interface:
 *
 *   lrv_opn_lite_ops   integer C for low-end machines; renders directly at the output rate
 *   lrv_opn_ymfm_ops   ymfm (C++), with LRV_HAVE_YMFM; renders at the chip's own rate,
 *                      clock / 72, whatever rate is asked for
 *
 * A core keeps all its state in the instance create() returns; the memory comes from the HAL.
 * Calls on one instance must not overlap (lrvne/mixer.h has the lock). */
#ifndef LRVNE_OPN_H
#define LRVNE_OPN_H

#include <stdint.h>
#include "lrvne/core.h"

#define LRV_OPN_CLOCK_PC98 3993600u /* the master clock of the PC-98 sound boards */
#define LRV_OPN_FM_DIV     72u      /* master clocks per FM sample: the chip runs at clock / 72 */

struct lrv_opn;                     /* one core instance; its layout belongs to the core */

/* render_split streams: the three FM channels and the SSG (its three tones mixed). Each stream
 * is scaled like the mono output, so their sum is the mono render up to rounding (every stream
 * has its own DC blocker). For a sink that mixes the channels itself. */
#define LRV_OPN_SPLIT_FM1     0
#define LRV_OPN_SPLIT_FM2     1
#define LRV_OPN_SPLIT_FM3     2
#define LRV_OPN_SPLIT_SSG     3
#define LRV_OPN_SPLIT_STREAMS 4

/* create() flags */
#define LRV_OPN_OVERSAMPLE 1u       /* lite: compute the FM part at twice the output rate and
                                     * decimate (less aliasing, about 1.6x the FM cost) */

struct lrv_opn_ops {
    const char *name;
    /* A core for a chip clocked at clock_hz. want_rate is the output rate the caller prefers
     * (0 = the core's default); *rate receives the rate the core really produces. NULL when
     * there is no memory. */
    struct lrv_opn *(*create)(const struct lrv_hal *hal, uint32_t clock_hz, uint32_t want_rate,
                              unsigned flags, uint32_t *rate);
    void (*destroy)(struct lrv_opn *c);
    void (*reset)(struct lrv_opn *c);                   /* the chip's reset: all registers 0 */
    void (*write)(struct lrv_opn *c, uint8_t reg, uint8_t val);
    void (*render)(struct lrv_opn *c, int16_t *mono, int frames);
    /* optional (NULL when the core cannot): renders LRV_OPN_SPLIT_STREAMS separate streams
     * instead of the mono mix. An instance is used with render() or with render_split(), not
     * both: the split streams keep their own filter state. */
    void (*render_split)(struct lrv_opn *c, int16_t *const streams[LRV_OPN_SPLIT_STREAMS],
                         int frames);
};

extern const struct lrv_opn_ops lrv_opn_lite_ops;
#ifdef LRV_HAVE_YMFM
extern const struct lrv_opn_ops lrv_opn_ymfm_ops;
#endif

/* The core for lrv_run_cfg.opn_core: 0 = the best one built in, 1 = the light C core. */
const struct lrv_opn_ops *lrv_opn_select(int which);

#endif
