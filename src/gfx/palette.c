/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Palette fade arithmetic and the frame hash (lrvne/gfx.h): pure functions over bytes. */
#include "lrvne/gfx.h"

int lrv_pal_step_toward(uint8_t *cur, const uint8_t *target, size_t n)
{
    size_t i;
    int changed = 0;
    for (i = 0; i < n; i++) {
        if (cur[i] < target[i]) {
            cur[i]++;
            changed++;
        } else if (cur[i] > target[i]) {
            cur[i]--;
            changed++;
        }
    }
    return changed;
}

void lrv_pal_lerp(uint8_t *out, const uint8_t *from, const uint8_t *to, size_t n, int step,
                  int steps)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int a = from[i], b = to[i];
        out[i] = (uint8_t)(a + (b - a) * step / steps);     /* C99: toward zero */
    }
}

int lrv_pal_bright(int v, int level, int max)
{
    return (v * level * 2 + max) / (2 * max);
}

void lrv_pal_brightness(uint8_t *out, const uint8_t *in, size_t n, int level, int max)
{
    size_t i;
    if (level < 0) level = 0;
    if (level > max) level = max;
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)lrv_pal_bright(in[i], level, max);
}

uint32_t lrv_frame_hash32(const struct lrv_frame *frame, int colours)
{
    const struct lrv_surface *s = frame->page;
    int planes = lrv_format_planes(s->format);
    int scroll = frame->scroll_line % s->h;
    uint32_t h = 2166136261u;
    int p, y, x;

    if (scroll < 0)
        scroll += s->h;
    for (y = 0; y < s->h; y++) {
        int sy = (y + scroll) % s->h;
        for (p = 0; p < planes; p++) {
            const uint8_t *row = s->plane[p] + (size_t)sy * (size_t)s->stride;
            for (x = 0; x < s->stride; x++)
                h = (h ^ row[x]) * 16777619u;
        }
    }
    for (x = 0; x < colours * 3; x++)
        h = (h ^ frame->rgb[x]) * 16777619u;
    h = (h ^ (uint32_t)(frame->display_on ? 1 : 0)) * 16777619u;
    return h;
}
