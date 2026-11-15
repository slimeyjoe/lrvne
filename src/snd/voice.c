/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The voice clip player (lrvne/voice.h): decode-ahead into a ring, a linear resampler, and
 * the two outputs (mixed into the music stream, or pushed to the backend's aux channel). */
#include <string.h>
#include "lrvne/voice.h"
#include "lrvne/hal.h"
#include "lrvne/mixer.h"

#define RING  LRV_VOICE_RING
#define LEAD  LRV_VOICE_LEAD
#define CHUNK LRV_VOICE_CHUNK

/* MIX: the ring and the play state are shared with the renderer */
static void lock(const struct lrv_voice *v)
{
    if (v->mode == LRV_AUX_MIX && v->hal->audio_lock)
        v->hal->audio_lock();
}

static void unlock(const struct lrv_voice *v)
{
    if (v->mode == LRV_AUX_MIX && v->hal->audio_unlock)
        v->hal->audio_unlock();
}

/* value of `key=` in a META text entry of a pack, or "" */
static int meta_get(const struct lrv_pack *pk, const char *entry, const char *key, char *out,
                    int cap)
{
    const struct lrv_pack_entry *e = lrv_pack_find(pk, entry, LRV_PT_META);
    uint8_t *d;
    uint32_t n, i, kl = (uint32_t)strlen(key);
    int k = 0;

    out[0] = 0;
    if (!e || !(d = lrv_pack_load(pk, e, &n)))
        return 0;
    for (i = 0; i + kl < n; i++) {
        if ((i == 0 || d[i - 1] == '\n') && !memcmp(d + i, key, kl) && d[i + kl] == '=') {
            /* a separate index, not the loop's: one compiler for the 68k miscompiled the
             * loop variable being advanced in here and copied the whole line */
            uint32_t j = i + kl + 1;
            while (j < n && d[j] != '\n' && d[j] != '\r' && k < cap - 1)
                out[k++] = (char)d[j++];
            out[k] = 0;
            break;
        }
    }
    lrv_pack_free(pk, d, n);
    return k;
}

int lrv_voice_meta(const struct lrv_voice *v, const char *key, char *out, int cap)
{
    if (cap <= 0)
        return 0;
    out[0] = 0;
    return v->on ? meta_get(&v->pak, "voice", key, out, cap) : 0;
}

void lrv_voice_set_volume(struct lrv_voice *v, int percent)
{
    if (percent <= 0 || percent > 100)
        percent = 100;
    v->gain = percent * 256 / 100;
    if (v->mode == LRV_AUX_CHANNEL && v->hal->set_volume &&
        v->hal->set_volume(LRV_VOL_AUX, percent))
        v->gain = 256;              /* the backend's output applies it */
}

int lrv_voice_open(struct lrv_voice *v, const struct lrv_hal *hal,
                   const struct lrv_voice_cfg *cfg)
{
    char rate_s[12];
    int rate = 0, i, rc;

    memset(v, 0, sizeof *v);
    v->hal = hal;
    v->cfg = *cfg;
    if (hal->aux_mode != LRV_AUX_MIX && hal->aux_mode != LRV_AUX_CHANNEL)
        return LRV_ERR_NOTFOUND;
    if (hal->aux_mode == LRV_AUX_CHANNEL &&
        (!hal->aux_open || !hal->aux_space || !hal->aux_push || !hal->aux_queued))
        return LRV_ERR_NOTFOUND;
    rc = lrv_pack_open(&v->pak, hal, LRV_PACK_VOICE);
    if (rc != LRV_OK)
        return rc;
    meta_get(&v->pak, "voice", "codec", v->codec, sizeof v->codec);
    meta_get(&v->pak, "voice", "rate", rate_s, sizeof rate_s);
    for (i = 0; rate_s[i] >= '0' && rate_s[i] <= '9' && rate < 1000000; i++)
        rate = rate * 10 + (rate_s[i] - '0');
    if (rate <= 0)
        rate = cfg->default_rate;
    v->pack_rate = rate;
    rc = LRV_ERR_FORMAT;
    if (!lrv_vdec_supports(v->codec) || rate <= 0)
        goto fail;
    v->mode = hal->aux_mode;
    if (v->mode == LRV_AUX_CHANNEL) {
        int got = hal->aux_open(rate);
        rc = LRV_ERR_IO;
        if (got <= 0)
            goto fail;
        v->ch_rate = (uint32_t)got;
    }
    v->ring = lrv_alloc(hal, RING * sizeof v->ring[0]);
    if (!v->ring) {
        if (v->mode == LRV_AUX_CHANNEL && hal->aux_close)
            hal->aux_close();
        rc = LRV_ERR_NOMEM;
        goto fail;
    }
    lrv_vdec_init(&v->dec);
    lrv_voice_set_volume(v, cfg->volume);
    v->on = 1;
    return LRV_OK;
fail:
    lrv_pack_close(&v->pak);
    v->mode = LRV_AUX_NONE;
    return rc;
}

int lrv_voice_enabled(const struct lrv_voice *v)
{
    return v->on;
}

/* ---- playback ---------------------------------------------------------------------------- */

static uint32_t queued(const struct lrv_voice *v)
{
    return v->rhead - v->rtail;
}

/* decodes up to `want` samples into the ring (the engine's thread) */
static void fill(struct lrv_voice *v, uint32_t want)
{
    int16_t *tmp = v->tmp;
    while (!v->dec_done && want > 0) {
        int n, i;
        uint32_t room;
        lock(v);
        room = RING - queued(v);
        unlock(v);
        if (room == 0)
            break;
        n = (int)(want < CHUNK ? want : CHUNK);
        if ((uint32_t)n > room)
            n = (int)room;
        n = lrv_vdec_read(&v->dec, tmp, n);
        if (n <= 0) {
            v->dec_done = 1;
            break;
        }
        if (v->gain != 256)
            lrv_pcm_gain(tmp, n, v->gain, 1);
        lock(v);
        for (i = 0; i < n; i++)
            v->ring[(v->rhead + (uint32_t)i) & (RING - 1)] = tmp[i];
        v->rhead += (uint32_t)n;
        unlock(v);
        want -= (uint32_t)n;
    }
}

static void event(struct lrv_voice *v, int ev)
{
    v->n_ev[ev]++;
    if (v->cfg.event)
        v->cfg.event(v->cfg.ctx, ev, v->clip, v->tag);
}

/* the clip is over or cut: back to idle */
static void halt(struct lrv_voice *v, int ev)
{
    int was = v->playing;
    lock(v);
    v->playing = 0;
    v->rhead = v->rtail = 0;
    v->primed = 0;
    unlock(v);
    if (v->mode == LRV_AUX_CHANNEL && v->hal->aux_flush && ev == LRV_VOICE_EV_CUT)
        v->hal->aux_flush();
    if (was)
        event(v, ev);
}

void lrv_voice_stop(struct lrv_voice *v)
{
    if (v->on && v->playing)
        halt(v, LRV_VOICE_EV_CUT);
}

int lrv_voice_playing(const struct lrv_voice *v)
{
    return v->on && v->playing;
}

int lrv_voice_play(struct lrv_voice *v, const char *clip, int tag)
{
    const struct lrv_pack_entry *e;
    int rc;

    if (!v->on)
        return LRV_ERR;
    lrv_voice_stop(v);
    e = lrv_pack_find(&v->pak, clip, LRV_PT_VOICE);
    if (!e)
        return LRV_ERR_NOTFOUND;
    rc = lrv_vdec_open(&v->dec, &v->pak, e);
    if (rc != LRV_OK)
        return rc;
    strncpy(v->clip, clip, sizeof v->clip - 1);
    v->clip[sizeof v->clip - 1] = 0;
    v->tag = tag;
    v->dec_done = 0;
    v->end_sent = 0;
    v->silent_acc = 0;
    v->still = 0;
    v->last_progress = 0;
    lock(v);
    v->rhead = v->rtail = 0;
    v->primed = 0;
    v->step_rate = 0;
    unlock(v);
    fill(v, LEAD);                  /* audible from the first render on */
    lock(v);
    v->playing = 1;
    unlock(v);
    event(v, LRV_VOICE_EV_START);
    return LRV_OK;
}

/* the clip is decoded and the resampler has output all of it, its last step faded to zero
 * (the caller holds the lock in MIX mode) */
static int drained(const struct lrv_voice *v)
{
    return v->dec_done && queued(v) == 0 && (!v->primed || (v->s0 == 0 && v->s1 == 0));
}

/* the next output sample through the resampler (the caller holds the lock in MIX mode) */
static int next_out(struct lrv_voice *v, uint32_t rate, int16_t *out)
{
    if (v->step_rate != rate) {
        v->step = (uint32_t)(((uint64_t)v->dec.rate << 16) / (rate ? rate : 1));
        v->step_rate = rate;
    }
    if (v->primed && drained(v))
        return 0;                   /* the clip is over (no endless silence) */
    if (v->step == 0x10000u) {      /* the output plays the clip's rate: copy, no interpolation */
        if (queued(v) == 0)
            return 0;               /* drained, or an underrun (the caller stops either way) */
        *out = v->ring[v->rtail++ & (RING - 1)];
        return 1;
    }
    if (!v->primed) {
        if (queued(v) < 2) {
            if (v->dec_done)
                v->rtail = v->rhead;    /* a last lone sample: drop it, the clip is over */
            return 0;
        }
        v->s0 = v->ring[v->rtail++ & (RING - 1)];
        v->s1 = v->ring[v->rtail++ & (RING - 1)];
        v->frac = 0;
        v->primed = 1;
    }
    *out = (int16_t)(v->s0 +
                     (int16_t)(((int32_t)(v->s1 - v->s0) * (int32_t)(v->frac >> 1)) >> 15));
    v->frac += v->step;
    while (v->frac >= 0x10000u) {
        v->frac -= 0x10000u;
        v->s0 = v->s1;
        if (queued(v) > 0)
            v->s1 = v->ring[v->rtail++ & (RING - 1)];
        else if (v->dec_done)
            v->s1 = 0;              /* the clip has run out: fade the last step to zero */
        else
            break;                  /* underrun: hold */
    }
    return 1;
}

void lrv_voice_mix(void *ctx, int16_t *mono, int frames, uint32_t rate)
{
    struct lrv_voice *v = ctx;
    int i;
    if (!v->on || v->mode != LRV_AUX_MIX || !v->playing)
        return;
    for (i = 0; i < frames; i++) {
        int16_t s;
        if (!next_out(v, rate, &s))
            break;
        mono[i] = lrv_sat16((int32_t)mono[i] + s);
    }
}

uint32_t lrv_voice_progress(const struct lrv_voice *v)
{
    uint32_t p = v->rtail;
    if (v->mode == LRV_AUX_CHANNEL)
        p -= (uint32_t)v->hal->aux_queued();
    return p;
}

/* the guard: 1 when the output made no progress for cfg.stall_frames pumps in a row */
static int stalled(struct lrv_voice *v)
{
    uint32_t now;
    if (v->cfg.stall_frames <= 0)
        return 0;
    now = lrv_voice_progress(v);
    if (now != v->last_progress) {
        v->last_progress = now;
        v->still = 0;
        return 0;
    }
    if (++v->still <= v->cfg.stall_frames)
        return 0;
    lrv_log(v->hal, LRV_LOG_WARN, "voice: %s made no progress for %d frames, cut", v->clip,
            v->cfg.stall_frames);
    return 1;
}

static void pump_channel(struct lrv_voice *v)
{
    const struct lrv_hal *hal = v->hal;
    int16_t *tmp = v->tmp;          /* fill() is done with it */
    uint32_t lead_out = (uint32_t)(((uint64_t)LEAD * v->ch_rate) /
                                   (v->dec.rate ? v->dec.rate : 1));
    if (hal->aux_lead_ms > 0) {
        uint32_t want = (uint32_t)(((uint64_t)v->ch_rate * (uint32_t)hal->aux_lead_ms) / 1000);
        if (want > lead_out)
            lead_out = want;
    }
    while ((uint32_t)hal->aux_queued() < lead_out) {
        int n = 0, space = hal->aux_space();
        if (space <= 0)
            break;
        if (space > CHUNK)
            space = CHUNK;
        while (n < space && next_out(v, v->ch_rate, &tmp[n]))
            n++;
        if (n == 0)
            break;
        hal->aux_push(tmp, n);
    }
    /* the end, once nothing more will be pushed: the backend plays out what it holds back (a
     * resampler's last frames). Pushing after it would leave those frames held for good, and
     * the clip would never end. */
    if (drained(v) && !v->end_sent) {
        v->end_sent = 1;
        hal->aux_push(tmp, 0);
    }
    if (v->end_sent && hal->aux_queued() == 0)
        halt(v, LRV_VOICE_EV_END);
    else if (stalled(v))
        halt(v, LRV_VOICE_EV_CUT);
}

void lrv_voice_pump(void *ctx)
{
    struct lrv_voice *v = ctx;
    uint32_t have;
    int over;

    if (!v->on || !v->playing)
        return;
    lock(v);
    have = queued(v);
    unlock(v);
    fill(v, LEAD - (have < LEAD ? have : LEAD));
    if (v->mode == LRV_AUX_CHANNEL) {
        pump_channel(v);
        return;
    }
    if (v->cfg.silent_num) {
        /* no audio stream: the clip plays out silently in real time */
        uint32_t n;
        v->silent_acc += v->dec.rate * v->cfg.silent_den;
        n = v->silent_acc / v->cfg.silent_num;
        v->silent_acc %= v->cfg.silent_num;
        if (n > queued(v))
            n = queued(v);
        v->rtail += n;
    }
    lock(v);
    over = drained(v);
    unlock(v);
    if (over)
        halt(v, LRV_VOICE_EV_END);
    else if (stalled(v))
        halt(v, LRV_VOICE_EV_CUT);
}

void lrv_voice_close(struct lrv_voice *v)
{
    const struct lrv_hal *hal = v->hal;
    if (!v->on)
        return;
    lrv_voice_stop(v);
    lrv_vdec_close(&v->dec);
    if (v->mode == LRV_AUX_CHANNEL && hal->aux_close)
        hal->aux_close();
    lrv_pack_close(&v->pak);
    lrv_free(hal, v->ring, RING * sizeof v->ring[0]);
    memset(v, 0, sizeof *v);
    v->hal = hal;
}
