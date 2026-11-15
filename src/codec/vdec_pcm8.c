/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pcm8 voice clips (lrvne/vdec.h): signed 8-bit samples behind a 12-byte header. */
#include <string.h>
#include "lrvne/vdec.h"
#include "lrvne/endian.h"

int lrv_vdec_pcm8_open(struct lrv_vdec_pcm8 *d, struct lrv_vdec_in *in)
{
    uint8_t h[LRV_VDEC_PCM8_HEADER];
    if (lrv_vdec_in_read(in, 0, h, sizeof h) != sizeof h || memcmp(h, "pcm8", 4) != 0)
        return LRV_ERR_FORMAT;
    d->in = in;
    d->rate = lrv_rd32le(h + 4);
    d->left = lrv_rd32le(h + 8);
    d->pos = LRV_VDEC_PCM8_HEADER;
    if (d->rate == 0 || d->left > in->e->size - LRV_VDEC_PCM8_HEADER)
        return LRV_ERR_FORMAT;
    return LRV_OK;
}

int lrv_vdec_pcm8_read(struct lrv_vdec_pcm8 *d, int16_t *out, int n)
{
    uint8_t buf[256];
    int done = 0;
    while (done < n && d->left > 0) {
        uint32_t want = (uint32_t)(n - done), got, i;
        if (want > sizeof buf)
            want = sizeof buf;
        if (want > d->left)
            want = d->left;
        got = (uint32_t)lrv_vdec_in_read(d->in, d->pos, buf, want);
        if (got == 0) {
            d->left = 0;                    /* a short entry: the clip ends here */
            break;
        }
        for (i = 0; i < got; i++)
            out[done + (int)i] = (int16_t)((int8_t)buf[i] * 256);
        d->pos += got;
        d->left -= got;
        done += (int)got;
    }
    return done;
}
