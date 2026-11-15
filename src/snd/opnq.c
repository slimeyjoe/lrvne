/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The register-write queue and the render path of an OPN core (lrvne/opnq.h). */
#include <string.h>
#include "lrvne/opnq.h"

/* 16.16 output samples a write keeps the bus busy: rate / (clock / bus_clocks) */
static uint32_t bus_cost(const struct lrv_opnq *q)
{
    uint32_t per_s;
    if (!q->cfg.bus_clocks || !q->rate)
        return 0;
    per_s = q->cfg.clock_hz / q->cfg.bus_clocks;        /* writes the bus takes in a second */
    return per_s ? (uint32_t)(((uint64_t)q->rate << 16) / per_s) : 0;
}

static int create_core(struct lrv_opnq *q, uint32_t want)
{
    uint32_t rate = 0;
    q->core = q->cfg.ops->create(q->hal, q->cfg.clock_hz, want, q->cfg.core_flags, &rate);
    q->rate = q->core ? rate : 0;
    q->cost = bus_cost(q);
    return q->core ? LRV_OK : LRV_ERR_NOMEM;
}

int lrv_opnq_init(struct lrv_opnq *q, const struct lrv_hal *hal, const struct lrv_opnq_cfg *cfg)
{
    memset(q, 0, sizeof *q);
    q->hal = hal;
    q->cfg = *cfg;
    if (!cfg->clock_hz || cfg->queue_size < 2 || (cfg->seq.run_until && cfg->seq.advance)) {
        q->cfg.queue_size = 0;
        return LRV_ERR_RANGE;
    }
    q->wq = lrv_alloc(hal, sizeof *q->wq * cfg->queue_size);
    if (!q->wq) {
        q->cfg.queue_size = 0;
        return LRV_ERR_NOMEM;
    }
    if (cfg->ops && cfg->rate) {
        if (create_core(q, cfg->rate) != LRV_OK) {
            lrv_opnq_free(q);
            return LRV_ERR_NOMEM;
        }
    } else {
        q->cfg.ops = 0;
        q->rate = cfg->rate;
        q->cost = bus_cost(q);
    }
    return LRV_OK;
}

void lrv_opnq_free(struct lrv_opnq *q)
{
    if (!q->hal)
        return;
    if (q->core)
        q->cfg.ops->destroy(q->core);
    q->core = 0;
    lrv_free(q->hal, q->wq, sizeof *q->wq * q->cfg.queue_size);
    q->wq = 0;
    q->cfg.queue_size = 0;
    q->head = q->tail = 0;
    q->rate = 0;
}

uint32_t lrv_opnq_rate(const struct lrv_opnq *q)
{
    return q->rate;
}

uint32_t lrv_opnq_set_rate(struct lrv_opnq *q, uint32_t rate)
{
    if (!q->core)
        return 0;
    q->cfg.ops->destroy(q->core);
    create_core(q, rate);
    return q->rate;
}

void lrv_opnq_reset(struct lrv_opnq *q)
{
    if (q->core)
        q->cfg.ops->reset(q->core);
    q->head = q->tail = 0;
    q->clock0 = 0;
    q->samples = 0;
    q->bus = 0;
}

void lrv_opnq_set_clock0(struct lrv_opnq *q, uint64_t clock)
{
    q->clock0 = clock;
}

void lrv_opnq_poke(struct lrv_opnq *q, uint8_t reg, uint8_t val)
{
    if (q->core)
        q->cfg.ops->write(q->core, reg, val);
}

static void apply_head(struct lrv_opnq *q)
{
    q->cfg.ops->write(q->core, q->wq[q->head].reg, q->wq[q->head].val);
    q->head = (q->head + 1) % q->cfg.queue_size;
}

static void enqueue(struct lrv_opnq *q, uint64_t t, int clocked, uint8_t reg, uint8_t val)
{
    uint32_t next;
    if (!q->core)
        return;                         /* no chip: nothing will ever render it */
    next = (q->tail + 1) % q->cfg.queue_size;
    if (next == q->head) {
        /* full: the driver wrote faster than the bus takes for longer than the queue holds */
        if (q->cfg.flags & LRV_OPNQ_FLUSH_ON_FULL) {
            while (q->head != q->tail)
                apply_head(q);
            q->bus = 0;
        } else {
            apply_head(q);
        }
    }
    q->wq[q->tail].t = t;
    q->wq[q->tail].reg = reg;
    q->wq[q->tail].val = val;
    q->wq[q->tail].clocked = (uint8_t)clocked;
    q->tail = next;
}

void lrv_opnq_write_at(struct lrv_opnq *q, uint64_t clock, uint8_t reg, uint8_t val)
{
    enqueue(q, clock, 1, reg, val);
}

void lrv_opnq_write(struct lrv_opnq *q, uint8_t reg, uint8_t val)
{
    enqueue(q, q->samples << 16, 0, reg, val);
}

uint32_t lrv_opnq_pending(const struct lrv_opnq *q)
{
    return q->cfg.queue_size ? (q->tail + q->cfg.queue_size - q->head) % q->cfg.queue_size : 0;
}

/* the output sample (16.16, counted from the last reset) a queued write falls on */
static uint64_t time_of(const struct lrv_opnq *q, const struct lrv_opnq_entry *e)
{
    if (!e->clocked)
        return e->t;
    if (e->t <= q->clock0)
        return 0;
    return ((e->t - q->clock0) * (uint64_t)q->rate << 16) / q->cfg.clock_hz;
}

/* Renders the next n samples to pcm: the writes that are due go to the core before the sample
 * they fall on, never closer together than the bus allows. */
static void render_run(struct lrv_opnq *q, int16_t *pcm, int n)
{
    uint64_t end = q->samples + (uint64_t)n;
    int pos = 0;

    if (!q->core) {
        memset(pcm, 0, sizeof *pcm * (size_t)n);
        q->samples = end;
        return;
    }
    while (pos < n) {
        int upto = n;
        if (q->head != q->tail) {
            uint64_t at = time_of(q, &q->wq[q->head]);
            if (at < q->bus)
                at = q->bus;
            if ((at >> 16) < end) {
                int k = (int)((at >> 16) - q->samples);     /* negative: overdue */
                if (k <= pos) {
                    apply_head(q);
                    q->bus = at + q->cost;
                    continue;
                }
                upto = k;
            }
        }
        if (q->skip)
            memset(pcm + pos, 0, sizeof *pcm * (size_t)(upto - pos));
        else
            q->cfg.ops->render(q->core, pcm + pos, upto - pos);
        pos = upto;
    }
    q->samples = end;
}

void lrv_opnq_span(void *ctx, int n)
{
    struct lrv_opnq *q = ctx;
    if (n > q->out_left)
        n = q->out_left;                /* a sequencer asking for more than the block */
    if (n <= 0)
        return;
    render_run(q, q->out, n);
    q->out += n;
    q->out_left -= n;
}

void lrv_opnq_render(void *ctx, int16_t *mono, int frames)
{
    struct lrv_opnq *q = ctx;
    const struct lrv_opnq_seq *seq = &q->cfg.seq;

    if (frames <= 0)
        return;
    if (seq->advance) {
        q->out = mono;
        q->out_left = frames;
        seq->advance(seq->ctx, frames, lrv_opnq_span, q);
        if (q->out_left > 0)            /* ... or for less */
            lrv_opnq_span(q, q->out_left);
        q->out = 0;
        return;
    }
    if (!q->rate) {
        memset(mono, 0, sizeof *mono * (size_t)frames);
        return;
    }
    if (seq->run_until) {
        /* up to the first clock that is not before the end of the block */
        uint64_t end = q->samples + (uint64_t)frames;
        seq->run_until(seq->ctx, q->clock0 + (end * q->cfg.clock_hz + q->rate - 1) / q->rate);
    }
    render_run(q, mono, frames);
}

void lrv_opnq_skip(void *ctx, int16_t *mono, int frames)
{
    struct lrv_opnq *q = ctx;
    q->skip = 1;
    lrv_opnq_render(q, mono, frames);
    q->skip = 0;
}
