/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Planar to chunky conversion (lrvne/p2c.h). */
#include <string.h>
#include "lrvne/p2c.h"

/* the page line a display line shows */
static int page_line(const struct lrv_surface *page, int y, int scroll)
{
    int sy = (y + scroll) % page->h;
    return sy < 0 ? sy + page->h : sy;
}

/* ---- 8-bit ------------------------------------------------------------------------------- */

void lrv_p2c_init(struct lrv_p2c *t)
{
    int b, i;
    for (b = 0; b < 256; b++) {
        uint8_t px[8];
        for (i = 0; i < 8; i++)
            px[i] = (uint8_t)((b >> (7 - i)) & 1);
        memcpy(&t->lut[b][0], px, 4);
        memcpy(&t->lut[b][1], px + 4, 4);
    }
}

/* A plane's contribution is its table entry shifted left by the plane number; no byte lane ever
 * exceeds 15, so shifting and ORing never crosses bytes, whatever the byte order. */
void lrv_p2c8(const struct lrv_p2c *t, const struct lrv_surface *page, int x0, int y0, int x1,
              int y1, int scroll, uint8_t *dst, size_t pitch, uint8_t base)
{
    uint32_t base4;
    uint8_t b4[4];
    int y, x;
    memset(b4, base, 4);
    memcpy(&base4, b4, 4);
    for (y = y0; y < y1; y++) {
        size_t off = (size_t)page_line(page, y, scroll) * (size_t)page->stride;
        uint8_t *d = dst + (size_t)(y - y0) * pitch;
        const uint8_t *p0 = page->plane[0] + off, *p1 = page->plane[1] + off;
        const uint8_t *p2 = page->plane[2] + off, *p3 = page->plane[3] + off;
        for (x = x0; x < x1; x++) {
            uint32_t w[2];
            w[0] = base4 | t->lut[p0[x]][0] | t->lut[p1[x]][0] << 1 | t->lut[p2[x]][0] << 2 |
                   t->lut[p3[x]][0] << 3;
            w[1] = base4 | t->lut[p0[x]][1] | t->lut[p1[x]][1] << 1 | t->lut[p2[x]][1] << 2 |
                   t->lut[p3[x]][1] << 3;
            memcpy(d, w, 8);
            d += 8;
        }
    }
}

/* ---- 32-bit ------------------------------------------------------------------------------ */

void lrv_p2rgba_init(struct lrv_p2rgba *t)
{
    int k, b, i;
    for (k = 0; k < 4; k++)
        for (b = 0; b < 256; b++) {
            uint64_t v = 0;
            for (i = 0; i < 8; i++)
                if (b & (0x80 >> i))
                    v |= (uint64_t)(1u << k) << (8 * i);
            t->bits[k][b] = v;
        }
}

void lrv_rgba_palette(uint32_t lut[16], const uint8_t *rgb, int colours, int rgb_bits,
                      int display_on)
{
    uint32_t max = (1u << rgb_bits) - 1u;
    int i;
    for (i = 0; i < 16; i++) {
        uint32_t r, g, b;
        if (!display_on || i >= colours || !max) {
            lut[i] = 0xFF000000u;
            continue;
        }
        r = rgb[i * 3] * 255u / max;
        g = rgb[i * 3 + 1] * 255u / max;
        b = rgb[i * 3 + 2] * 255u / max;
        lut[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
}

void lrv_p2rgba(const struct lrv_p2rgba *t, const struct lrv_surface *page, int x0, int y0,
                int x1, int y1, int scroll, const uint32_t lut[16], uint32_t *dst,
                size_t pitch)
{
    int y, x;
    for (y = y0; y < y1; y++) {
        size_t off = (size_t)page_line(page, y, scroll) * (size_t)page->stride;
        const uint8_t *p0 = page->plane[0] + off, *p1 = page->plane[1] + off;
        const uint8_t *p2 = page->plane[2] + off, *p3 = page->plane[3] + off;
        uint32_t *d = dst + (size_t)(y - y0) * pitch;
        for (x = x0; x < x1; x++, d += 8) {
            uint64_t v = t->bits[0][p0[x]] | t->bits[1][p1[x]] | t->bits[2][p2[x]] |
                         t->bits[3][p3[x]];
            d[0] = lut[v & 15];         d[1] = lut[(v >> 8) & 15];
            d[2] = lut[(v >> 16) & 15]; d[3] = lut[(v >> 24) & 15];
            d[4] = lut[(v >> 32) & 15]; d[5] = lut[(v >> 40) & 15];
            d[6] = lut[(v >> 48) & 15]; d[7] = lut[(v >> 56) & 15];
        }
    }
}
