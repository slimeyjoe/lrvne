/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Voice clip decoders (lrvne/vdec.h): the read-ahead window and the dispatcher. */
#include <string.h>
#include "lrvne/vdec.h"
#include "lrvne/hal.h"

size_t lrv_vdec_in_read(struct lrv_vdec_in *in, uint32_t off, void *dst, size_t n)
{
    uint8_t *out = dst;
    size_t done = 0;
    if (!in->buf)                           /* no window (out of memory): read directly */
        return lrv_pack_read(in->pk, in->e, off, dst, n);
    while (done < n) {
        uint32_t at = off + (uint32_t)done, m;
        if (at < in->off || at >= in->off + in->len) {
            uint32_t want;
            if (at >= in->e->size)
                break;
            want = in->e->size - at;
            if (want > LRV_VDEC_READAHEAD)
                want = LRV_VDEC_READAHEAD;
            in->off = at;
            in->len = (uint32_t)lrv_pack_read(in->pk, in->e, at, in->buf, want);
            if (in->len == 0)
                break;
        }
        m = in->off + in->len - at;
        if (m > n - done)
            m = (uint32_t)(n - done);
        memcpy(out + done, in->buf + (at - in->off), m);
        done += m;
    }
    return done;
}

int lrv_vdec_supports(const char *codec)
{
    if (!strcmp(codec, "pcm8") || !strcmp(codec, "ldp6") || !strcmp(codec, "ldp4"))
        return 1;
#ifdef LRV_HAVE_OPUS
    if (!strcmp(codec, "opus"))
        return 1;
#endif
    return 0;
}

void lrv_vdec_init(struct lrv_vdec *d)
{
    memset(d, 0, sizeof *d);
}

int lrv_vdec_open(struct lrv_vdec *d, const struct lrv_pack *pk, const struct lrv_pack_entry *e)
{
    uint8_t m[4];
    int rc = LRV_ERR_FORMAT;

    d->kind = LRV_VDEC_NONE;
    d->rate = 0;
    if (!d->in.buf)
        d->in.buf = lrv_alloc(pk->hal, LRV_VDEC_READAHEAD);     /* may fail: see in_read */
    d->in.pk = pk;
    d->in.e = e;
    d->in.off = d->in.len = 0;              /* a new clip: nothing of it read yet */
    if (e->size < 4 || lrv_vdec_in_read(&d->in, 0, m, 4) != 4)
        return LRV_ERR_FORMAT;
    if (!memcmp(m, "pcm8", 4)) {
        if ((rc = lrv_vdec_pcm8_open(&d->pcm8, &d->in)) == LRV_OK) {
            d->kind = LRV_VDEC_PCM8;
            d->rate = d->pcm8.rate;
        }
    } else if (!memcmp(m, "ldp", 3)) {
        if ((rc = lrv_vdec_ldp_open(&d->ldp, &d->in)) == LRV_OK) {
            d->kind = LRV_VDEC_LDP;
            d->rate = d->ldp.rate;
        }
    }
#ifdef LRV_HAVE_OPUS
    else if (!memcmp(m, "opus", 4)) {
        if ((rc = lrv_vdec_opus_open(&d->opus, &d->in)) == LRV_OK) {
            d->kind = LRV_VDEC_OPUS;
            d->rate = d->opus.rate;
        }
    }
#endif
    return rc;
}

int lrv_vdec_read(struct lrv_vdec *d, int16_t *out, int n)
{
    switch (d->kind) {
    case LRV_VDEC_PCM8:
        return lrv_vdec_pcm8_read(&d->pcm8, out, n);
    case LRV_VDEC_LDP:
        return lrv_vdec_ldp_read(&d->ldp, out, n);
#ifdef LRV_HAVE_OPUS
    case LRV_VDEC_OPUS:
        return lrv_vdec_opus_read(&d->opus, out, n);
#endif
    default:
        return 0;
    }
}

void lrv_vdec_close(struct lrv_vdec *d)
{
    if (d->in.buf)
        lrv_free(d->in.pk->hal, d->in.buf, LRV_VDEC_READAHEAD);
    d->in.buf = 0;
#ifdef LRV_HAVE_OPUS
    lrv_vdec_opus_close(&d->opus);
#endif
    d->kind = LRV_VDEC_NONE;
}
