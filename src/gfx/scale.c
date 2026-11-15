/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Integer scaling and the aspect stretch (lrvne/scale.h). */
#include <string.h>
#include "lrvne/scale.h"

void lrv_rowmap_init(struct lrv_rowmap *m, int src_h, int dst_h)
{
    int r;
    if (src_h < 1)
        src_h = 1;
    if (src_h > LRV_ROWMAP_ROWS)
        src_h = LRV_ROWMAP_ROWS;
    if (dst_h < src_h)
        dst_h = src_h;
    if (dst_h > 65535)
        dst_h = 65535;
    m->src_h = src_h;
    m->dst_h = dst_h;
    for (r = 0; r <= src_h; r++)
        m->start[r] = (uint16_t)((long)r * dst_h / src_h);
}

int lrv_aspect_height(const struct lrv_video_mode *mode, int sx, unsigned px_w, unsigned px_h)
{
    /* w * sx pixels across of px_w each, and aspect_h / aspect_w of that tall:
     * h * px_h * aspect_w = w * sx * px_w * aspect_h */
    long long num, den, h, min = (long long)mode->h * sx;
    if (px_w == 0 || px_h == 0)
        px_w = px_h = 1;
    if (mode->aspect_w <= 0 || mode->aspect_h <= 0)
        return (int)min;
    num = (long long)mode->w * sx * (long long)px_w * mode->aspect_h;
    den = (long long)px_h * mode->aspect_w;
    h = (num * 2 + den) / (den * 2);
    return (int)(h <= min + min / 50 ? min : h);
}

void lrv_expand8(const struct lrv_p2c *t, const struct lrv_surface *page, int x0, int y0, int x1,
                 int y1, int scroll, int sx, const struct lrv_rowmap *m, const uint8_t pen[16],
                 uint8_t *dst, size_t pitch)
{
    uint8_t idx[LRV_EXPAND_W];
    int n = (x1 - x0) * 8, w = n * sx, y, i, k;
    uint8_t *d = dst;
    if (n <= 0 || n > LRV_EXPAND_W)
        return;
    for (y = y0; y < y1; y++) {
        int reps = m->start[y + 1] - m->start[y];
        if (reps <= 0)
            continue;
        lrv_p2c8(t, page, x0, y, x1, y + 1, scroll, idx, sizeof idx, 0);
        if (sx == 1) {
            for (i = 0; i < n; i++)
                d[i] = pen[idx[i]];
        } else if (sx == 2) {
            for (i = 0; i < n; i++)
                d[2 * i] = d[2 * i + 1] = pen[idx[i]];
        } else {
            uint8_t *o = d;
            for (i = 0; i < n; i++) {
                uint8_t v = pen[idx[i]];
                for (k = 0; k < sx; k++)
                    *o++ = v;
            }
        }
        for (k = 1; k < reps; k++)
            memcpy(d + (size_t)k * pitch, d, (size_t)w);
        d += (size_t)reps * pitch;
    }
}
