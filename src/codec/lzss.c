/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Okumura LZSS decoder (lrvne/lzss.h). */
#include <string.h>
#include "lrvne/lzss.h"

#define N         LRV_LZSS_RING
#define F         18                /* the longest match */
#define THRESHOLD 2                 /* matches are longer than this */

size_t lrv_lzss_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap,
                       uint8_t *ring)
{
    unsigned r = N - F;
    unsigned flags = 0;
    size_t i = 0, out = 0;

    memset(ring, 0, N);
    for (;;) {
        flags >>= 1;
        if (!(flags & 0x100)) {             /* the eight items of a flag byte are used up */
            if (i >= srclen)
                break;
            flags = src[i++] | 0xFF00u;
        }
        if (flags & 1) {
            uint8_t c;
            if (i >= srclen)
                break;
            c = src[i++];
            if (out < dstcap)
                dst[out++] = c;
            ring[r] = c;
            r = (r + 1) & (N - 1);
        } else {
            unsigned lo, hi, pos, len, k;
            if (i + 1 >= srclen)
                break;
            lo = src[i];
            hi = src[i + 1];
            i += 2;
            pos = lo | ((hi & 0xF0) << 4);
            len = (hi & 0x0F) + THRESHOLD;
            for (k = 0; k <= len; k++) {
                uint8_t c = ring[(pos + k) & (N - 1)];
                if (out < dstcap)
                    dst[out++] = c;
                ring[r] = c;
                r = (r + 1) & (N - 1);
            }
        }
    }
    return out;
}
