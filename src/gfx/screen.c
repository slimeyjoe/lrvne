/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The screen (lrvne/gfx.h): pages, dirty rectangles, the DAC and presentation with overlays. */
#include <string.h>
#include "lrvne/gfx.h"

static void surface_setup(struct lrv_surface *s, const struct lrv_video_mode *mode, int stride,
                          uint8_t *mem, int planes)
{
    size_t bytes = (size_t)stride * (size_t)mode->h;
    int p;
    memset(s, 0, sizeof *s);
    s->format = mode->format;
    s->w = mode->w;
    s->h = mode->h;
    s->stride = stride;
    for (p = 0; p < planes; p++)
        s->plane[p] = mem + (size_t)p * bytes;
}

int lrv_screen_init(struct lrv_screen *scr, const struct lrv_hal *hal,
                    const struct lrv_video_mode *mode, int npages, int nbuffers,
                    unsigned mem_flags)
{
    int planes = lrv_format_planes(mode->format);
    int stride, i;
    size_t set;

    memset(scr, 0, sizeof *scr);
    if (planes < 1 || mode->w < 1 || mode->h < 1 || mode->w % lrv_format_xunit(mode->format) ||
        npages < 1 || npages > LRV_SCREEN_PAGES_MAX ||
        nbuffers < 0 || nbuffers > LRV_SCREEN_BUFFERS_MAX ||
        mode->colours < 0 || mode->colours > LRV_PALETTE_MAX)
        return LRV_ERR_RANGE;
    stride = mode->w / lrv_format_xunit(mode->format);
    set = (size_t)planes * (size_t)stride * (size_t)mode->h;    /* one page or buffer */

    scr->hal = hal;
    scr->mode = *mode;
    scr->mem_size = set * (size_t)(npages + nbuffers);
    scr->mem = hal->mem_alloc(scr->mem_size, mem_flags);
    if (!scr->mem) {
        scr->mem_size = 0;
        return LRV_ERR_NOMEM;
    }
    scr->npages = npages;
    scr->nbuffers = nbuffers;
    for (i = 0; i < npages; i++)
        surface_setup(&scr->page[i], mode, stride, scr->mem + (size_t)i * set, planes);
    for (i = 0; i < nbuffers; i++)
        surface_setup(&scr->buffer[i], mode, stride, scr->mem + (size_t)(npages + i) * set,
                      planes);
    lrv_screen_reset(scr);
    return LRV_OK;
}

void lrv_screen_free(struct lrv_screen *scr)
{
    if (scr->mem)
        scr->hal->mem_free(scr->mem, scr->mem_size);
    memset(scr, 0, sizeof *scr);
}

void lrv_screen_reset(struct lrv_screen *scr)
{
    int i;
    if (scr->mem)
        memset(scr->mem, 0, scr->mem_size);
    for (i = 0; i < LRV_SCREEN_PAGES_MAX; i++)
        lrv_rect_clear(&scr->dirty[i]);
    scr->access_page = 0;
    scr->display_page = 0;
    scr->page_switched = 1;
    scr->display_on = 1;
    scr->scroll_line = 0;
    memset(scr->rgb, 0, sizeof scr->rgb);
}

/* ---- pages ------------------------------------------------------------------------------- */

int lrv_screen_set_access(struct lrv_screen *scr, int page)
{
    if (page < 0 || page >= scr->npages)
        return LRV_ERR_RANGE;
    scr->access_page = page;
    return LRV_OK;
}

int lrv_screen_set_display(struct lrv_screen *scr, int page)
{
    if (page < 0 || page >= scr->npages)
        return LRV_ERR_RANGE;
    if (page != scr->display_page) {
        scr->display_page = page;
        scr->page_switched = 1;
    }
    return LRV_OK;
}

struct lrv_surface *lrv_screen_access(struct lrv_screen *scr)
{
    return &scr->page[scr->access_page];
}

void lrv_screen_set_display_on(struct lrv_screen *scr, int on)
{
    on = on ? 1 : 0;
    if (on != scr->display_on)
        scr->page_switched = 1;
    scr->display_on = on;
}

void lrv_screen_set_scroll(struct lrv_screen *scr, int line)
{
    scr->scroll_line = line;
}

/* ---- dirty rectangles -------------------------------------------------------------------- */

void lrv_screen_mark(struct lrv_screen *scr, int page, int x0, int y0, int x1, int y1)
{
    struct lrv_rect r;
    if (page < 0 || page >= scr->npages)
        return;
    r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
    lrv_rect_clip(&r, &scr->page[page]);
    lrv_rect_union(&scr->dirty[page], &r);
}

void lrv_screen_mark_off(struct lrv_screen *scr, int page, int off, int wb, int h)
{
    int stride, x, y;
    if (page < 0 || page >= scr->npages || off < 0)
        return;
    stride = scr->page[page].stride;
    y = off / stride;
    x = off % stride;
    if (x + wb > stride)
        lrv_screen_mark(scr, page, 0, y, stride, y + h + 1);
    else
        lrv_screen_mark(scr, page, x, y, x + wb, y + h);
}

void lrv_screen_mark_surface(struct lrv_screen *scr, const struct lrv_surface *s,
                             const struct lrv_rect *r)
{
    int pg;
    if (lrv_rect_empty(r))
        return;
    for (pg = 0; pg < scr->npages; pg++)
        if (s->plane[0] == scr->page[pg].plane[0])
            lrv_screen_mark(scr, pg, r->x0, r->y0, r->x1, r->y1);
}

/* ---- surfaces ---------------------------------------------------------------------------- */

void lrv_surface_clear(struct lrv_surface *s)
{
    int p, planes = lrv_format_planes(s->format);
    for (p = 0; p < planes; p++)
        memset(s->plane[p], 0, lrv_surface_plane_bytes(s));
}

void lrv_surface_copy_rows(struct lrv_surface *dst, const struct lrv_surface *src, int off,
                           int wb, int h)
{
    int p, y, planes = lrv_format_planes(dst->format);
    long bytes = (long)lrv_surface_plane_bytes(dst);
    if (off < 0 || wb <= 0 || h <= 0 || src->format != dst->format ||
        src->stride != dst->stride || src->h != dst->h)
        return;
    for (p = 0; p < planes; p++) {
        for (y = 0; y < h; y++) {
            long o = (long)off + (long)y * dst->stride;
            if (o + wb > bytes)
                break;
            memcpy(dst->plane[p] + o, src->plane[p] + o, (size_t)wb);
        }
    }
}

void lrv_screen_clear_access(struct lrv_screen *scr)
{
    struct lrv_surface *s = lrv_screen_access(scr);
    lrv_surface_clear(s);
    lrv_screen_mark(scr, scr->access_page, 0, 0, s->stride, s->h);
}

/* ---- palette ----------------------------------------------------------------------------- */

void lrv_screen_set_colour(struct lrv_screen *scr, int i, int r, int g, int b)
{
    int mask = (1 << scr->mode.rgb_bits) - 1;
    if (i < 0 || i >= scr->mode.colours)
        return;
    scr->rgb[i * 3] = (uint8_t)(r & mask);
    scr->rgb[i * 3 + 1] = (uint8_t)(g & mask);
    scr->rgb[i * 3 + 2] = (uint8_t)(b & mask);
}

void lrv_screen_set_palette(struct lrv_screen *scr, const uint8_t *rgb)
{
    memcpy(scr->rgb, rgb, (size_t)scr->mode.colours * 3);
}

/* ---- presentation ------------------------------------------------------------------------ */

int lrv_screen_add_overlay(struct lrv_screen *scr, lrv_overlay_apply_fn *apply,
                           lrv_overlay_restore_fn *restore, void *ctx)
{
    struct lrv_overlay *o;
    if (scr->noverlays >= LRV_SCREEN_OVERLAYS_MAX)
        return LRV_ERR_RANGE;
    o = &scr->overlay[scr->noverlays++];
    o->apply = apply;
    o->restore = restore;
    o->ctx = ctx;
    return LRV_OK;
}

void lrv_screen_present(struct lrv_screen *scr)
{
    struct lrv_surface *page = &scr->page[scr->display_page];
    struct lrv_rect *d = &scr->dirty[scr->display_page];
    struct lrv_frame f;
    int i;

    for (i = 0; i < scr->noverlays; i++)
        if (scr->overlay[i].apply)
            scr->overlay[i].apply(scr->overlay[i].ctx, page, d);
    f.page = page;
    f.dirty = d;
    f.page_switched = scr->page_switched;
    f.scroll_line = scr->scroll_line;
    f.rgb = scr->rgb;
    f.display_on = scr->display_on;
    scr->hal->video_present(&f);
    lrv_rect_clear(d);
    /* after the clear: what is restored is dirty for the next present, so something an overlay
     * has stopped drawing by then is erased on backends that upload only dirty rectangles */
    for (i = scr->noverlays - 1; i >= 0; i--)
        if (scr->overlay[i].restore)
            scr->overlay[i].restore(scr->overlay[i].ctx, page, d);
    scr->page_switched = 0;
}

void lrv_screen_present_hook(void *scr)
{
    lrv_screen_present(scr);
}
