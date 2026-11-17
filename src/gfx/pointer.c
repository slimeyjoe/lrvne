/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The software pointer (lrvne/gfx.h): an overlay that draws a shape at a pixel position. */
#include <string.h>
#include "lrvne/gfx.h"

#define CELLS (LRV_POINTER_W / 8 + 1)   /* bytes a row touches at any pixel position */

void lrv_pointer_init(struct lrv_pointer *ptr, const struct lrv_pointer_shape *shape)
{
    memset(ptr, 0, sizeof *ptr);
    ptr->shape = shape;
}

/* the rows and byte columns of the drawn pointer that lie on the page */
static void ptr_mark(const struct lrv_pointer *ptr, const struct lrv_surface *page,
                     struct lrv_rect *dirty)
{
    struct lrv_rect r;
    r.x0 = ptr->col;
    r.x1 = ptr->col + CELLS;
    r.y0 = ptr->top;
    r.y1 = ptr->top + ptr->shape->h;
    lrv_rect_clip(&r, page);
    lrv_rect_union(dirty, &r);
}

static int on_page(const struct lrv_surface *page, int col, int line)
{
    return col >= 0 && col < page->w / 8 && line >= 0 && line < page->h;
}

void lrv_pointer_apply(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty)
{
    struct lrv_pointer *ptr = ctx;
    const struct lrv_pointer_shape *sh = ptr->shape;
    int px, shift, r, b, p;

    ptr->drawn = 0;
    if (ptr->follow_x)
        ptr->x = *ptr->follow_x;
    if (ptr->follow_y)
        ptr->y = *ptr->follow_y;
    if (!ptr->shown || !sh || sh->h < 1 || sh->h > LRV_POINTER_H_MAX ||
        page->format != LRV_FMT_PLANAR4)
        return;
    px = ptr->x - sh->hot_x;
    shift = px & 7;
    ptr->col = (px - shift) / 8;        /* floor, also left of the page */
    ptr->top = ptr->y - sh->hot_y;
    for (r = 0; r < sh->h; r++) {
        uint32_t ink = (uint32_t)(sh->ink[r] & sh->mask[r]) << 8 >> shift;
        uint32_t mask = (uint32_t)sh->mask[r] << 8 >> shift;
        for (b = 0; b < CELLS; b++) {
            uint8_t m = (uint8_t)(mask >> (16 - 8 * b)), i = (uint8_t)(ink >> (16 - 8 * b));
            if (!on_page(page, ptr->col + b, ptr->top + r))
                continue;
            for (p = 0; p < 4; p++) {
                uint8_t *d = page->plane[p] + (ptr->top + r) * page->stride + ptr->col + b;
                uint8_t v = (uint8_t)(*d & ~m);
                ptr->under[p][r][b] = *d;
                if ((sh->fg >> p) & 1)
                    v |= i;
                if ((sh->bg >> p) & 1)
                    v |= (uint8_t)(m & ~i);
                *d = v;
            }
        }
    }
    ptr->drawn = 1;
    ptr_mark(ptr, page, dirty);
}

void lrv_pointer_restore(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty)
{
    struct lrv_pointer *ptr = ctx;
    int r, b, p;

    if (!ptr->drawn)
        return;
    for (r = 0; r < ptr->shape->h; r++)
        for (b = 0; b < CELLS; b++) {
            if (!on_page(page, ptr->col + b, ptr->top + r))
                continue;
            for (p = 0; p < 4; p++)
                page->plane[p][(ptr->top + r) * page->stride + ptr->col + b] =
                    ptr->under[p][r][b];
        }
    ptr->drawn = 0;
    ptr_mark(ptr, page, dirty);
}
