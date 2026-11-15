/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mixer (lrvne/mixer.h): the HAL's audio stream, push and pull delivery, volume. */
#include <string.h>
#include "lrvne/mixer.h"
#include "lrvne/hal.h"

void lrv_pcm_gain(int16_t *pcm, int n, int32_t gain, int floor)
{
    int i;
    if (floor) {
        for (i = 0; i < n; i++)
            pcm[i] = (int16_t)(((int32_t)pcm[i] * gain) >> 8);
    } else {
        for (i = 0; i < n; i++)
            pcm[i] = (int16_t)(((int32_t)pcm[i] * gain) / 256);
    }
}

void lrv_mixer_lock(const struct lrv_mixer *m)
{
    if (m->hal->audio_lock)
        m->hal->audio_lock();
}

void lrv_mixer_unlock(const struct lrv_mixer *m)
{
    if (m->hal->audio_unlock)
        m->hal->audio_unlock();
}

void lrv_mixer_init(struct lrv_mixer *m, const struct lrv_hal *hal,
                    const struct lrv_mixer_cfg *cfg)
{
    memset(m, 0, sizeof *m);
    m->hal = hal;
    m->cfg = *cfg;
    m->volume = 100;
    m->gain = 256;
}

void lrv_mixer_render(struct lrv_mixer *m, int16_t *mono, int frames)
{
    if (frames <= 0)
        return;
    if (!m->live || !m->cfg.render) {
        memset(mono, 0, sizeof *mono * (size_t)frames);
    } else {
        m->cfg.render(m->cfg.ctx, mono, frames);
        if (m->gain != 256)
            lrv_pcm_gain(mono, frames, m->gain, m->cfg.gain_floor);
    }
    if (m->live && m->cfg.aux)
        m->cfg.aux(m->cfg.aux_ctx, mono, frames, m->rate);
}

/* pull mode: the backend's audio thread or interrupt */
static void mixer_pull(void *ctx, int16_t *pcm, int frames)
{
    struct lrv_mixer *m = ctx;
    lrv_mixer_lock(m);
    lrv_mixer_render(m, pcm, frames);
    lrv_mixer_unlock(m);
}

int lrv_mixer_open(struct lrv_mixer *m, int rate)
{
    int got;
    if (m->open || rate <= 0 || !m->hal->audio_open)
        return 0;
    got = m->hal->audio_open(rate, mixer_pull, m);
    if (got <= 0)
        return 0;
    /* a pulling backend may be calling mixer_pull already: it renders silence until
     * lrv_mixer_start, and from here on the mixer's state changes under the lock */
    lrv_mixer_lock(m);
    m->open = 1;
    m->pull = m->hal->audio_lock != 0;
    m->rate = (uint32_t)got;
    lrv_mixer_unlock(m);
    return got;
}

void lrv_mixer_start(struct lrv_mixer *m, uint32_t rate)
{
    lrv_mixer_lock(m);
    if (rate)
        m->rate = rate;
    m->live = 1;
    lrv_mixer_unlock(m);
}

void lrv_mixer_close(struct lrv_mixer *m)
{
    if (!m->hal)
        return;
    lrv_mixer_lock(m);
    m->live = 0;
    lrv_mixer_unlock(m);
    if (m->open && m->hal->audio_close)
        m->hal->audio_close();
    m->open = 0;
    m->pull = 0;
    m->rate = 0;
}

int lrv_mixer_has_device(const struct lrv_mixer *m)
{
    return m->open;
}

uint32_t lrv_mixer_rate(const struct lrv_mixer *m)
{
    return m->live ? m->rate : 0;
}

void lrv_mixer_set_volume(struct lrv_mixer *m, int percent)
{
    int32_t gain;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    if (percent > 0 && m->hal->set_volume && m->hal->set_volume(LRV_VOL_MAIN, percent))
        gain = 256;                     /* the backend's output applies it */
    else
        gain = percent * 256 / 100;
    lrv_mixer_lock(m);
    m->volume = percent;
    m->gain = gain;
    lrv_mixer_unlock(m);
}

void lrv_mixer_set_aux(struct lrv_mixer *m, lrv_mixer_aux_fn *aux, void *ctx)
{
    lrv_mixer_lock(m);
    m->cfg.aux = aux;
    m->cfg.aux_ctx = ctx;
    lrv_mixer_unlock(m);
}

void lrv_mixer_pump(void *ctx)
{
    struct lrv_mixer *m = ctx;
    int space;

    if (!m->open) {
        if (m->cfg.idle)
            m->cfg.idle(m->cfg.idle_ctx);
        return;
    }
    if (m->pull || !m->live || !m->hal->audio_space || !m->hal->audio_push)
        return;
    /* what the sink can take now, asked once: a sink that reports more room than it has
     * cannot hold the frame */
    space = m->hal->audio_space();
    while (space > 0) {
        int n = space > LRV_MIXER_BLOCK ? LRV_MIXER_BLOCK : space;
        lrv_mixer_render(m, m->buf, n);
        m->hal->audio_push(m->buf, n);
        space -= n;
    }
}
