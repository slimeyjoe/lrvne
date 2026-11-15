/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Planar to chunky: a rectangle of an LRV_FMT_PLANAR4 surface as one value per pixel, for
 * backends whose display takes chunky pixels.
 *
 * Two conversions, each with a lookup table the caller owns and fills once:
 *   lrv_p2c8     8-bit pixels, base + colour index (palette displays, pen arrays)
 *   lrv_p2rgba   32-bit pixels through a 16-entry colour table (true colour textures)
 *
 * Both give the same result on either byte order. Rectangles are in columns (bytes of a plane,
 * 8 pixels) and display lines; display line y shows page line (y + scroll) mod height, as a
 * hardware scroll does. */
#ifndef LRVNE_P2C_H
#define LRVNE_P2C_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/surface.h"

/* ---- 8-bit ------------------------------------------------------------------------------- */

/* lut[b] = the eight pixels of plane byte b, each 1 where b has a 1 (most significant bit =
 * leftmost pixel), as two 32-bit words in memory order. */
struct lrv_p2c {
    uint32_t lut[256][2];
};

void lrv_p2c_init(struct lrv_p2c *t);

/* Columns [x0, x1) of display lines [y0, y1) as pixels base + colour index: the pixel at
 * (x, y) goes to dst[(y - y0) * pitch + (x - x0 * 8)]. base must not use bits 0-3 (0, 16, ...). */
void lrv_p2c8(const struct lrv_p2c *t, const struct lrv_surface *page, int x0, int y0, int x1,
              int y1, int scroll, uint8_t *dst, size_t pitch, uint8_t base);

/* ---- 32-bit ------------------------------------------------------------------------------ */

/* bits[k][b] = plane k's byte b spread over eight 8-bit lanes of a 64-bit word: lane i (bits
 * 8i .. 8i+7) is pixel i from the left and holds bit k where b has a 1. ORing the four planes'
 * entries gives the eight colour indices of a column at once. */
struct lrv_p2rgba {
    uint64_t bits[4][256];
};

void lrv_p2rgba_init(struct lrv_p2rgba *t);

/* The colour table of a palette for lrv_p2rgba: entry i = 0xFF000000 | B << 16 | G << 8 | R
 * with 8-bit components (a gun of rgb_bits bits scaled so that its maximum is 255), which in
 * memory is R, G, B, A on a little-endian host. All entries are opaque black when the display
 * is off. rgb is colours x {R,G,B}; colours <= 16. */
void lrv_rgba_palette(uint32_t lut[16], const uint8_t *rgb, int colours, int rgb_bits,
                      int display_on);

/* Columns [x0, x1) of display lines [y0, y1): the pixel at (x, y) goes to
 * dst[(y - y0) * pitch + (x - x0 * 8)], pitch in pixels. */
void lrv_p2rgba(const struct lrv_p2rgba *t, const struct lrv_surface *page, int x0, int y0,
                int x1, int y1, int scroll, const uint32_t lut[16], uint32_t *dst,
                size_t pitch);

#endif
