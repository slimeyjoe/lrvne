/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ldp4 / ldp6 voice clips (lrvne/vdec.h has the layout): a delta codec whose 4- or 6-bit codes
 * index a table of steps, roughly exponential in both directions, with code 0 for a run of
 * the same sample. Decoding is a table lookup and an addition per sample. */
#include <string.h>
#include "lrvne/vdec.h"
#include "lrvne/endian.h"

/* the steps of the two code widths; entry 0 is the run code */
static const int16_t tab4[16] = {
    0, 8, 26, 86, 283, 927, 3043, 9986, -32768, -9986, -3043, -927, -283, -86, -26, -8
};
static const int16_t tab6[64] = {
    0, 1, 2, 3, 4, 5, 6, 7, 10, 15, 20, 29, 40, 56, 78, 109,
    153, 214, 299, 419, 586, 819, 1145, 1601, 2240, 3132, 4380, 6126, 8567, 11980, 16754, 23431,
    -32768, -23431, -16754, -11980, -8567, -6126, -4380, -3132,
    -2240, -1601, -1145, -819, -586, -419, -299, -214,
    -153, -109, -78, -56, -40, -29, -20, -15, -10, -7, -6, -5, -4, -3, -2, -1
};

/* next block header; 0 at the end of the entry */
static int next_block(struct lrv_vdec_ldp *d)
{
    uint32_t size = d->in->e->size;
    uint8_t h[4];
    d->pos += d->blk_bytes;                 /* skip what the previous block did not use */
    if (d->pos > size || size - d->pos < 4 || lrv_vdec_in_read(d->in, d->pos, h, 4) != 4)
        return 0;
    d->pos += 4;
    d->blk_bytes = lrv_rd16be(h);
    d->blk_samples = lrv_rd16be(h + 2);
    if (d->blk_bytes > size - d->pos)
        d->blk_bytes = size - d->pos;
    d->bitbuf = 0;
    d->nbits = 0;
    d->buf_n = d->buf_i = 0;
    d->acc = 0;
    return 1;
}

static unsigned next_byte(struct lrv_vdec_ldp *d)
{
    if (d->buf_i >= d->buf_n) {
        uint32_t n = d->blk_bytes < sizeof d->buf ? d->blk_bytes : (uint32_t)sizeof d->buf;
        if (n == 0)
            return 0;                       /* past the block: zero bits */
        d->buf_n = (int)lrv_vdec_in_read(d->in, d->pos, d->buf, n);
        d->buf_i = 0;
        if (d->buf_n <= 0) {
            d->blk_bytes = 0;
            return 0;
        }
        d->pos += (uint32_t)d->buf_n;
        d->blk_bytes -= (uint32_t)d->buf_n;
    }
    return d->buf[d->buf_i++];
}

static unsigned code(struct lrv_vdec_ldp *d)
{
    while (d->nbits < d->bits) {
        d->bitbuf = (d->bitbuf << 8) | next_byte(d);
        d->nbits += 8;
    }
    d->nbits -= d->bits;
    return (d->bitbuf >> d->nbits) & ((1u << d->bits) - 1);
}

int lrv_vdec_ldp_open(struct lrv_vdec_ldp *d, struct lrv_vdec_in *in)
{
    uint8_t h[12];
    memset(d, 0, sizeof *d);
    if (in->e->size < 12 || lrv_vdec_in_read(in, 0, h, 12) != 12 || memcmp(h, "ldp", 3) != 0)
        return LRV_ERR_FORMAT;
    if (h[3] == '4') {
        d->bits = 4;
        d->table = tab4;
    } else if (h[3] == '6') {
        d->bits = 6;
        d->table = tab6;
    } else {
        return LRV_ERR_FORMAT;
    }
    d->in = in;
    d->rate = lrv_rd32be(h + 4);
    d->left = lrv_rd32be(h + 8);
    d->pos = 12;
    return d->rate ? LRV_OK : LRV_ERR_FORMAT;
}

int lrv_vdec_ldp_read(struct lrv_vdec_ldp *d, int16_t *out, int n)
{
    int k = 0;
    while (k < n) {
        unsigned c, run;
        if (d->pend) {                      /* a run that did not fit the previous call */
            uint32_t m = d->pend < (uint32_t)(n - k) ? d->pend : (uint32_t)(n - k);
            d->pend -= m;
            while (m--)
                out[k++] = (int16_t)d->acc;
            continue;
        }
        if (d->left == 0)
            break;
        if (d->blk_samples == 0) {
            if (!next_block(d))
                d->left = 0;
            continue;
        }
        c = code(d);
        if (c) {
            d->acc = (uint16_t)(d->acc + (uint16_t)d->table[c]);
            run = 1;
        } else {
            run = code(d) + 1;
        }
        if (run > d->blk_samples)
            run = d->blk_samples;
        if (run > d->left)
            run = d->left;
        d->blk_samples -= run;
        d->left -= run;
        d->pend = run;
    }
    return k;
}
