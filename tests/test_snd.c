/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests of the sound path: the OPN cores, the register-write queue, the mixer's push and
 * pull delivery, and the voice clip player. Nothing here is a game's data: the "song" is a
 * register sequence made up for the tests, the clips are ramps. */
#include "test_hal.h"
#include "lrvne/mixer.h"
#include "lrvne/opn.h"
#include "lrvne/opnq.h"
#include "lrvne/voice.h"

/* ---- a synthetic register sequence ----------------------------------------------------- */

/* A short "song": every block song_block() makes the writes of block b. It visits all eight
 * algorithms with feedback, detune and key scaling, the FM3 per-slot frequencies, back-to-back
 * key-off/key-on, the SSG tones, noise and hardware envelope. */
#define SONG_BLOCK  61
#define SONG_BLOCKS 420

typedef void song_write_fn(void *ctx, uint8_t reg, uint8_t val);

static void song_voice(song_write_fn *w, void *ctx, int ch, int alg, int fb)
{
    static const uint8_t mul[4] = { 0x01, 0x32, 0x71, 0x02 };
    static const uint8_t tl[4] = { 0x1C, 0x24, 0x18, 0x00 };
    static const uint8_t ar[4] = { 0x1F, 0x9C, 0x5A, 0x1F };
    static const uint8_t dr[4] = { 0x08, 0x0A, 0x05, 0x0C };
    static const uint8_t sr[4] = { 0x02, 0x00, 0x03, 0x04 };
    static const uint8_t rr[4] = { 0x27, 0x48, 0x36, 0x1A };
    int s;
    for (s = 0; s < 4; s++) {
        uint8_t off = (uint8_t)(ch + 4 * s);
        w(ctx, (uint8_t)(0x30 + off), mul[s]);
        w(ctx, (uint8_t)(0x40 + off), (uint8_t)(tl[s] + (alg >= 4 && s != 0 ? 0 : 2 * alg)));
        w(ctx, (uint8_t)(0x50 + off), ar[s]);
        w(ctx, (uint8_t)(0x60 + off), dr[s]);
        w(ctx, (uint8_t)(0x70 + off), sr[s]);
        w(ctx, (uint8_t)(0x80 + off), rr[s]);
    }
    w(ctx, (uint8_t)(0xB0 + ch), (uint8_t)((fb << 3) | alg));
}

static void song_note(song_write_fn *w, void *ctx, int ch, int block, int fnum)
{
    w(ctx, (uint8_t)(0xA4 + ch), (uint8_t)((block << 3) | (fnum >> 8)));
    w(ctx, (uint8_t)(0xA0 + ch), (uint8_t)fnum);
}

static void song_block(int b, song_write_fn *w, void *ctx)
{
    static const uint16_t fnum[8] = { 617, 653, 692, 733, 777, 823, 872, 924 };
    int step = b / 20, ph = b % 20, ch = step % 3, alg = step % 8;
    if (b == 0) {
        w(ctx, 0x07, 0x38);                             /* SSG: tones on, noise off */
        w(ctx, 0x27, 0x00);
        w(ctx, 0x0B, 0x40); w(ctx, 0x0C, 0x02);         /* envelope period */
    }
    if (ph == 0) {
        /* a new note on the step's channel: voice, pitch, key-off and key-on back to back */
        song_voice(w, ctx, ch, alg, (step * 3) % 8);
        song_note(w, ctx, ch, 2 + step % 5, fnum[step % 8]);
        w(ctx, 0x28, (uint8_t)ch);
        w(ctx, 0x28, (uint8_t)(0xF0 | ch));
        /* SSG part: a tone per step, every third with the envelope, some with noise */
        w(ctx, (uint8_t)(2 * ch), (uint8_t)(0x1AC >> (step % 4)));
        w(ctx, (uint8_t)(2 * ch + 1), (uint8_t)((0x1AC >> (step % 4)) >> 8));
        w(ctx, (uint8_t)(8 + ch), (uint8_t)(step % 3 == 2 ? 0x10 : 9 + step % 7));
        if (step % 3 == 2)
            w(ctx, 0x0D, (uint8_t)(8 + step % 8));
        w(ctx, 0x06, (uint8_t)(3 + step));
        w(ctx, 0x07, (uint8_t)(step % 4 == 3 ? 0x38 & ~(8 << ch) : 0x38));
    }
    if (ph == 7 && step % 4 == 1) {
        /* FM3 special mode: its own frequency for three of the slots */
        w(ctx, 0x27, 0x40);
        w(ctx, 0xAC, 0x1A); w(ctx, 0xA8, 0x55);
        w(ctx, 0xAD, 0x22); w(ctx, 0xA9, 0x10);
        w(ctx, 0xAE, 0x13); w(ctx, 0xAA, 0xC0);
    }
    if (ph == 15 && step % 4 == 1)
        w(ctx, 0x27, 0x00);
    if (ph == 12) {
        w(ctx, 0x28, (uint8_t)ch);                      /* release */
        if (step % 2)
            w(ctx, (uint8_t)(8 + ch), 0);
    }
    if (ph == 16 && step % 5 == 0)
        w(ctx, 0x28, (uint8_t)(0x50 | ((ch + 1) % 3))); /* two slots of another channel */
}

/* FNV-1a over the samples as little-endian bytes */
static uint32_t pcm_hash(uint32_t h, const int16_t *pcm, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        h = (h ^ (uint8_t)pcm[i]) * 16777619u;
        h = (h ^ (uint8_t)((uint16_t)pcm[i] >> 8)) * 16777619u;
    }
    return h;
}
#define PCM_HASH0 2166136261u

/* ---- two synthetic music drivers playing the song --------------------------------------- */

/* Clock-driven, like a driver modelled on the chip's timers: an "interrupt" every CSEQ_PERIOD
 * master clocks makes the writes of the next block, all stamped with the interrupt's clock. */
#define CSEQ_START  1000u
#define CSEQ_PERIOD 11057u          /* about 361 interrupts a second; no multiple of 72 */

typedef void cseq_write_fn(void *ctx, uint64_t clock, uint8_t reg, uint8_t val);

struct cseq {
    cseq_write_fn *write;
    void *ctx;
    uint64_t clk, next;
    int b;
};

static void cseq_init(struct cseq *s, cseq_write_fn *write, void *ctx)
{
    s->write = write;
    s->ctx = ctx;
    s->clk = CSEQ_START;
    s->next = CSEQ_START + CSEQ_PERIOD;
    s->b = 0;
}

static void cseq_song_write(void *ctx, uint8_t reg, uint8_t val)
{
    struct cseq *s = ctx;
    s->write(s->ctx, s->clk, reg, val);
}

static void cseq_run_until(void *ctx, uint64_t until)
{
    struct cseq *s = ctx;
    while (s->next <= until && s->b < SONG_BLOCKS) {
        s->clk = s->next;
        song_block(s->b++, cseq_song_write, s);
        s->next += CSEQ_PERIOD;
    }
    if (until > s->clk)
        s->clk = until;
}

/* Frame-driven, like a driver that counts time in output samples: it is asked to advance by a
 * number of frames and calls back to have the samples between two of its events rendered. */
#define FSEQ_PERIOD 0x3D5EB8u       /* 61.37 samples between events, 16.16 */

struct fseq {
    song_write_fn *write;
    void *ctx;
    uint32_t acc;                   /* samples until the next event, 16.16 */
    int b;
};

static void fseq_init(struct fseq *s, song_write_fn *write, void *ctx)
{
    s->write = write;
    s->ctx = ctx;
    s->acc = 0;
    s->b = 0;
}

static void fseq_events(struct fseq *s)
{
    while (s->acc < 0x10000u && s->b < SONG_BLOCKS) {
        song_block(s->b++, s->write, s->ctx);
        s->acc += FSEQ_PERIOD;
    }
}

static void fseq_advance(void *ctx, int frames, void (*render)(void *rctx, int n), void *rctx)
{
    struct fseq *s = ctx;
    while (frames > 0) {
        int n = frames;
        fseq_events(s);
        if (s->b < SONG_BLOCKS && (int)(s->acc >> 16) < n)
            n = (int)(s->acc >> 16);
        render(rctx, n);
        if (s->b < SONG_BLOCKS)
            s->acc -= (uint32_t)n << 16;
        frames -= n;
    }
    fseq_events(s);
}

/* the sizes a sink asks for: uneven, some single samples, some long */
static int chunk_size(int i)
{
    static const int sizes[] = { 1, 367, 2048, 13, 512, 1, 1, 940, 64, 1500 };
    return sizes[i % (int)(sizeof sizes / sizeof sizes[0])];
}

/* ---- the cores ---------------------------------------------------------------------------- */

struct core_ref {
    const struct lrv_opn_ops *ops;
    struct lrv_opn *core;
};

static void core_write(void *ctx, uint8_t reg, uint8_t val)
{
    struct core_ref *r = ctx;
    r->ops->write(r->core, reg, val);
}

/* the song written straight to a core, a block of samples after each block of writes */
static uint32_t core_sum(const struct lrv_hal *hal, const struct lrv_opn_ops *ops, uint32_t want,
                         unsigned flags, int split, uint32_t *rate, int *peak)
{
    static int16_t buf[LRV_OPN_SPLIT_STREAMS][SONG_BLOCK];
    int16_t *st[LRV_OPN_SPLIT_STREAMS] = { buf[0], buf[1], buf[2], buf[3] };
    struct core_ref r;
    uint32_t h = PCM_HASH0;
    int b, k, i;

    r.ops = ops;
    r.core = ops->create(hal, LRV_OPN_CLOCK_PC98, want, flags, rate);
    CHECK(r.core != 0);
    if (!r.core)
        return 0;
    *peak = 0;
    for (b = 0; b < SONG_BLOCKS; b++) {
        song_block(b, core_write, &r);
        if (split) {
            ops->render_split(r.core, st, SONG_BLOCK);
            for (k = 0; k < LRV_OPN_SPLIT_STREAMS; k++)
                h = pcm_hash(h, buf[k], SONG_BLOCK);
        } else {
            ops->render(r.core, buf[0], SONG_BLOCK);
            h = pcm_hash(h, buf[0], SONG_BLOCK);
        }
        for (i = 0; i < SONG_BLOCK; i++)
            if (buf[0][i] > *peak)
                *peak = buf[0][i];
    }
    ops->destroy(r.core);
    return h;
}

/* The checksums below were taken from the cores as they were before they became part of this
 * library, playing the same sequence: the rendered samples must stay bit-identical. */
static void test_cores(void)
{
    struct lrv_hal hal;
    uint32_t rate = 0;
    int peak = 0;

    th_hal(&hal);
    CHECK(core_sum(&hal, &lrv_opn_lite_ops, 22050, 0, 0, &rate, &peak) == 0xB79BB439u);
    CHECK(rate == 22050 && peak > 4000);                /* and it is not silence */
    CHECK(core_sum(&hal, &lrv_opn_lite_ops, 22050, LRV_OPN_OVERSAMPLE, 0, &rate, &peak) ==
          0x9296483Bu);
    CHECK(core_sum(&hal, &lrv_opn_lite_ops, 11025, 0, 0, &rate, &peak) == 0xE78567B6u);
    CHECK(rate == 11025);
    /* more than the chip's own rate is not produced; less than 8 kHz neither */
    CHECK(core_sum(&hal, &lrv_opn_lite_ops, 60000, 0, 0, &rate, &peak) == 0xACCBAE42u);
    CHECK(rate == LRV_OPN_CLOCK_PC98 / LRV_OPN_FM_DIV);
    CHECK(core_sum(&hal, &lrv_opn_lite_ops, 22050, 0, 1, &rate, &peak) == 0x71F1E23Fu);
    CHECK(core_sum(&hal, &lrv_opn_lite_ops, 22050, LRV_OPN_OVERSAMPLE, 1, &rate, &peak) ==
          0xE0DB272Fu);
    /* the same again: nothing is left behind between instances */
    CHECK(core_sum(&hal, &lrv_opn_lite_ops, 22050, 0, 0, &rate, &peak) == 0xB79BB439u);
    CHECK(th_live == 0);
    CHECK(lrv_opn_select(1) == &lrv_opn_lite_ops);
#ifdef LRV_HAVE_YMFM
    CHECK(lrv_opn_select(0) == &lrv_opn_ymfm_ops);
    CHECK(core_sum(&hal, &lrv_opn_ymfm_ops, 22050, 0, 0, &rate, &peak) == 0x9A036301u);
    CHECK(rate == LRV_OPN_CLOCK_PC98 / LRV_OPN_FM_DIV && peak > 4000);
    CHECK(core_sum(&hal, &lrv_opn_ymfm_ops, 0, 0, 0, &rate, &peak) == 0x9A036301u);
    CHECK(th_live == 0);
#else
    CHECK(lrv_opn_select(0) == &lrv_opn_lite_ops);
#endif
}

/* a reset core plays like a new one, and two instances do not share anything */
static void test_core_reset(void)
{
    static int16_t a[2000], b[2000], c[2000];
    struct lrv_hal hal;
    struct core_ref r1, r2;
    uint32_t rate;
    int blk;

    th_hal(&hal);
    r1.ops = r2.ops = &lrv_opn_lite_ops;
    r1.core = r1.ops->create(&hal, LRV_OPN_CLOCK_PC98, 16000, 0, &rate);
    r2.core = r2.ops->create(&hal, LRV_OPN_CLOCK_PC98, 16000, 0, &rate);
    CHECK(r1.core && r2.core);
    if (r1.core && r2.core) {
        for (blk = 0; blk < 20; blk++) {
            song_block(blk, core_write, &r1);
            r1.ops->render(r1.core, a + blk * 100, 100);
        }
        r1.ops->reset(r1.core);
        for (blk = 0; blk < 20; blk++) {
            song_block(blk, core_write, &r1);
            song_block(blk, core_write, &r2);
            r1.ops->render(r1.core, b + blk * 100, 100);
            r2.ops->render(r2.core, c + blk * 100, 100);
        }
        CHECK(!memcmp(a, b, sizeof a) && !memcmp(a, c, sizeof a));
        r1.ops->destroy(r1.core);
        r2.ops->destroy(r2.core);
    }
    CHECK(th_live == 0);
}

/* ---- the queue: timing with a core that records ------------------------------------------- */

#define FK_MAX 64
static struct { uint64_t pos; uint8_t reg, val; } fk_log[FK_MAX];
static int fk_n, fk_resets, fk_creates;
static uint64_t fk_frames;              /* samples rendered by the current core */

struct fk_core {
    const struct lrv_hal *hal;
};

static struct lrv_opn *fk_create(const struct lrv_hal *hal, uint32_t clock_hz, uint32_t want,
                                 unsigned flags, uint32_t *rate)
{
    struct fk_core *c = lrv_alloc(hal, sizeof *c);
    (void)clock_hz;
    (void)flags;
    if (!c)
        return 0;
    c->hal = hal;
    *rate = want;
    fk_frames = 0;
    fk_n = 0;
    fk_creates++;
    return (struct lrv_opn *)c;
}

static void fk_destroy(struct lrv_opn *c)
{
    lrv_free(((struct fk_core *)c)->hal, c, sizeof(struct fk_core));
}

static void fk_reset(struct lrv_opn *c)
{
    (void)c;
    fk_resets++;
    fk_frames = 0;
    fk_n = 0;
}

static void fk_write(struct lrv_opn *c, uint8_t reg, uint8_t val)
{
    (void)c;
    if (fk_n < FK_MAX) {
        fk_log[fk_n].pos = fk_frames;
        fk_log[fk_n].reg = reg;
        fk_log[fk_n].val = val;
    }
    fk_n++;
}

/* the samples are their own index + 1: a block shows where it was rendered */
static void fk_render(struct lrv_opn *c, int16_t *mono, int frames)
{
    int i;
    (void)c;
    for (i = 0; i < frames; i++)
        mono[i] = (int16_t)(fk_frames + (uint64_t)i + 1);
    fk_frames += (uint64_t)frames;
}

static const struct lrv_opn_ops fk_ops = {
    "fake", fk_create, fk_destroy, fk_reset, fk_write, fk_render, 0
};

static void fk_cfg(struct lrv_opnq_cfg *cfg, uint32_t rate, uint32_t queue)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->ops = &fk_ops;
    cfg->clock_hz = LRV_OPN_CLOCK_PC98;
    cfg->rate = rate;
    cfg->bus_clocks = LRV_OPN_FM_DIV;
    cfg->queue_size = queue;
}

/* renders `total` samples in pieces of `piece` */
static void render_pieces(struct lrv_opnq *q, int total, int piece)
{
    static int16_t buf[4096];
    while (total > 0) {
        int n = total < piece ? total : piece;
        lrv_opnq_render(q, buf, n);
        total -= n;
    }
}

static void test_queue_clocked(void)
{
    struct lrv_hal hal;
    struct lrv_opnq q;
    struct lrv_opnq_cfg cfg;
    static const int pieces[] = { 1, 7, 100, 4096 };
    uint32_t cost;
    int i, p;

    th_hal(&hal);
    fk_cfg(&cfg, 48000, 32);
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK);
    CHECK(lrv_opnq_rate(&q) == 48000);
    /* a write keeps the bus for one FM sample: 48000 / 55466 of an output sample */
    cost = (uint32_t)(((uint64_t)48000 << 16) / (LRV_OPN_CLOCK_PC98 / 72));
    CHECK(q.cost == cost && cost == 56714);

    /* the same schedule whatever the sizes of the blocks asked for */
    for (p = 0; p < 4; p++) {
        lrv_opnq_reset(&q);
        /* five writes of one interrupt at clock 0, one in the middle of a second, two more at
         * an odd clock */
        for (i = 0; i < 5; i++)
            lrv_opnq_write_at(&q, 0, (uint8_t)i, 0);
        lrv_opnq_write_at(&q, LRV_OPN_CLOCK_PC98 / 2, 0x10, 0);
        lrv_opnq_write_at(&q, 1000000, 0x20, 0);
        lrv_opnq_write_at(&q, 1000000, 0x21, 0);
        CHECK(lrv_opnq_pending(&q) == 8);
        /* the middle one was queued before two earlier ones: the queue is in arrival order
         * and a write is never applied before those queued ahead of it */
        render_pieces(&q, 30000, pieces[p]);
        CHECK(fk_n == 8 && lrv_opnq_pending(&q) == 0);
        for (i = 0; i < 5; i++)                 /* i FM samples after the first, rounded down */
            CHECK(fk_log[i].pos == ((uint64_t)i * cost) >> 16 && fk_log[i].reg == i);
        CHECK(fk_log[1].pos == 0 && fk_log[2].pos == 1 && fk_log[4].pos == 3);
        CHECK(fk_log[5].pos == 24000);          /* half a second in: exactly sample 24000 */
        /* overdue behind it: at once, but still a bus slot apart */
        CHECK(fk_log[6].pos == 24000 && fk_log[7].pos == 24001);
        CHECK(fk_frames == 30000 && q.samples == 30000);
    }

    /* a clock falls on the sample floor(clock * rate / clock_hz), counted from clock0 */
    lrv_opnq_reset(&q);
    lrv_opnq_set_clock0(&q, 500);
    lrv_opnq_write_at(&q, 400, 1, 0);           /* before sample 0: at sample 0 */
    lrv_opnq_write_at(&q, 500 + 1000000, 2, 0); /* 1000000 * 48000 / 3993600 = 12019.2 */
    lrv_opnq_write_at(&q, 500 + 83, 3, 0);      /* out of order: after the one before it */
    render_pieces(&q, 12020, 512);
    CHECK(fk_n == 2 && fk_log[0].pos == 0 && fk_log[1].pos == 12019);
    render_pieces(&q, 1, 1);
    CHECK(fk_n == 3 && fk_log[2].pos == 12020);
    lrv_opnq_free(&q);

    /* at a low output rate several writes fit into one output sample: 55466 / 8000 = 6.9 */
    fk_cfg(&cfg, 8000, 32);
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK);
    for (i = 0; i < 16; i++)
        lrv_opnq_write_at(&q, 0, (uint8_t)i, 0);
    render_pieces(&q, 8, 3);
    CHECK(fk_n == 16 && fk_log[6].pos == 0 && fk_log[7].pos == 1 && fk_log[13].pos == 1 &&
          fk_log[14].pos == 2);
    lrv_opnq_free(&q);

    /* no bus spacing: all at the sample of their clock */
    fk_cfg(&cfg, 48000, 32);
    cfg.bus_clocks = 0;
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK);
    for (i = 0; i < 8; i++)
        lrv_opnq_write_at(&q, LRV_OPN_CLOCK_PC98 / 100, (uint8_t)i, 0);
    render_pieces(&q, 500, 100);
    CHECK(fk_n == 8 && fk_log[0].pos == 480 && fk_log[7].pos == 480);
    lrv_opnq_free(&q);
    CHECK(th_live == 0);
}

static void test_queue_framed(void)
{
    static struct fseq fs;
    static int16_t buf[256];
    struct lrv_hal hal;
    struct lrv_opnq q;
    struct lrv_opnq_cfg cfg;
    int i;

    th_hal(&hal);
    fk_cfg(&cfg, 22050, 8);
    cfg.seq.ctx = &fs;
    cfg.seq.advance = fseq_advance;
    fseq_init(&fs, 0, 0);
    fs.b = SONG_BLOCKS;                         /* a driver with no events: one run a block */
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK);
    CHECK(q.cost == (uint32_t)(((uint64_t)22050 << 16) / 55466));   /* 0.3975 samples */

    /* writes made now take effect at the sample the rendering has reached, a bus slot apart:
     * three fit into a sample at this rate */
    lrv_opnq_render(&q, buf, 100);
    for (i = 0; i < 7; i++)
        lrv_opnq_write(&q, (uint8_t)i, 0);
    CHECK(lrv_opnq_pending(&q) == 7 && fk_n == 0);
    lrv_opnq_render(&q, buf, 50);
    CHECK(fk_n == 7 && fk_log[0].pos == 100 && fk_log[2].pos == 100 && fk_log[3].pos == 101 &&
          fk_log[5].pos == 101 && fk_log[6].pos == 102);
    CHECK(buf[0] == 101 && buf[49] == 150);     /* the block is whole and in order */
    /* a later write waits for nothing once the bus is free again */
    lrv_opnq_write(&q, 0x30, 0);
    lrv_opnq_render(&q, buf, 10);
    CHECK(fk_n == 8 && fk_log[7].pos == 150);

    /* a skipped block: time and writes go on, the core is not run, the block is silence */
    lrv_opnq_write(&q, 0x31, 0);
    buf[0] = buf[9] = 99;
    lrv_opnq_skip(&q, buf, 10);
    CHECK(fk_n == 9 && buf[0] == 0 && buf[9] == 0 && fk_frames == 160 && q.samples == 170);
    lrv_opnq_render(&q, buf, 4);
    CHECK(buf[0] == 161);
    lrv_opnq_free(&q);
    CHECK(th_live == 0);
}

static void test_queue_edges(void)
{
    static int16_t buf[64];
    struct lrv_hal hal;
    struct lrv_opnq q;
    struct lrv_opnq_cfg cfg;
    int i;

    th_hal(&hal);
    /* a full queue (size 4 holds 3): the oldest write goes to the core early ... */
    fk_cfg(&cfg, 48000, 4);
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK);
    lrv_opnq_render(&q, buf, 10);
    for (i = 0; i < 5; i++)
        lrv_opnq_write_at(&q, 4000000, (uint8_t)i, 0);
    CHECK(fk_n == 2 && fk_log[0].reg == 0 && fk_log[1].reg == 1 && fk_log[1].pos == 10);
    CHECK(lrv_opnq_pending(&q) == 3);
    lrv_opnq_free(&q);
    /* ... or, with the flag, everything queued does and the bus is free */
    cfg.flags = LRV_OPNQ_FLUSH_ON_FULL;
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK);
    for (i = 0; i < 5; i++)
        lrv_opnq_write(&q, (uint8_t)i, 0);
    CHECK(fk_n == 3 && lrv_opnq_pending(&q) == 2 && q.bus == 0);
    lrv_opnq_render(&q, buf, 4);
    CHECK(fk_n == 5 && fk_log[3].pos == 0);

    /* a write past the queue; the chip's reset; another rate */
    lrv_opnq_poke(&q, 0x07, 0xBF);
    CHECK(fk_n == 6 && fk_log[5].pos == 4 && fk_log[5].val == 0xBF);
    lrv_opnq_write(&q, 1, 1);
    fk_resets = 0;
    lrv_opnq_reset(&q);
    CHECK(fk_resets == 1 && lrv_opnq_pending(&q) == 0 && q.samples == 0);
    fk_creates = 0;
    CHECK(lrv_opnq_set_rate(&q, 44100) == 44100 && fk_creates == 1);
    CHECK(q.cost == (uint32_t)(((uint64_t)44100 << 16) / 55466));
    lrv_opnq_free(&q);
    CHECK(th_live == 0);

    /* no chip: time passes in silence, writes vanish */
    fk_cfg(&cfg, 44100, 4);
    cfg.ops = 0;
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK && lrv_opnq_rate(&q) == 44100);
    lrv_opnq_write(&q, 1, 1);
    CHECK(lrv_opnq_pending(&q) == 0);
    buf[0] = buf[63] = 5;
    lrv_opnq_render(&q, buf, 64);
    CHECK(buf[0] == 0 && buf[63] == 0 && q.samples == 64);
    lrv_opnq_free(&q);
    /* no rate either (audio is off) */
    fk_cfg(&cfg, 0, 4);
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK && lrv_opnq_rate(&q) == 0);
    buf[0] = 5;
    lrv_opnq_render(&q, buf, 64);
    CHECK(buf[0] == 0);
    lrv_opnq_free(&q);

    /* configurations that cannot work */
    fk_cfg(&cfg, 48000, 1);
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_ERR_RANGE);
    fk_cfg(&cfg, 48000, 8);
    cfg.clock_hz = 0;
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_ERR_RANGE);
    lrv_opnq_free(&q);                          /* harmless after a failed init */
    CHECK(th_live == 0);
}

/* ---- the queue with the real cores: the two kinds of driver ------------------------------- */

static void q_write_at(void *ctx, uint64_t clock, uint8_t reg, uint8_t val)
{
    lrv_opnq_write_at(ctx, clock, reg, val);
}

static void q_write(void *ctx, uint8_t reg, uint8_t val)
{
    lrv_opnq_write(ctx, reg, val);
}

/* One and a half seconds of the song through a queue and a mixer at `volume`. kind 0: the
 * clock-driven driver; 1: the frame-driven one; skip_every > 0 skips every n-th block. */
static uint32_t song_sum(const struct lrv_opn_ops *ops, uint32_t want, int kind, int volume,
                         int skip_every)
{
    static int16_t buf[2048];
    static struct cseq cs;
    static struct fseq fs;
    struct lrv_hal hal;
    struct lrv_opnq q;
    struct lrv_opnq_cfg cfg;
    struct lrv_mixer mx;
    struct lrv_mixer_cfg mcfg;
    uint32_t h = PCM_HASH0, rate;
    uint64_t total = 0;
    int i = 0;

    th_hal(&hal);
    memset(&cfg, 0, sizeof cfg);
    cfg.ops = ops;
    cfg.clock_hz = LRV_OPN_CLOCK_PC98;
    cfg.rate = want;
    cfg.bus_clocks = LRV_OPN_FM_DIV;
    if (kind == 0) {
        cfg.queue_size = 2048;
        cfg.seq.ctx = &cs;
        cfg.seq.run_until = cseq_run_until;
    } else {
        cfg.queue_size = 1024;
        cfg.flags = LRV_OPNQ_FLUSH_ON_FULL;
        cfg.seq.ctx = &fs;
        cfg.seq.advance = fseq_advance;
    }
    CHECK(lrv_opnq_init(&q, &hal, &cfg) == LRV_OK);
    rate = lrv_opnq_rate(&q);
    if (kind == 0) {
        cseq_init(&cs, q_write_at, &q);
        lrv_opnq_set_clock0(&q, CSEQ_START);
    } else {
        fseq_init(&fs, q_write, &q);
    }
    memset(&mcfg, 0, sizeof mcfg);
    mcfg.render = lrv_opnq_render;
    mcfg.skip = lrv_opnq_skip;
    mcfg.ctx = &q;
    mcfg.gain_floor = kind;         /* each as the engine with that kind of driver rounds */
    lrv_mixer_init(&mx, &hal, &mcfg);
    lrv_mixer_set_volume(&mx, volume);
    lrv_mixer_start(&mx, rate);
    while (rate && total < (uint64_t)rate * 3 / 2) {
        int n = chunk_size(i);
        if (skip_every && i % skip_every == skip_every - 1)
            lrv_mixer_skip(&mx, buf, n);        /* a backend that fell behind */
        else
            lrv_mixer_render(&mx, buf, n);
        i++;
        h = pcm_hash(h, buf, n);
        total += (uint64_t)n;
    }
    lrv_mixer_close(&mx);
    lrv_opnq_free(&q);
    CHECK(th_live == 0);
    return h;
}

/* The checksums are those of the render loops the queue replaces (one stamping writes with
 * the chip's clock, one counting samples), run with the same drivers before they were
 * merged: the queue must reproduce both sample for sample. */
static void test_queue_song(void)
{
    CHECK(song_sum(&lrv_opn_lite_ops, 22050, 0, 100, 0) == 0xFABF684Cu);
    CHECK(song_sum(&lrv_opn_lite_ops, 22050, 0, 50, 0) == 0xF7860EFDu);
    CHECK(song_sum(&lrv_opn_lite_ops, 22050, 1, 100, 0) == 0x46A7F986u);
    CHECK(song_sum(&lrv_opn_lite_ops, 22050, 1, 50, 0) == 0xE2523A75u);
    CHECK(song_sum(&lrv_opn_lite_ops, 22050, 1, 100, 5) == 0xD88C2929u);
#ifdef LRV_HAVE_YMFM
    CHECK(song_sum(&lrv_opn_ymfm_ops, 22050, 0, 100, 0) == 0x1A78F7AFu);
    CHECK(song_sum(&lrv_opn_ymfm_ops, 22050, 1, 100, 0) == 0x0C3B059Cu);
#endif
}

/* ---- the mixer with a fake sink ------------------------------------------------------------- */

static struct {
    int accept;                         /* the rate audio_open answers, 0 = no audio */
    int asked;                          /* the rate it was asked for */
    struct lrv_audio_source src;        /* what audio_open was given */
    int space;                          /* what audio_space reports */
    int16_t pushed[8192];
    int npushed, pushes, closed;
    int depth, max_depth, locks;        /* the audio lock */
    int vol_takes;                      /* set_volume answers this */
    int vol_which, vol_percent;
} fa;

static int fa_open(int rate, const struct lrv_audio_source *source)
{
    fa.asked = rate;
    fa.src = *source;
    return fa.accept;
}

static void fa_close(void) { fa.closed++; }
static int fa_space(void) { return fa.space; }

static void fa_push(const int16_t *pcm, int frames)
{
    if (fa.npushed + frames <= (int)(sizeof fa.pushed / sizeof fa.pushed[0]))
        memcpy(fa.pushed + fa.npushed, pcm, sizeof *pcm * (size_t)frames);
    fa.npushed += frames;
    fa.pushes++;
    fa.space -= frames;
}

static void fa_lock(void)
{
    fa.locks++;
    if (++fa.depth > fa.max_depth)
        fa.max_depth = fa.depth;
}

static void fa_unlock(void) { fa.depth--; }

static int fa_set_volume(int which, int percent)
{
    fa.vol_which = which;
    fa.vol_percent = percent;
    return fa.vol_takes;
}

static void fa_hal(struct lrv_hal *hal, int pull)
{
    th_hal(hal);
    memset(&fa, 0, sizeof fa);
    hal->audio_open = fa_open;
    hal->audio_close = fa_close;
    if (pull) {
        hal->audio_lock = fa_lock;
        hal->audio_unlock = fa_unlock;
    } else {
        hal->audio_space = fa_space;
        hal->audio_push = fa_push;
    }
}

static struct {
    int16_t val;                        /* the main source's constant sample */
    int16_t aux;                        /* what the auxiliary source adds */
    int calls, frames, idles;
    int skips, skipped;                 /* calls of the skip hook, and their frames */
    uint32_t aux_rate;
} src;

static void src_render(void *ctx, int16_t *mono, int frames)
{
    int i;
    (void)ctx;
    for (i = 0; i < frames; i++)
        mono[i] = src.val;
    src.calls++;
    src.frames += frames;
}

static void src_skip(void *ctx, int16_t *mono, int frames)
{
    (void)ctx;
    memset(mono, 0, sizeof *mono * (size_t)frames);
    src.skips++;
    src.skipped += frames;
}

static void src_aux(void *ctx, int16_t *mono, int frames, uint32_t rate)
{
    int i;
    (void)ctx;
    for (i = 0; i < frames; i++)
        mono[i] = lrv_sat16((int32_t)mono[i] + src.aux);
    src.aux_rate = rate;
}

static void src_idle(void *ctx)
{
    (void)ctx;
    src.idles++;
}

static void src_cfg(struct lrv_mixer_cfg *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    memset(&src, 0, sizeof src);
    cfg->render = src_render;
    cfg->aux = src_aux;
    cfg->idle = src_idle;
}

static void test_mixer_push(void)
{
    static struct lrv_mixer mx;
    struct lrv_hal hal;
    struct lrv_mixer_cfg cfg;

    fa_hal(&hal, 0);
    src_cfg(&cfg);
    lrv_mixer_init(&mx, &hal, &cfg);

    /* no device yet: the pump is the idle clock, the mixer renders silence */
    lrv_mixer_pump(&mx);
    CHECK(src.idles == 1 && src.calls == 0 && !lrv_mixer_has_device(&mx));

    fa.accept = 0;                              /* the backend has no audio */
    CHECK(lrv_mixer_open(&mx, 22050) == 0 && !lrv_mixer_has_device(&mx));
    fa.accept = 24000;                          /* it takes another rate than asked for */
    CHECK(lrv_mixer_open(&mx, 22050) == 24000 && fa.asked == 22050);
    CHECK(lrv_mixer_has_device(&mx) && lrv_mixer_rate(&mx) == 0);

    /* open but not started: nothing is rendered or pushed */
    fa.space = 100;
    lrv_mixer_pump(&mx);
    CHECK(fa.npushed == 0 && src.calls == 0);

    lrv_mixer_start(&mx, 0);
    CHECK(lrv_mixer_rate(&mx) == 24000);
    src.val = 1000;
    src.aux = 5;
    lrv_mixer_pump(&mx);
    CHECK(fa.npushed == 100 && fa.pushes == 1 && src.frames == 100 && src.idles == 1);
    CHECK(fa.pushed[0] == 1005 && fa.pushed[99] == 1005 && src.aux_rate == 24000);
    lrv_mixer_pump(&mx);                        /* the sink is full */
    CHECK(fa.npushed == 100);
    /* more than a block: pushed in blocks, the room asked for once */
    fa.space = 2 * LRV_MIXER_BLOCK + 10;
    lrv_mixer_pump(&mx);
    CHECK(fa.npushed == 100 + 2 * LRV_MIXER_BLOCK + 10 && fa.pushes == 4 && fa.space == 0);

    /* the volume: scaled by the mixer, rounding towards zero ... */
    fa.npushed = 0;
    lrv_mixer_set_volume(&mx, 50);
    src.val = -3;
    src.aux = 0;
    fa.space = 1;
    lrv_mixer_pump(&mx);
    CHECK(fa.pushed[0] == -1);                  /* -3 * 128 / 256 */
    src.val = 30000;
    src.aux = 10000;                            /* the aux source is not scaled with it */
    fa.space = 1;
    lrv_mixer_pump(&mx);
    CHECK(fa.pushed[1] == 25000);
    lrv_mixer_set_volume(&mx, -5);              /* silence, and the aux source still heard */
    fa.space = 1;
    lrv_mixer_pump(&mx);
    CHECK(fa.pushed[2] == 10000);
    lrv_mixer_set_volume(&mx, 250);             /* clamped */
    CHECK(mx.volume == 100 && mx.gain == 256);

    /* saturation of the mix, both ways */
    src.val = 30000;
    fa.space = 1;
    lrv_mixer_pump(&mx);
    CHECK(fa.pushed[3] == 32767);
    src.val = -30000;
    src.aux = -10000;
    fa.space = 1;
    lrv_mixer_pump(&mx);
    CHECK(fa.pushed[4] == -32768);

    /* without the aux source */
    lrv_mixer_set_aux(&mx, 0, 0);
    fa.space = 1;
    lrv_mixer_pump(&mx);
    CHECK(fa.pushed[5] == -30000);

    lrv_mixer_close(&mx);
    CHECK(fa.closed == 1 && !lrv_mixer_has_device(&mx) && lrv_mixer_rate(&mx) == 0);
    lrv_mixer_close(&mx);
    CHECK(fa.closed == 1);

    /* ... or down, as the engine asks; and the backend applying the volume itself */
    cfg.gain_floor = 1;
    lrv_mixer_init(&mx, &hal, &cfg);
    hal.set_volume = fa_set_volume;
    lrv_mixer_start(&mx, 8000);                 /* no device: a renderer */
    lrv_mixer_set_volume(&mx, 50);
    CHECK(fa.vol_which == LRV_VOL_MAIN && fa.vol_percent == 50 && mx.gain == 128);
    src.val = -3;
    src.aux = 0;
    lrv_mixer_render(&mx, fa.pushed, 2);
    CHECK(fa.pushed[0] == -2 && fa.pushed[1] == -2 && src.aux_rate == 8000);
    fa.vol_takes = 1;
    lrv_mixer_set_volume(&mx, 40);
    CHECK(fa.vol_percent == 40 && mx.gain == 256 && mx.volume == 40);
    lrv_mixer_render(&mx, fa.pushed, 1);
    CHECK(fa.pushed[0] == -3);
    lrv_mixer_close(&mx);
}

static void test_mixer_pull(void)
{
    static struct lrv_mixer mx;
    static int16_t buf[64];
    struct lrv_hal hal;
    struct lrv_mixer_cfg cfg;

    fa_hal(&hal, 1);
    src_cfg(&cfg);
    lrv_mixer_init(&mx, &hal, &cfg);
    fa.accept = 44100;
    CHECK(lrv_mixer_open(&mx, 44100) == 44100 && fa.src.render != 0 && fa.src.skip != 0);

    /* the backend may pull as soon as the stream is open: silence until the start */
    src.val = 77;
    buf[0] = buf[63] = 1;
    fa.src.render(fa.src.ctx, buf, 64);
    CHECK(buf[0] == 0 && buf[63] == 0 && src.calls == 0);
    lrv_mixer_start(&mx, 0);
    fa.locks = 0;
    fa.src.render(fa.src.ctx, buf, 64);
    CHECK(buf[0] == 77 && buf[63] == 77 && src.frames == 64);
    CHECK(fa.locks == 1 && fa.depth == 0);      /* the callback holds the lock for the block */

    /* the pump has nothing to do, with a device or (idle) without */
    lrv_mixer_pump(&mx);
    CHECK(src.frames == 64 && src.idles == 0);

    /* the engine's own calls are bracketed; the lock is never taken twice */
    lrv_mixer_lock(&mx);
    CHECK(fa.depth == 1);
    lrv_mixer_unlock(&mx);
    lrv_mixer_set_volume(&mx, 50);
    lrv_mixer_set_aux(&mx, 0, 0);
    fa.src.render(fa.src.ctx, buf, 4);
    CHECK(buf[0] == 38 && fa.depth == 0 && fa.max_depth == 1);

    /* a backend that fell behind skips a block: without a skip hook the source is rendered
     * all the same (its time must pass), with one the hook is called in its place. The block
     * is the hook's silence, unscaled, and whatever the auxiliary source adds; the lock is
     * taken once, by the mixer */
    src.frames = 0;
    fa.locks = 0;
    fa.src.skip(fa.src.ctx, buf, 8);
    CHECK(buf[0] == 38 && buf[7] == 38 && src.frames == 8 && src.skips == 0);
    CHECK(fa.locks == 1 && fa.depth == 0);
    lrv_mixer_lock(&mx);
    mx.cfg.skip = src_skip;
    lrv_mixer_unlock(&mx);
    lrv_mixer_set_aux(&mx, src_aux, 0);
    src.aux = 5;
    fa.locks = 0;
    fa.src.skip(fa.src.ctx, buf, 8);
    CHECK(buf[0] == 5 && buf[7] == 5 && src.frames == 8 && src.skips == 1 && src.skipped == 8);
    CHECK(fa.locks == 1 && fa.depth == 0 && fa.max_depth == 1);
    fa.src.render(fa.src.ctx, buf, 4);          /* and the next block is rendered again */
    CHECK(buf[0] == 43 && src.frames == 12 && src.skips == 1);
    lrv_mixer_skip(&mx, buf, 2);                /* the same without the lock, for a renderer */
    CHECK(buf[0] == 5 && src.skipped == 10 && fa.depth == 0);

    lrv_mixer_close(&mx);
    CHECK(fa.closed == 1 && fa.max_depth == 1);
    /* a late callback of a backend that is slow to stop: silence, the source is not called */
    src.calls = src.skips = 0;
    fa.src.render(fa.src.ctx, buf, 4);
    CHECK(buf[0] == 0 && src.calls == 0);
    fa.src.skip(fa.src.ctx, buf, 4);
    CHECK(buf[0] == 0 && src.skips == 0);
}

/* ---- the voice player ----------------------------------------------------------------------- */

#define CLIP_N 3000
static struct th_builder vpack;
static int8_t clip_data[CLIP_N];

/* the voice pack: its "voice" entry, a pcm8 ramp at 8 kHz, and an entry that is no clip */
static void voice_pack(const char *meta)
{
    static uint8_t clip[LRV_VDEC_PCM8_HEADER + CLIP_N];
    int i;
    memcpy(clip, "pcm8", 4);
    lrv_wr32le(clip + 4, 8000);
    lrv_wr32le(clip + 8, CLIP_N);
    for (i = 0; i < CLIP_N; i++) {
        clip_data[i] = (int8_t)(1 + i * 120 / CLIP_N);      /* never 0: an end is seen */
        clip[LRV_VDEC_PCM8_HEADER + i] = (uint8_t)clip_data[i];
    }
    th_pack_begin(&vpack, 3);
    th_pack_add(&vpack, "voice", LRV_PT_META, 0, meta, (uint32_t)strlen(meta));
    th_pack_add(&vpack, "LINE1", LRV_PT_VOICE, 0, clip, sizeof clip);
    th_pack_add(&vpack, "BROKEN", LRV_PT_VOICE, 0, "nope", 4);
    th_pack_end(&vpack, LRV_PACK_VOICE);
}

static struct {
    int ev[32], tag[32], n;
    char clip[24];
} evs;

static void on_event(void *ctx, int ev, const char *clip, int tag)
{
    (void)ctx;
    if (evs.n < 32) {
        evs.ev[evs.n] = ev;
        evs.tag[evs.n] = tag;
    }
    evs.n++;
    snprintf(evs.clip, sizeof evs.clip, "%s", clip);
}

static void voice_cfg(struct lrv_voice_cfg *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    memset(&evs, 0, sizeof evs);
    cfg->volume = 100;
    cfg->event = on_event;
}

/* the clip through the mixer's aux path at `rate`, pumping once per block as a frame does;
 * returns the samples that came out before the clip ended */
static int play_mixed(struct lrv_voice *v, uint32_t rate, int16_t *out, int cap, int16_t under)
{
    int n = 0, i, guard = 0;
    while (lrv_voice_playing(v) && guard++ < 1000) {
        int16_t blk[160];
        for (i = 0; i < 160; i++)
            blk[i] = under;
        lrv_voice_mix(v, blk, 160, rate);
        for (i = 0; i < 160 && n < cap; i++)
            if (blk[i] != under)
                out[n++] = blk[i];
        lrv_voice_pump(v);
    }
    return n;
}

static void test_voice_mix(void)
{
    static struct lrv_voice v;
    static int16_t out[3 * CLIP_N];
    struct lrv_hal hal;
    struct lrv_voice_cfg cfg;
    char val[16];
    int i, n, ok;

    /* no auxiliary stream, no voice pack, a codec this build does not have */
    fa_hal(&hal, 1);
    voice_cfg(&cfg);
    voice_pack("codec=pcm8\nrate=8000\ncues=abc123\n");
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_ERR_NOTFOUND && !lrv_voice_enabled(&v));
    hal.aux_mode = LRV_AUX_MIX;
    th_pack[LRV_PACK_VOICE] = 0;
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_ERR_NOTFOUND);
    voice_pack("codec=mp7\nrate=8000\n");
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_ERR_FORMAT && !strcmp(v.codec, "mp7"));
    voice_pack("codec=pcm8\n");                 /* no rate and no default */
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_ERR_FORMAT);
    cfg.default_rate = 22050;
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_OK && v.pack_rate == 22050);
    lrv_voice_close(&v);
    CHECK(th_live == 0);
    /* a player that is off does nothing */
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_ERR && !lrv_voice_playing(&v));
    lrv_voice_pump(&v);
    lrv_voice_stop(&v);

    voice_pack("codec=pcm8\r\nrate=8000\r\ncues=abc123\r\n");
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_OK && lrv_voice_enabled(&v));
    CHECK(v.pack_rate == 8000 && !strcmp(v.codec, "pcm8"));
    CHECK(lrv_voice_meta(&v, "cues", val, sizeof val) == 6 && !strcmp(val, "abc123"));
    CHECK(lrv_voice_meta(&v, "cue", val, sizeof val) == 0 && val[0] == 0);
    CHECK(lrv_voice_play(&v, "nosuch", 0) == LRV_ERR_NOTFOUND);
    CHECK(lrv_voice_play(&v, "broken", 0) == LRV_ERR_FORMAT && !lrv_voice_playing(&v));
    CHECK(evs.n == 0);

    /* at the clip's own rate the samples come out as they are, all of them, then the end */
    fa.locks = 0;
    CHECK(lrv_voice_play(&v, "line1", 7) == LRV_OK && lrv_voice_playing(&v));
    CHECK(evs.n == 1 && evs.ev[0] == LRV_VOICE_EV_START && evs.tag[0] == 7);
    CHECK(!strcmp(evs.clip, "line1"));
    n = play_mixed(&v, 8000, out, 3 * CLIP_N, 0);
    ok = n == CLIP_N;
    for (i = 0; i < CLIP_N && ok; i++)
        ok = out[i] == clip_data[i] * 256;
    CHECK(ok);
    CHECK(evs.n == 2 && evs.ev[1] == LRV_VOICE_EV_END && evs.tag[1] == 7);
    CHECK(fa.locks > 0 && fa.depth == 0 && fa.max_depth == 1);  /* the ring is shared: locked */

    /* at twice the rate: twice the samples, interpolated (a ramp stays monotonic) */
    CHECK(lrv_voice_play(&v, "LINE1", 0) == LRV_OK);
    n = play_mixed(&v, 16000, out, 3 * CLIP_N, 0);
    CHECK(n >= 2 * CLIP_N - 4 && n <= 2 * CLIP_N + 4);
    for (i = 1, ok = 1; i < 2 * CLIP_N - 4 && ok; i++)
        ok = out[i] >= out[i - 1] && out[i] - out[i - 1] <= 256;
    CHECK(ok && out[0] == clip_data[0] * 256);
    CHECK(evs.n == 4 && evs.ev[3] == LRV_VOICE_EV_END);

    /* the volume scales the clip; the mix saturates */
    lrv_voice_set_volume(&v, 50);
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_OK);
    n = play_mixed(&v, 8000, out, 3 * CLIP_N, 0);
    CHECK(n == CLIP_N && out[0] == clip_data[0] * 128 && out[CLIP_N - 1] == 120 * 128);
    lrv_voice_set_volume(&v, 100);
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_OK);
    n = play_mixed(&v, 8000, out, 3 * CLIP_N, 20000);
    CHECK(n == CLIP_N && out[0] == 20256 && out[CLIP_N - 1] == 32767);

    /* a cut: at once, with its event; a clip started over another cuts it */
    CHECK(lrv_voice_play(&v, "line1", 1) == LRV_OK);
    CHECK(lrv_voice_play(&v, "line1", 2) == LRV_OK);
    CHECK(evs.ev[evs.n - 2] == LRV_VOICE_EV_CUT && evs.tag[evs.n - 2] == 1);
    lrv_voice_stop(&v);
    CHECK(!lrv_voice_playing(&v) && evs.ev[evs.n - 1] == LRV_VOICE_EV_CUT);
    out[0] = 0;
    lrv_voice_mix(&v, out, 1, 8000);
    CHECK(out[0] == 0);
    CHECK(v.n_ev[LRV_VOICE_EV_START] == 6 && v.n_ev[LRV_VOICE_EV_END] == 4 &&
          v.n_ev[LRV_VOICE_EV_CUT] == 2);
    lrv_voice_close(&v);
    CHECK(th_live == 0 && fa.depth == 0 && fa.max_depth == 1);
}

/* no audio stream: the clip plays out silently, in the time it would take to hear it; and the
 * guard cuts a clip nothing consumes */
static void test_voice_silent(void)
{
    static struct lrv_voice v;
    struct lrv_hal hal;
    struct lrv_voice_cfg cfg;
    int pumps;

    fa_hal(&hal, 0);
    hal.aux_mode = LRV_AUX_MIX;
    voice_pack("codec=pcm8\nrate=8000\n");
    voice_cfg(&cfg);
    cfg.silent_num = 5642;                      /* 56.42 frames a second */
    cfg.silent_den = 100;
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_OK);
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_OK);
    for (pumps = 0; lrv_voice_playing(&v) && pumps < 100; pumps++)
        lrv_voice_pump(&v);
    CHECK(pumps == 22);                         /* 3000 samples at 141.8 a frame */
    CHECK(evs.n == 2 && evs.ev[1] == LRV_VOICE_EV_END);
    lrv_voice_close(&v);

    voice_cfg(&cfg);
    cfg.stall_frames = 5;
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_OK);
    CHECK(lrv_voice_play(&v, "line1", 3) == LRV_OK);
    th_last_log[0] = 0;
    for (pumps = 0; lrv_voice_playing(&v) && pumps < 100; pumps++)
        lrv_voice_pump(&v);
    CHECK(pumps == 6 && evs.n == 2 && evs.ev[1] == LRV_VOICE_EV_CUT);
    CHECK(strstr(th_last_log, "no progress") != 0);
    /* without the guard the player waits, and the engine can watch the progress itself */
    lrv_voice_close(&v);
    voice_cfg(&cfg);
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_OK);
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_OK);
    for (pumps = 0; pumps < 50; pumps++)
        lrv_voice_pump(&v);
    CHECK(lrv_voice_playing(&v) && lrv_voice_progress(&v) == 0);
    lrv_voice_mix(&v, (int16_t[10]){ 0 }, 10, 8000);
    CHECK(lrv_voice_progress(&v) == 10);
    lrv_voice_close(&v);
    CHECK(th_live == 0);
}

/* ---- the voice on the backend's own channel ------------------------------------------------ */

static struct {
    int accept, asked;
    int queued, cap;                    /* frames the fake holds / can hold */
    long total;                         /* frames pushed */
    int ends, flushes, closes, after_end;
    int16_t first;
} fx;

static int fx_open(int rate)
{
    fx.asked = rate;
    return fx.accept;
}

static void fx_close(void) { fx.closes++; }
static int fx_space(void) { return fx.cap - fx.queued; }
static int fx_queued(void) { return fx.queued; }

static void fx_push(const int16_t *pcm, int frames)
{
    if (frames == 0) {
        fx.ends++;
        return;
    }
    if (fx.ends)
        fx.after_end++;
    if (fx.total == 0)
        fx.first = pcm[0];
    fx.queued += frames;
    fx.total += frames;
}

static void fx_flush(void)
{
    fx.flushes++;
    fx.queued = 0;
}

static void test_voice_channel(void)
{
    static struct lrv_voice v;
    struct lrv_hal hal;
    struct lrv_voice_cfg cfg;
    int frames, play;

    fa_hal(&hal, 1);
    memset(&fx, 0, sizeof fx);
    hal.aux_mode = LRV_AUX_CHANNEL;
    hal.aux_open = fx_open; hal.aux_close = fx_close; hal.aux_space = fx_space;
    hal.aux_push = fx_push; hal.aux_queued = fx_queued; hal.aux_flush = fx_flush;
    hal.set_volume = fa_set_volume;
    voice_pack("codec=pcm8\nrate=8000\n");
    voice_cfg(&cfg);
    cfg.volume = 60;
    cfg.stall_frames = 20;

    fx.accept = 0;                              /* the backend cannot open its output */
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_ERR_IO && th_live == 0);
    fx.accept = 12000;
    fx.cap = 4000;
    fa.vol_takes = 1;                           /* ... and applies the volume itself */
    CHECK(lrv_voice_open(&v, &hal, &cfg) == LRV_OK && fx.asked == 8000);
    CHECK(fa.vol_which == LRV_VOL_AUX && fa.vol_percent == 60 && v.gain == 256);

    /* a device playing 200 frames a frame: the clip is resampled to its rate, fed ahead of
     * it, ended once, and over when the device has played everything */
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_OK);
    play = 200;
    for (frames = 0; lrv_voice_playing(&v) && frames < 500; frames++) {
        lrv_voice_pump(&v);
        if (frames == 0)
            CHECK(fx.queued == 4000);           /* as much as it had room for */
        fx.queued -= fx.queued < play ? fx.queued : play;
    }
    CHECK(fx.total >= CLIP_N * 3 / 2 - 4 && fx.total <= CLIP_N * 3 / 2 + 4);
    CHECK(fx.first == clip_data[0] * 256);      /* unscaled: the backend has the volume */
    CHECK(fx.ends == 1 && fx.after_end == 0 && fx.flushes == 0);
    CHECK(frames >= (int)(fx.total / play) && frames <= (int)(fx.total / play) + 3);
    CHECK(evs.n == 2 && evs.ev[1] == LRV_VOICE_EV_END);
    CHECK(fa.locks == 0);                       /* one thread: the audio lock is not used */

    /* a cut drops what the backend still holds */
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_OK);
    lrv_voice_pump(&v);
    CHECK(fx.queued > 0);
    lrv_voice_stop(&v);
    CHECK(fx.flushes == 1 && fx.queued == 0 && evs.ev[evs.n - 1] == LRV_VOICE_EV_CUT);

    /* a device that plays nothing: the guard ends the clip */
    CHECK(lrv_voice_play(&v, "line1", 0) == LRV_OK);
    for (frames = 0; lrv_voice_playing(&v) && frames < 500; frames++)
        lrv_voice_pump(&v);
    CHECK(frames > 20 && frames < 30 && evs.ev[evs.n - 1] == LRV_VOICE_EV_CUT);
    CHECK(fx.flushes == 2);
    lrv_voice_close(&v);
    CHECK(fx.closes == 1 && th_live == 0);
}

int main(void)
{
    test_cores();
    test_core_reset();
    test_queue_clocked();
    test_queue_framed();
    test_queue_edges();
    test_queue_song();
    test_mixer_push();
    test_mixer_pull();
    test_voice_mix();
    test_voice_silent();
    test_voice_channel();
    if (th_fails) {
        fprintf(stderr, "test_snd: %d checks failed\n", th_fails);
        return 1;
    }
    printf("test_snd: ok\n");
    return 0;
}
