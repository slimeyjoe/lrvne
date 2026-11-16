/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The screen of the emulated machine: its pages, what is dirty on them, the palette as the DAC
 * holds it, and the presentation of the displayed page through the HAL.
 *
 * A screen has `npages` video pages, one of which is displayed and one of which is accessed
 * (drawn into), and optionally off-screen buffers of the same shape. All of them are plain
 * memory in one block from the HAL. Every write into a page marks a rectangle dirty on it, so a
 * backend uploads only what changed.
 *
 * Overlays draw things that exist only in the presented picture and never in the page as the
 * engine sees it: a software mouse pointer, a text layer above the graphics. An overlay is a
 * pair of functions; `apply` draws into the displayed page keeping what was under it, `restore`
 * puts that back after the present.
 *
 * Also here: arithmetic of palette fades (no timing, no state) and the hash of a presented
 * frame that tests and backends compare pictures by.
 *
 * Picture formats, effects and what an engine's own video services mean are not part of the
 * library. */
#ifndef LRVNE_GFX_H
#define LRVNE_GFX_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/core.h"
#include "lrvne/hal.h"
#include "lrvne/surface.h"

#define LRV_SCREEN_PAGES_MAX    4
#define LRV_SCREEN_BUFFERS_MAX  4
#define LRV_SCREEN_OVERLAYS_MAX 4
#define LRV_PALETTE_MAX         256     /* entries of the largest palette */

/* apply: draw into `page` (the displayed one), keeping what is overwritten, and add what was
 * touched to `dirty`. restore: put the kept bytes back and add them to `dirty`, which by then
 * belongs to the next present: what an overlay no longer draws is erased there. */
typedef void lrv_overlay_apply_fn(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty);
typedef void lrv_overlay_restore_fn(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty);

struct lrv_overlay {
    lrv_overlay_apply_fn *apply;
    lrv_overlay_restore_fn *restore;
    void *ctx;
};

struct lrv_screen {
    const struct lrv_hal *hal;
    struct lrv_video_mode mode;
    int npages, nbuffers;
    struct lrv_surface page[LRV_SCREEN_PAGES_MAX];
    struct lrv_rect dirty[LRV_SCREEN_PAGES_MAX];    /* per page, since it was last presented */
    struct lrv_surface buffer[LRV_SCREEN_BUFFERS_MAX];  /* off-screen, never presented */
    uint8_t *mem;                       /* the one block all planes are in */
    size_t mem_size;
    int access_page, display_page;
    int page_switched;                  /* the next present must upload everything */
    int display_on;
    int scroll_line;
    uint8_t rgb[LRV_PALETTE_MAX * 3];   /* the DAC: mode.colours x {R,G,B}, mode.rgb_bits each */
    struct lrv_overlay overlay[LRV_SCREEN_OVERLAYS_MAX];
    int noverlays;
};

/* ---- life cycle ------------------------------------------------------------------------- */

/* Allocates npages pages and nbuffers buffers of the mode's format and size in one block of
 * lrv_alloc_flags(hal, size, mem_flags) (LRV_MEM_*: a backend that shows the pages where they
 * are needs them in memory its display hardware reaches), page 0 first, plane after plane, then
 * the next page, then the buffers. The state is that of lrv_screen_reset. Returns LRV_OK,
 * LRV_ERR_RANGE for counts or a format that cannot be had, LRV_ERR_NOMEM. */
int  lrv_screen_init(struct lrv_screen *scr, const struct lrv_hal *hal,
                     const struct lrv_video_mode *mode, int npages, int nbuffers,
                     unsigned mem_flags);
void lrv_screen_free(struct lrv_screen *scr);

/* As after init: pages and buffers cleared, page 0 displayed and accessed, nothing dirty but
 * everything to be uploaded, the display on, no scroll, all colours black. Overlays stay. */
void lrv_screen_reset(struct lrv_screen *scr);

/* ---- pages ------------------------------------------------------------------------------ */

/* Select the page drawn into / shown. LRV_ERR_RANGE (and no change) for a page that is not
 * there: what an out-of-range number means on the original is the engine's to decide. */
int  lrv_screen_set_access(struct lrv_screen *scr, int page);
int  lrv_screen_set_display(struct lrv_screen *scr, int page);
struct lrv_surface *lrv_screen_access(struct lrv_screen *scr);

/* The display on or off (off shows black); a change makes the next present upload everything. */
void lrv_screen_set_display_on(struct lrv_screen *scr, int on);
/* The page line shown at the top of the display. */
void lrv_screen_set_scroll(struct lrv_screen *scr, int line);

/* ---- dirty rectangles ------------------------------------------------------------------- */

/* Columns [x0, x1) of lines [y0, y1) of `page` changed (clipped to the page). */
void lrv_screen_mark(struct lrv_screen *scr, int page, int x0, int y0, int x1, int y1);
/* The same for a run addressed as the original machines do: h lines of wb columns starting at
 * plane offset off = y * stride + x. A run that crosses the right edge spills into the next
 * line: the whole band of lines is marked then. */
void lrv_screen_mark_off(struct lrv_screen *scr, int page, int off, int wb, int h);
/* A rectangle written into a surface that may be one of the pages (a decoder's target):
 * marked on that page, ignored for any other surface. */
void lrv_screen_mark_surface(struct lrv_screen *scr, const struct lrv_surface *s,
                             const struct lrv_rect *r);

/* ---- surfaces --------------------------------------------------------------------------- */

/* All planes to zero (colour 0). */
void lrv_surface_clear(struct lrv_surface *s);
/* Copies h lines of wb columns at plane offset `off` (y * stride + x) from src to dst, all
 * planes, whole columns. Lines that would end past the plane are not copied. */
void lrv_surface_copy_rows(struct lrv_surface *dst, const struct lrv_surface *src, int off,
                           int wb, int h);
/* The access page to colour 0, marked dirty. */
void lrv_screen_clear_access(struct lrv_screen *scr);

/* ---- palette ---------------------------------------------------------------------------- */

/* DAC entry i (ignored when out of range); components are masked to mode.rgb_bits. */
void lrv_screen_set_colour(struct lrv_screen *scr, int i, int r, int g, int b);
/* All mode.colours entries from rgb (colours x {R,G,B}). */
void lrv_screen_set_palette(struct lrv_screen *scr, const uint8_t *rgb);

/* ---- presentation ----------------------------------------------------------------------- */

/* Adds an overlay. They are applied in the order added (the last one is on top) and restored
 * in the opposite order. LRV_ERR_RANGE when there is no room. */
int  lrv_screen_add_overlay(struct lrv_screen *scr, lrv_overlay_apply_fn *apply,
                            lrv_overlay_restore_fn *restore, void *ctx);

/* Applies the overlays, gives the displayed page to hal->video_present, clears its dirty
 * rectangle and restores the overlays (whose bytes are then dirty for the next present). */
void lrv_screen_present(struct lrv_screen *scr);
/* The same as an lrv_hook_fn: lrv_sys_set_present(sys, lrv_screen_present_hook, scr). */
void lrv_screen_present_hook(void *scr);

/* ---- palette fade arithmetic ------------------------------------------------------------ */

/* The ways the engines step a palette, as functions over n components (bytes). They keep
 * no time and no state: the engine sequences the steps as its original did. */

/* Moves every component of cur one unit towards target. Returns how many changed: 0 means cur
 * was already the target. */
int  lrv_pal_step_toward(uint8_t *cur, const uint8_t *target, size_t n);

/* Linear interpolation: out = from + (to - from) * step / steps, the signed division
 * truncating toward zero. step = steps gives `to`; steps must be positive. out may be from. */
void lrv_pal_lerp(uint8_t *out, const uint8_t *from, const uint8_t *to, size_t n, int step,
                  int steps);

/* Shift: out = in + d, clamped to 0..max (a fade that adds the same signed amount to every
 * component). out may be in. */
void lrv_pal_shift(uint8_t *out, const uint8_t *in, size_t n, int d, int max);

/* A step that only decrements: every component of cur above its target goes down by one, the
 * others stay. Returns how many changed: 0 means none is above its target. */
int  lrv_pal_step_down(uint8_t *cur, const uint8_t *target, size_t n);

/* Brightness: a component v at level 0..max is round(v * level / max), halves rounded up. */
int  lrv_pal_bright(int v, int level, int max);
/* ... applied to n components (level is clamped to 0..max). out may be in. */
void lrv_pal_brightness(uint8_t *out, const uint8_t *in, size_t n, int level, int max);

/* ---- frame hash ------------------------------------------------------------------------- */

/* FNV-1a (32 bit) over what a frame shows: the displayed image line by line from the scroll
 * line, in every line plane after plane; then the frame's palette entries; then the display
 * flag as one byte. Two frames with the same hash show the same picture, on any backend. */
uint32_t lrv_frame_hash32(const struct lrv_frame *frame);

#endif
