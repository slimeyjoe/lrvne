/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The music path (lrvne/music.h): the core chosen and the device's rate agreed, the fallback
 * to the light core, time passing without a device, offline rendering, push and pull, with a
 * toy driver of each kind (frame- and clock-driven). */
#include "test_hal.h"
#include "lrvne/music.h"
#include "lrvne/opn.h"

/* ---- a pretend audio device -------------------------------------------------------------- */

static struct {
    int accept;                         /* the rate audio_open answers; 0 = no audio */
    int asked, opens, closed;
    int space, npushed;
    int depth, max_depth, locks;
    struct lrv_audio_source src;
} fa;

static int fa_open(int rate, const struct lrv_audio_source *source)
{
    fa.asked = rate;
    fa.opens++;
    fa.src = *source;
    return fa.accept;
}

static void fa_close(void) { fa.closed++; }
static int fa_space(void) { return fa.space; }

static void fa_push(const int16_t *pcm, int frames)
{
    (void)pcm;
    fa.npushed += frames;
    fa.space -= frames;
}

static void fa_lock(void)
{
    fa.locks++;
    if (++fa.depth > fa.max_depth)
        fa.max_depth = fa.depth;
}

static void fa_unlock(void) { fa.depth--; }

static void fa_hal(struct lrv_hal *hal, int pull, int accept)
{
    th_hal(hal);
    memset(&fa, 0, sizeof fa);
    fa.accept = accept;
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

/* ---- toy drivers ------------------------------------------------------------------------- */

static struct {
    struct lrv_opnq *q;
    long frames;                        /* frame-driven: samples advanced */
    uint64_t clock;                     /* clock-driven: the last clock run to */
    int calls;
} drv;

/* frame-driven: keys a note on at the start, then lets the block render */
static void toy_advance(void *ctx, int frames, void (*span)(void *q, int n), void *q)
{
    (void)ctx;
    if (!drv.frames) {
        lrv_opnq_write(drv.q, 0x28, 0x00);
        lrv_opnq_write(drv.q, 0xA4, 0x22);
        lrv_opnq_write(drv.q, 0xA0, 0x69);
        lrv_opnq_write(drv.q, 0x4C, 0x00);     /* slot 4, the carrier: loud, fast attack */
        lrv_opnq_write(drv.q, 0x5C, 0x1F);
        lrv_opnq_write(drv.q, 0x8C, 0x0F);
        lrv_opnq_write(drv.q, 0x28, 0xF0);
    }
    span(q, frames);
    drv.frames += frames;
    drv.calls++;
}

static void toy_run_until(void *ctx, uint64_t clock)
{
    (void)ctx;
    drv.clock = clock;
    drv.calls++;
}

static void base_cfg(struct lrv_music_cfg *c, int clocked)
{
    memset(c, 0, sizeof *c);
    if (clocked)
        c->seq.run_until = toy_run_until;
    else
        c->seq.advance = toy_advance;
    c->core = 1;                        /* the light C core: renders at the rate asked */
    c->rate = 22050;
    c->clock_hz = LRV_OPN_CLOCK_PC98;
    c->bus_clocks = LRV_OPN_FM_DIV;
    c->queue_size = 64;
    c->queue_flags = LRV_OPNQ_FLUSH_ON_FULL;
    c->idle_rate = 44100;
    c->frame_num = 5642;
    c->frame_den = 100;
}

static void begin(struct lrv_music *m)
{
    memset(&drv, 0, sizeof drv);
    drv.q = &m->q;
}

/* samples the silent clock passes in k frames at `rate` (the same integer arithmetic) */
static long silent_samples(uint32_t rate, int k)
{
    uint32_t inc = (uint32_t)((((uint64_t)rate << 16) * 100u) / 5642u);
    uint64_t acc = 0;
    long n = 0;
    int i;
    for (i = 0; i < k; i++) {
        acc += inc;
        n += (long)(acc >> 16);
        acc &= 0xFFFFu;
    }
    return n;
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_push(void)
{
    static struct lrv_music m;
    struct lrv_hal hal;
    struct lrv_music_cfg c;
    int i;

    fa_hal(&hal, 0, 22050);
    base_cfg(&c, 0);
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    CHECK(fa.opens == 1 && fa.asked == 22050 && lrv_music_rate(&m) == 22050);
    CHECK(lrv_music_has_chip(&m) && m.core_name && !strcmp(m.core_name, "lite"));
    CHECK(lrv_mixer_has_device(&m.mixer) && !m.mixer.pull);

    /* nothing runs before the start */
    fa.space = 500;
    lrv_mixer_pump(&m.mixer);
    CHECK(drv.calls == 0 && fa.npushed == 0);
    lrv_music_start(&m);
    lrv_mixer_pump(&m.mixer);
    CHECK(drv.frames == 500 && fa.npushed == 500);
    /* the device sets the pace, not the frames */
    for (i = 0; i < 10; i++)
        lrv_mixer_pump(&m.mixer);
    CHECK(drv.frames == 500);
    lrv_music_close(&m);
    CHECK(fa.closed == 1 && !lrv_music_has_chip(&m));
    lrv_music_close(&m);                /* twice is harmless */
    CHECK(fa.closed == 1 && th_live == 0);
}

static void test_rate_fallback(void)
{
    static struct lrv_music m;
    struct lrv_hal hal;
    struct lrv_music_cfg c;

    /* the light core follows the device's rate */
    fa_hal(&hal, 0, 24000);
    base_cfg(&c, 0);
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    CHECK(fa.asked == 22050 && lrv_music_rate(&m) == 24000 && lrv_opnq_rate(&m.q) == 24000);
    CHECK(m.core_name && !strcmp(m.core_name, "lite"));
    lrv_music_close(&m);
    CHECK(th_live == 0);

#ifdef LRV_HAVE_YMFM
    /* ymfm renders at clock / 72 whatever is asked: the device is asked for that, and when it
     * insists on another rate the light core takes over at the device's */
    fa_hal(&hal, 0, (int)(LRV_OPN_CLOCK_PC98 / LRV_OPN_FM_DIV));
    base_cfg(&c, 0);
    c.core = 0;
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    CHECK(fa.asked == (int)(LRV_OPN_CLOCK_PC98 / LRV_OPN_FM_DIV));
    CHECK(m.core_name && !strcmp(m.core_name, "ymfm"));
    CHECK(lrv_music_rate(&m) == LRV_OPN_CLOCK_PC98 / LRV_OPN_FM_DIV);
    lrv_music_close(&m);

    fa_hal(&hal, 0, 48000);
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    CHECK(m.core_name && !strcmp(m.core_name, "lite") && lrv_music_rate(&m) == 48000);
    lrv_music_close(&m);
    CHECK(th_live == 0);
#endif
}

static void test_no_device(void)
{
    static struct lrv_music m;
    struct lrv_hal hal;
    struct lrv_music_cfg c;
    int i;

    /* audio off: no device asked for, no core; the frames keep the driver's time */
    fa_hal(&hal, 0, 22050);
    base_cfg(&c, 0);
    c.rate = 0;
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    CHECK(fa.opens == 0 && !lrv_music_has_chip(&m) && lrv_music_rate(&m) == 44100);
    lrv_music_start(&m);
    for (i = 0; i < 1000; i++)
        lrv_mixer_pump(&m.mixer);
    CHECK(drv.frames == silent_samples(44100, 1000));
    lrv_music_close(&m);
    CHECK(fa.closed == 0 && th_live == 0);

    /* the backend has no audio: the same */
    fa_hal(&hal, 0, 0);
    base_cfg(&c, 0);
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    CHECK(fa.opens == 1 && !lrv_music_has_chip(&m) && lrv_music_rate(&m) == 44100);
    lrv_music_start(&m);
    for (i = 0; i < 57; i++)
        lrv_mixer_pump(&m.mixer);
    CHECK(drv.frames == silent_samples(44100, 57));
    lrv_music_close(&m);
    CHECK(th_live == 0);

    /* a clock-driven driver is run up to the clock of the samples passed */
    fa_hal(&hal, 0, 0);
    base_cfg(&c, 1);
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    lrv_music_start(&m);
    for (i = 0; i < 100; i++)
        lrv_mixer_pump(&m.mixer);
    {
        uint64_t n = (uint64_t)silent_samples(44100, 100);
        CHECK(drv.clock == (n * LRV_OPN_CLOCK_PC98 + 44099) / 44100);
    }
    lrv_music_close(&m);

    /* without idle_rate time stands still while there is no device */
    fa_hal(&hal, 0, 0);
    base_cfg(&c, 1);
    c.idle_rate = 0;
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK && lrv_music_rate(&m) == 0);
    lrv_music_start(&m);
    for (i = 0; i < 100; i++)
        lrv_mixer_pump(&m.mixer);
    CHECK(drv.calls == 0);
    lrv_music_close(&m);
    CHECK(th_live == 0);
}

static void test_pull(void)
{
    static struct lrv_music m;
    static int16_t buf[256];
    struct lrv_hal hal;
    struct lrv_music_cfg c;

    fa_hal(&hal, 1, 22050);
    base_cfg(&c, 0);
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK && m.mixer.pull);
    fa.src.render(fa.src.ctx, buf, 256);    /* before the start: silence, the driver idle */
    CHECK(drv.calls == 0);
    lrv_music_start(&m);
    fa.src.render(fa.src.ctx, buf, 256);
    fa.src.skip(fa.src.ctx, buf, 100);      /* a skipped block passes in the driver too */
    CHECK(drv.frames == 356 && fa.max_depth == 1 && fa.depth == 0);
    lrv_mixer_pump(&m.mixer);               /* the backend pulls: the pump does nothing */
    CHECK(drv.frames == 356);
    lrv_music_close(&m);
    CHECK(fa.closed == 1 && th_live == 0);
}

static void test_offline(void)
{
    static struct lrv_music m;
    static int16_t buf[4096];
    struct lrv_hal hal;
    struct lrv_music_cfg c;
    int i, loud = 0;

    fa_hal(&hal, 0, 22050);
    base_cfg(&c, 0);
    c.flags = LRV_MUSIC_OFFLINE;
    c.rate = 0;                         /* offline without a rate: idle_rate */
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK);
    CHECK(fa.opens == 0 && lrv_music_has_chip(&m) && lrv_music_rate(&m) == 44100);
    lrv_music_start(&m);
    lrv_mixer_render(&m.mixer, buf, 4096);
    for (i = 0; i < 4096; i++)
        loud |= buf[i] != 0;
    CHECK(drv.frames == 4096 && loud);  /* the core plays the toy's note */
    lrv_music_close(&m);
    CHECK(fa.closed == 0 && th_live == 0);

    c.rate = 30000;
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_OK && lrv_music_rate(&m) == 30000);
    lrv_music_close(&m);
    CHECK(th_live == 0);
}

static void test_errors(void)
{
    static struct lrv_music m;
    struct lrv_hal hal;
    struct lrv_music_cfg c;

    fa_hal(&hal, 0, 22050);
    base_cfg(&c, 0);
    c.queue_size = 1;                   /* the queue refuses it */
    begin(&m);
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_ERR_RANGE);
    lrv_music_close(&m);
    CHECK(th_live == 0);

    base_cfg(&c, 0);
    c.seq.run_until = toy_run_until;    /* both kinds at once */
    CHECK(lrv_music_open(&m, &hal, &c) == LRV_ERR_RANGE);
    lrv_music_close(&m);
    CHECK(th_live == 0);
}

int main(void)
{
    test_push();
    test_rate_fallback();
    test_no_device();
    test_pull();
    test_offline();
    test_errors();
    if (th_fails) {
        fprintf(stderr, "test_music: %d check(s) failed\n", th_fails);
        return 1;
    }
    printf("test_music: ok\n");
    return 0;
}
