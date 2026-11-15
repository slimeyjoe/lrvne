/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Integer scaling and the aspect stretch of a page, for backends that draw it larger than its
 * own size themselves (no scaler on the host to lean on).
 *
 * Horizontally each pixel becomes sx pixels (sx = 1..LRV_SCALE_MAX). Vertically a row map sends
 * display row r to the destination rows [start[r], start[r + 1]): nearest neighbour, so 400
 * rows to 480 repeats every fifth row, and any destination height from the page's up works.
 *
 * The expansion converts a rectangle of the page (rotated by `scroll` as in lrvne/p2c.h) and
 * writes each pixel through a 16-entry pen table, which serves palette and true colour
 * displays that draw pens alike. */
#ifndef LRVNE_SCALE_H
#define LRVNE_SCALE_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/hal.h"
#include "lrvne/p2c.h"
#include "lrvne/surface.h"

#define LRV_SCALE_MAX   3
#define LRV_ROWMAP_ROWS 480     /* the tallest page a row map serves */
#define LRV_EXPAND_W    1024    /* the widest rectangle lrv_expand8 converts, in page pixels */

struct lrv_rowmap {
    int src_h;                              /* rows of the page */
    int dst_h;                              /* destination height, >= src_h */
    uint16_t start[LRV_ROWMAP_ROWS + 1];    /* start[src_h] = dst_h */
};

/* The map from src_h rows (1..LRV_ROWMAP_ROWS, else clamped) to dst_h (values below src_h are
 * taken as src_h; at most 65535). */
void lrv_rowmap_init(struct lrv_rowmap *m, int src_h, int dst_h);

/* The destination height that shows a page of `mode` in the mode's aspect (640x400 shown 4:3)
 * at sx pixels across per page pixel, on a display whose pixels are px_w:px_h (width:height,
 * 1:1 when square; 0 is taken as square). Never below mode->h * sx: rows are repeated, never
 * dropped, and within 2 % of mode->h * sx it is mode->h * sx, because a few repeated rows
 * would only make the picture uneven. */
int lrv_aspect_height(const struct lrv_video_mode *mode, int sx, unsigned px_w, unsigned px_h);

/* Columns [x0, x1) of display rows [y0, y1), at sx pixels per page pixel and through the row
 * map: destination rows m->start[y0] .. m->start[y1] - 1, (x1 - x0) * 8 * sx pixels each.
 * Destination row k (counted from m->start[y0]) starts at dst + k * pitch (bytes). Nothing is
 * written for a rectangle wider than LRV_EXPAND_W pixels. */
void lrv_expand8(const struct lrv_p2c *t, const struct lrv_surface *page, int x0, int y0, int x1,
                 int y1, int scroll, int sx, const struct lrv_rowmap *m, const uint8_t pen[16],
                 uint8_t *dst, size_t pitch);

#endif
