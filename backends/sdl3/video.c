/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDL3 backend, video: the window, the page as a texture, scaling and the readback.
 *
 * The texture has the size of the video mode. A present converts and uploads only what
 * changed (the engine's dirty rectangle, in byte columns x lines), everything when the page,
 * the scroll line or the palette changed, and nothing at all when the screen is unchanged:
 * then the renderer is not even presented unless the window needs a redraw (expose, resize,
 * scale mode) or the frames are paced by vsync.
 *
 * Scaling, from the mode's size and the shape of its picture (aspect_w:aspect_h), which
 * together say how much taller than wide a pixel is (640x400 shown 4:3: 1.2):
 *   integer   the same whole factor on both axes
 *   aspect    whole factors on each axis, the pair nearest the pixel's shape
 *   fit       the largest picture of the right shape, filtered
 *   stretch   the whole window, filtered */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lrvne/core.h"
#include "lrvne/p2c.h"
#include "sdl3.h"

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static uint32_t *g_rgba;                /* the mode's picture as uploaded, w x h */
static int g_scale_mode = LRV_SCALE_INTEGER;
static int g_fullscreen;
static struct lrv_p2rgba g_p2c;         /* the planar -> chunky table */
static uint32_t g_lut[16];              /* colour index -> ABGR */
static uint8_t g_last_rgb[16 * 3];
static int g_last_on = -1, g_last_scroll = -1;
static int g_tex_valid;                 /* the texture holds the last presented page */
static int g_redraw = 1;                /* present again without new content */

SDL_Window *lrv_sdl3_window(void)
{
    return g_win;
}

int lrv_sdl3_video_open(const struct lrv_video_cfg *cfg)
{
    const struct lrv_video_mode *m = &cfg->mode;
    SDL_PropertiesID props;

    if (m->format != LRV_FMT_PLANAR4 || m->w <= 0 || m->h <= 0 || m->w % 8 != 0 ||
        m->colours < 1 || m->colours > 16 || m->rgb_bits < 1 || m->rgb_bits > 8) {
        fprintf(stderr, "%s: video mode not supported by this backend\n", lrv_sdl3.app->name);
        return LRV_ERR_RANGE;
    }
    g_fullscreen = cfg->fullscreen;
    g_scale_mode = cfg->scale_mode;
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          cfg->title && *cfg->title ? cfg->title : lrv_sdl3.app->name);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          lrv_sdl3.win_w ? lrv_sdl3.win_w : 2 * m->w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                          lrv_sdl3.win_h ? lrv_sdl3.win_h : 2 * m->h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                          (g_fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
#ifdef __APPLE__
    /* On macOS SDL gives every window an OpenGL or Metal backend by default, and the
     * offscreen driver then needs EGL, which macOS does not have. Headless, the window gets no
     * graphics backend of its own; the software renderer (see main.c) draws into it. */
    if (lrv_sdl3.control)
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN,
                               true);
#endif
    g_win = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (g_win)
        g_ren = SDL_CreateRenderer(g_win, NULL);
    if (g_ren)
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                  m->w, m->h);
    if (!g_tex) {
        fprintf(stderr, "%s: SDL: %s\n", lrv_sdl3.app->name, SDL_GetError());
        lrv_sdl3_video_close();
        return LRV_ERR;
    }
    g_rgba = malloc((size_t)m->w * (size_t)m->h * sizeof *g_rgba);
    if (!g_rgba) {
        lrv_sdl3_video_close();
        return LRV_ERR_NOMEM;
    }
    SDL_SetRenderVSync(g_ren, lrv_sdl3.vsync ? 1 : 0);
    lrv_p2rgba_init(&g_p2c);
    if (!lrv_sdl3.control && !lrv_sdl3.app->host_pointer)
        SDL_HideCursor();               /* the engine draws its machine's pointer itself */
    lrv_sdl3.mode = *m;
    lrv_ctl_video(m);
    g_tex_valid = 0;
    g_redraw = 1;
    return LRV_OK;
}

void lrv_sdl3_video_close(void)
{
    free(g_rgba);
    if (g_tex)
        SDL_DestroyTexture(g_tex);
    if (g_ren)
        SDL_DestroyRenderer(g_ren);
    if (g_win)
        SDL_DestroyWindow(g_win);
    g_rgba = 0;
    g_tex = 0;
    g_ren = 0;
    g_win = 0;
    lrv_sdl3.mode.w = 0;
}

/* how many times taller than wide a pixel of the mode is on its monitor */
static double pixel_aspect(void)
{
    const struct lrv_video_mode *m = &lrv_sdl3.mode;
    if (m->aspect_w <= 0 || m->aspect_h <= 0)
        return 1.0;
    return ((double)m->w * m->aspect_h) / ((double)m->h * m->aspect_w);
}

/* the destination rectangle for the current scale mode, in output pixels */
static void dest_rect(SDL_FRect *d, int *nearest)
{
    const struct lrv_video_mode *m = &lrv_sdl3.mode;
    double par = pixel_aspect();
    int W, H;

    SDL_GetWindowSizeInPixels(g_win, &W, &H);
    *nearest = 1;
    switch (g_scale_mode) {
    case LRV_SCALE_INTEGER: {
        int f = W / m->w < H / m->h ? W / m->w : H / m->h;
        if (f < 1)
            f = 1;
        d->w = (float)(m->w * f);
        d->h = (float)(m->h * f);
        break;
    }
    case LRV_SCALE_INTEGER_ASPECT: {
        /* the largest pair of whole factors whose ratio is the pixel's shape, rounded */
        int maxx = W / m->w, maxy = H / m->h, f, bx = 1, by = 1;
        for (f = 1; f <= (par >= 1.0 ? maxx : maxy); f++) {
            int fx = par >= 1.0 ? f : (int)(f / par + 0.5);
            int fy = par >= 1.0 ? (int)(f * par + 0.5) : f;
            if (fx >= 1 && fy >= 1 && fx <= maxx && fy <= maxy) {
                bx = fx;
                by = fy;
            }
        }
        d->w = (float)(m->w * bx);
        d->h = (float)(m->h * by);
        break;
    }
    case LRV_SCALE_FIT: {
        /* the picture with square host pixels: 640x400 at 4:3 is 640x480 */
        float pw = par >= 1.0 ? (float)m->w : (float)(m->w / par);
        float ph = par >= 1.0 ? (float)(m->h * par) : (float)m->h;
        float s = (float)W / pw < (float)H / ph ? (float)W / pw : (float)H / ph;
        d->w = pw * s;
        d->h = ph * s;
        *nearest = 0;
        break;
    }
    default:
        d->w = (float)W;
        d->h = (float)H;
        *nearest = 0;
        break;
    }
    d->x = ((float)W - d->w) / 2.0f;
    d->y = ((float)H - d->h) / 2.0f;
}

/* clears the window and draws the texture into the destination rectangle (no present) */
static void draw(SDL_FRect *d)
{
    int nearest;
    dest_rect(d, &nearest);
    SDL_SetTextureScaleMode(g_tex, nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, d);
}

/* converts display lines [y0,y1) x byte columns [x0,x1) into g_rgba and uploads them */
static void convert(const struct lrv_surface *pg, int scroll, int x0, int y0, int x1, int y1)
{
    uint32_t *dst = g_rgba + (size_t)y0 * (size_t)pg->w + (size_t)x0 * 8;
    SDL_Rect r;

    lrv_p2rgba(&g_p2c, pg, x0, y0, x1, y1, scroll, g_lut, dst, (size_t)pg->w);
    r.x = x0 * 8;
    r.y = y0;
    r.w = (x1 - x0) * 8;
    r.h = y1 - y0;
    SDL_UpdateTexture(g_tex, &r, dst, pg->w * 4);
}

static void set_palette(const struct lrv_frame *f)
{
    const struct lrv_video_mode *m = &lrv_sdl3.mode;

    lrv_rgba_palette(g_lut, f->rgb, m->colours, m->rgb_bits, f->display_on);
    memcpy(g_last_rgb, f->rgb, (size_t)m->colours * 3);
}

void lrv_sdl3_video_present(const struct lrv_frame *f)
{
    const struct lrv_video_mode *m = &lrv_sdl3.mode;
    const struct lrv_surface *pg = f->page;
    int cols = m->w / 8, h = m->h, scroll, changed = 0;
    SDL_FRect d;

    if (!g_tex || pg->format != LRV_FMT_PLANAR4 || pg->w != m->w || pg->h != h)
        return;
    if (lrv_sdl3.control)
        lrv_ctl_present(f);
    scroll = ((f->scroll_line % h) + h) % h;
    if (!g_tex_valid || f->page_switched || !f->dirty || scroll != g_last_scroll ||
        (f->display_on != 0) != g_last_on ||
        memcmp(f->rgb, g_last_rgb, (size_t)m->colours * 3) != 0) {
        set_palette(f);
        g_last_on = f->display_on != 0;
        g_last_scroll = scroll;
        convert(pg, scroll, 0, 0, cols, h);
        g_tex_valid = changed = 1;
    } else if (!lrv_rect_empty(f->dirty)) {
        int x0 = f->dirty->x0 < 0 ? 0 : f->dirty->x0, y0 = f->dirty->y0 < 0 ? 0 : f->dirty->y0;
        int x1 = f->dirty->x1 > cols ? cols : f->dirty->x1;
        int y1 = f->dirty->y1 > h ? h : f->dirty->y1;
        if (x0 < x1 && y0 < y1) {
            /* page line r is displayed at line (r - scroll) mod h: it may wrap at the bottom */
            int d0 = (y0 - scroll + h) % h, n = y1 - y0;
            if (d0 + n <= h) {
                convert(pg, scroll, x0, d0, x1, d0 + n);
            } else {
                convert(pg, scroll, x0, d0, x1, h);
                convert(pg, scroll, x0, 0, x1, d0 + n - h);
            }
            changed = 1;
        }
    }
    if (changed || g_redraw || lrv_sdl3.vsync) {
        draw(&d);
        SDL_RenderPresent(g_ren);
        g_redraw = 0;
    }
}

void lrv_sdl3_video_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        g_redraw = 1;                   /* the unchanged page still has to be drawn again */
        break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
        g_tex_valid = 0;                /* the texture may be gone: upload everything */
        g_redraw = 1;
        break;
    default:
        break;
    }
}

void lrv_sdl3_toggle_fullscreen(void)
{
    if (!g_win)
        return;
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(g_win, g_fullscreen ? true : false);
}

void lrv_sdl3_cycle_scale(void)
{
    g_scale_mode = (g_scale_mode + 1) % (LRV_SCALE_STRETCH + 1);
    g_redraw = 1;
}

int lrv_sdl3_window_to_mode(float wx, float wy, int *x, int *y)
{
    const struct lrv_video_mode *m = &lrv_sdl3.mode;
    SDL_FRect d;
    float rx, ry;
    int nearest;

    if (!g_ren || !SDL_RenderCoordinatesFromWindow(g_ren, wx, wy, &rx, &ry))
        return 0;
    dest_rect(&d, &nearest);
    if (d.w <= 0 || d.h <= 0)
        return 0;
    *x = (int)SDL_floorf((rx - d.x) * (float)m->w / d.w);
    *y = (int)SDL_floorf((ry - d.y) * (float)m->h / d.h);
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
    if (*x > m->w - 1)
        *x = m->w - 1;
    if (*y > m->h - 1)
        *y = m->h - 1;
    return 1;
}

void lrv_sdl3_window_to_mode_rel(float wdx, float wdy, float *dx, float *dy)
{
    const struct lrv_video_mode *m = &lrv_sdl3.mode;
    SDL_FRect d;
    float x0, y0, x1, y1;
    int nearest;

    *dx = *dy = 0;
    if (!g_ren || !SDL_RenderCoordinatesFromWindow(g_ren, 0, 0, &x0, &y0) ||
        !SDL_RenderCoordinatesFromWindow(g_ren, wdx, wdy, &x1, &y1))
        return;
    dest_rect(&d, &nearest);
    if (d.w <= 0 || d.h <= 0)
        return;
    *dx = (x1 - x0) * (float)m->w / d.w;
    *dy = (y1 - y0) * (float)m->h / d.h;
}

int lrv_sdl3_mode_to_window(int x, int y, float *wx, float *wy)
{
    const struct lrv_video_mode *m = &lrv_sdl3.mode;
    SDL_FRect d;
    int nearest;

    if (!g_ren)
        return 0;
    dest_rect(&d, &nearest);
    return SDL_RenderCoordinatesToWindow(g_ren, d.x + ((float)x + 0.5f) * d.w / (float)m->w,
                                         d.y + ((float)y + 0.5f) * d.h / (float)m->h, wx, wy);
}

/* Control-mode readback (control.h): draws the last uploaded frame again exactly as a present
 * did and reads the renderer's pixels back. One sample per source pixel at the centre of its
 * destination block; every destination pixel inside the rectangle is compared with the sample
 * of the source pixel it maps to (nonuniform), everything outside must be black (border_lit). */
int lrv_sdl3_readback(struct lrv_ctl_readback *rb)
{
    int mw = lrv_sdl3.mode.w, mh = lrv_sdl3.mode.h;
    SDL_FRect d;
    SDL_Surface *s, *c;
    int x, y, x0, y0, w, h;

    if (!g_tex)
        return -1;
    draw(&d);
    s = SDL_RenderReadPixels(g_ren, NULL);
    if (!s)
        return -1;
    c = s->format == SDL_PIXELFORMAT_ABGR8888 ? s : SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
    if (!c) {
        SDL_DestroySurface(s);
        return -1;
    }
    x0 = (int)SDL_floorf(d.x + 0.5f);                   /* may be negative */
    y0 = (int)SDL_floorf(d.y + 0.5f);
    w = (int)SDL_floorf(d.w + 0.5f);
    h = (int)SDL_floorf(d.h + 0.5f);
    rb->win_w = c->w;
    rb->win_h = c->h;
    rb->dst_x = x0;
    rb->dst_y = y0;
    rb->dst_w = w;
    rb->dst_h = h;
    rb->scale_mode = g_scale_mode;
    rb->linear = g_scale_mode == LRV_SCALE_FIT || g_scale_mode == LRV_SCALE_STRETCH;
    rb->nonuniform = rb->border_lit = 0;
    for (y = 0; y < mh; y++) {
        int dy = y0 + (int)(((float)y + 0.5f) * (float)h / (float)mh);
        for (x = 0; x < mw; x++) {
            int dx = x0 + (int)(((float)x + 0.5f) * (float)w / (float)mw);
            uint8_t *o = rb->rgb + ((size_t)y * (size_t)mw + (size_t)x) * 3;
            o[0] = o[1] = o[2] = 0;
            if (dx >= 0 && dx < c->w && dy >= 0 && dy < c->h) {
                uint32_t px;
                memcpy(&px, (const uint8_t *)c->pixels + dy * c->pitch + dx * 4, sizeof px);
                o[0] = (uint8_t)px;
                o[1] = (uint8_t)(px >> 8);
                o[2] = (uint8_t)(px >> 16);
            }
        }
    }
    for (y = 0; y < c->h; y++) {
        const uint8_t *row = (const uint8_t *)c->pixels + y * c->pitch;
        for (x = 0; x < c->w; x++) {
            uint32_t px;
            memcpy(&px, row + x * 4, sizeof px);
            px &= 0xFFFFFFu;
            if (w > 0 && h > 0 && x >= x0 && x < x0 + w && y >= y0 && y < y0 + h) {
                int sx = (int)((long)(x - x0) * mw / w), sy = (int)((long)(y - y0) * mh / h);
                const uint8_t *o = rb->rgb + ((size_t)sy * (size_t)mw + (size_t)sx) * 3;
                if (px != ((uint32_t)o[0] | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16)))
                    rb->nonuniform++;
            } else if (px != 0) {
                rb->border_lit++;
            }
        }
    }
    if (c != s)
        SDL_DestroySurface(c);
    SDL_DestroySurface(s);
    return 0;
}
