/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Opus voice clips (lrvne/vdec.h has the layout), decoded by the vendored libopus
 * (third_party/opus: decoder only, fixed point). Built with LRV_HAVE_OPUS.
 *
 * One packet is decoded at a time into pcm[]; the decoder's state is allocated through the
 * pack's HAL at the first clip and reused for every clip after it. */
#ifdef LRV_HAVE_OPUS
#include <string.h>
#include <opus.h>
#include "lrvne/vdec.h"
#include "lrvne/endian.h"

int lrv_vdec_opus_open(struct lrv_vdec_opus *d, struct lrv_vdec_in *in)
{
    uint8_t h[16];
    uint32_t rate;
    int size;

    if (in->e->size < 16 || lrv_vdec_in_read(in, 0, h, 16) != 16 || memcmp(h, "opus", 4) != 0)
        return LRV_ERR_FORMAT;
    rate = lrv_rd32be(h + 4);
    if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000)
        return LRV_ERR_FORMAT;              /* 48000 too: pcm[] holds 120 ms at 24 kHz */
    if (!d->od) {
        size = opus_decoder_get_size(1);
        if (size <= 0)
            return LRV_ERR_FORMAT;
        d->hal = in->pk->hal;
        d->od = lrv_alloc(d->hal, (size_t)size);
        if (!d->od)
            return LRV_ERR_NOMEM;
        d->od_size = (uint32_t)size;
        d->od_rate = 0;
    }
    if (d->od_rate != rate) {
        if (opus_decoder_init((OpusDecoder *)d->od, (opus_int32)rate, 1) != OPUS_OK)
            return LRV_ERR_FORMAT;
        d->od_rate = rate;
    } else if (opus_decoder_ctl((OpusDecoder *)d->od, OPUS_RESET_STATE) != OPUS_OK) {
        return LRV_ERR_FORMAT;
    }
    d->in = in;
    d->rate = rate;
    d->left = lrv_rd32be(h + 8);
    d->skip = lrv_rd16be(h + 12);
    d->pos = 16;
    d->pcm_n = d->pcm_i = 0;
    return LRV_OK;
}

/* decodes the next packet into pcm[]; 0 at the end of the entry or on a bad packet */
static int next_packet(struct lrv_vdec_opus *d)
{
    uint32_t size = d->in->e->size, len;
    uint8_t h[2];
    int n;

    if (d->pos > size || size - d->pos < 2 || lrv_vdec_in_read(d->in, d->pos, h, 2) != 2)
        return 0;
    len = lrv_rd16be(h);
    if (len > sizeof d->pkt || len > size - d->pos - 2)
        return 0;
    if (len && lrv_vdec_in_read(d->in, d->pos + 2, d->pkt, len) != len)
        return 0;
    d->pos += 2 + len;
    n = opus_decode((OpusDecoder *)d->od, d->pkt, (opus_int32)len, d->pcm, LRV_VDEC_OPUS_FRAME,
                    0);
    if (n < 0)
        return 0;
    d->pcm_n = n;
    d->pcm_i = 0;
    if (d->skip) {
        uint32_t k = d->skip < (uint32_t)n ? d->skip : (uint32_t)n;
        d->pcm_i = (int)k;
        d->skip -= k;
    }
    return 1;
}

int lrv_vdec_opus_read(struct lrv_vdec_opus *d, int16_t *out, int n)
{
    int k = 0;
    while (k < n && d->left) {
        uint32_t m;
        if (d->pcm_i >= d->pcm_n) {
            if (!next_packet(d)) {
                d->left = 0;
                break;
            }
            continue;
        }
        m = (uint32_t)(d->pcm_n - d->pcm_i);
        if (m > (uint32_t)(n - k))
            m = (uint32_t)(n - k);
        if (m > d->left)
            m = d->left;
        memcpy(out + k, d->pcm + d->pcm_i, m * sizeof out[0]);
        d->pcm_i += (int)m;
        k += (int)m;
        d->left -= m;
    }
    return k;
}

void lrv_vdec_opus_close(struct lrv_vdec_opus *d)
{
    if (d->od)
        lrv_free(d->hal, d->od, d->od_size);
    d->od = 0;
    d->od_rate = 0;
}
#else
typedef int lrv_vdec_opus_unused;           /* ISO C: no empty translation unit */
#endif
