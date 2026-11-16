/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The music path: driver, write queue, core, mixer, device (lrvne/music.h). */
#include <string.h>
#include "lrvne/music.h"
#include "lrvne/hal.h"
#include "lrvne/opn.h"

/* the mixer's idle hook: without a device, a frame's worth of samples at the driver's rate
 * is rendered into nothing, so that the driver and the voice keep the frame's time */
static void silent_clock(void *ctx)
{
    struct lrv_music *m = ctx;
    int16_t buf[LRV_MIXER_BLOCK];
    /* 16.16 samples per frame, in integers: rate * 65536 * den / num */
    m->silent_acc += (uint32_t)((((uint64_t)m->rate << 16) * m->cfg.frame_den) /
                                m->cfg.frame_num);
    while (m->silent_acc >= 0x10000u) {
        int n = (int)(m->silent_acc >> 16);
        if (n > LRV_MIXER_BLOCK)
            n = LRV_MIXER_BLOCK;
        lrv_mixer_render(&m->mixer, buf, n);
        m->silent_acc -= (uint32_t)n << 16;
    }
}

/* the queue with a core of `ops` at `rate`; ops NULL = no chip (time passes, nothing sounds) */
static int chip_init(struct lrv_music *m, const struct lrv_opn_ops *ops, unsigned flags,
                     uint32_t rate)
{
    struct lrv_opnq_cfg qc;
    memset(&qc, 0, sizeof qc);
    qc.ops = ops;
    qc.core_flags = flags;
    qc.clock_hz = m->cfg.clock_hz;
    qc.rate = rate;
    qc.bus_clocks = m->cfg.bus_clocks;
    qc.queue_size = m->cfg.queue_size;
    qc.flags = m->cfg.queue_flags;
    qc.seq = m->cfg.seq;
    return lrv_opnq_init(&m->q, m->hal, &qc);
}

/* The core first, then the device at the core's rate; a device that insists on another rate
 * gets a core that follows it, or the light C core. Returns the rate of the core that renders,
 * 0 when there is no device (and then no core). */
static int device_init(struct lrv_music *m, uint32_t *rate)
{
    const struct lrv_opn_ops *ops = lrv_opn_select(m->cfg.core);
    unsigned flags = lrv_opn_flags(m->cfg.core);
    uint32_t r;
    int rc = chip_init(m, ops, flags, (uint32_t)m->cfg.rate);
    *rate = 0;
    if (rc != LRV_OK)
        return rc;
    r = lrv_opnq_rate(&m->q);
    if (r) {
        int got = lrv_mixer_open(&m->mixer, (int)r);
        if (got && (uint32_t)got != r && lrv_opnq_set_rate(&m->q, (uint32_t)got) != (uint32_t)got) {
            lrv_opnq_free(&m->q);           /* nothing renders before lrv_music_start */
            ops = &lrv_opn_lite_ops;
            rc = chip_init(m, ops, flags, (uint32_t)got);
            if (rc != LRV_OK)
                return rc;
        }
        r = got ? lrv_opnq_rate(&m->q) : 0;
    }
    if (!r)
        lrv_opnq_free(&m->q);               /* no device: no chip */
    else
        m->core_name = ops->name;
    *rate = r;
    return LRV_OK;
}

int lrv_music_open(struct lrv_music *m, const struct lrv_hal *hal,
                   const struct lrv_music_cfg *cfg)
{
    struct lrv_mixer_cfg mc;
    uint32_t rate = 0;
    int rc;

    memset(m, 0, sizeof *m);
    m->hal = hal;
    m->cfg = *cfg;
    if (!m->cfg.frame_num || !m->cfg.frame_den)
        m->cfg.idle_rate = 0;               /* no frame: no time to keep */
    memset(&mc, 0, sizeof mc);
    mc.render = lrv_opnq_render;
    mc.skip = lrv_opnq_skip;                /* for a backend that falls behind */
    mc.ctx = &m->q;
    if (m->cfg.idle_rate) {
        mc.idle = silent_clock;
        mc.idle_ctx = m;
    }
    mc.gain_floor = cfg->gain_floor;
    lrv_mixer_init(&m->mixer, hal, &mc);

    if (cfg->flags & LRV_MUSIC_OFFLINE) {
        const struct lrv_opn_ops *ops = lrv_opn_select(cfg->core);
        rc = chip_init(m, ops, lrv_opn_flags(cfg->core),
                       cfg->rate > 0 ? (uint32_t)cfg->rate : m->cfg.idle_rate);
        if (rc != LRV_OK)
            return rc;
        m->rate = lrv_opnq_rate(&m->q);
        m->core_name = m->rate ? ops->name : 0;
        return LRV_OK;
    }
    if (cfg->rate > 0) {
        rc = device_init(m, &rate);
        if (rc != LRV_OK) {
            lrv_mixer_close(&m->mixer);
            return rc;
        }
    }
    if (!rate) {
        rc = chip_init(m, 0, 0, m->cfg.idle_rate);
        if (rc != LRV_OK) {
            lrv_mixer_close(&m->mixer);
            return rc;
        }
    }
    m->rate = rate ? rate : m->cfg.idle_rate;
    lrv_log(hal, LRV_LOG_INFO, "sound: core %s, %lu Hz%s",
            m->core_name ? m->core_name : "none", (unsigned long)rate,
            !lrv_mixer_has_device(&m->mixer) ? " (no audio output)"
            : m->mixer.pull ? " (pull: backend thread)" : " (push)");
    return LRV_OK;
}

uint32_t lrv_music_rate(const struct lrv_music *m)
{
    return m->rate;
}

int lrv_music_has_chip(const struct lrv_music *m)
{
    return m->q.core != 0;
}

void lrv_music_start(struct lrv_music *m)
{
    lrv_mixer_start(&m->mixer, m->rate);
}

void lrv_music_close(struct lrv_music *m)
{
    if (!m->hal)
        return;
    lrv_mixer_close(&m->mixer);             /* first: the sources are not called after it */
    lrv_opnq_free(&m->q);
    m->rate = 0;
    m->core_name = 0;
}
