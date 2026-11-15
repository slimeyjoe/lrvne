/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Frame copies, hashes, snapshot lines, PPM and WAV files (shot.h). */
#include <stdlib.h>
#include <string.h>
#include "lrvne/core.h"
#include "lrvne/gfx.h"
#include "jsonout.h"
#include "shot.h"

#define NPLANES 4               /* LRV_FMT_PLANAR4 */

static int supported(const struct lrv_frame *f)
{
    const struct lrv_surface *pg = f->page;
    return pg && pg->format == LRV_FMT_PLANAR4 && pg->w > 0 && pg->h > 0 &&
           pg->stride >= (pg->w + 7) / 8 && f->colours > 0 && f->colours <= LRV_PALETTE_MAX;
}

/* the page line shown at display line y */
static int source_line(const struct lrv_frame *f, int y)
{
    int h = f->page->h, sy = (y + f->scroll_line) % h;
    return sy < 0 ? sy + h : sy;
}

int lrv_shot_capture(struct lrv_shot *s, const struct lrv_frame *f)
{
    size_t plane, pal, need;
    int p;

    if (!supported(f))
        return LRV_ERR_RANGE;
    plane = (size_t)lrv_surface_plane_bytes(f->page);
    pal = (size_t)f->colours * 3;
    need = NPLANES * plane + pal;
    if (need != s->size) {
        uint8_t *mem = realloc(s->mem, need);
        if (!mem)
            return LRV_ERR_NOMEM;
        s->mem = mem;
        s->size = need;
    }
    s->page = *f->page;
    for (p = 0; p < NPLANES; p++) {
        s->page.plane[p] = s->mem + (size_t)p * plane;
        memcpy(s->page.plane[p], f->page->plane[p], plane);
    }
    memcpy(s->mem + NPLANES * plane, f->rgb, pal);
    memset(&s->frame, 0, sizeof s->frame);
    s->frame.page = &s->page;
    s->frame.scroll_line = f->scroll_line;
    s->frame.rgb = s->mem + NPLANES * plane;
    s->frame.colours = f->colours;
    s->frame.rgb_bits = f->rgb_bits;
    s->frame.display_on = f->display_on;
    s->valid = 1;
    return LRV_OK;
}

void lrv_shot_free(struct lrv_shot *s)
{
    free(s->mem);
    memset(s, 0, sizeof *s);
}

uint64_t lrv_shot_hash64(const struct lrv_frame *f)
{
    const struct lrv_surface *pg = f->page;
    uint64_t h = 1469598103934665603ull;
    size_t i, plane = (size_t)lrv_surface_plane_bytes(pg), npal = (size_t)f->colours * 3;
    int p;

    for (p = 0; p < NPLANES; p++)
        for (i = 0; i < plane; i++)
            h = (h ^ pg->plane[p][i]) * 1099511628211ull;
    for (i = 0; i < npal; i++)
        h = (h ^ f->rgb[i]) * 1099511628211ull;
    h = (h ^ (uint8_t)(f->display_on != 0)) * 1099511628211ull;
    h = (h ^ (uint8_t)f->scroll_line) * 1099511628211ull;
    h = (h ^ (uint8_t)(f->scroll_line >> 8)) * 1099511628211ull;
    return h;
}

size_t lrv_shot_indices_size(const struct lrv_frame *f)
{
    return (size_t)(f->page->w / 8) * 4 * (size_t)f->page->h;
}

void lrv_shot_indices(const struct lrv_frame *f, uint8_t *out)
{
    const struct lrv_surface *pg = f->page;
    int cols = pg->w / 8, y, x, k;

    for (y = 0; y < pg->h; y++) {
        size_t off = (size_t)source_line(f, y) * (size_t)pg->stride;
        const uint8_t *p0 = pg->plane[0] + off, *p1 = pg->plane[1] + off;
        const uint8_t *p2 = pg->plane[2] + off, *p3 = pg->plane[3] + off;
        for (x = 0; x < cols; x++) {
            unsigned b0 = p0[x], b1 = p1[x], b2 = p2[x], b3 = p3[x];
            for (k = 0; k < 4; k++) {
                int s1 = 7 - 2 * k, s0 = 6 - 2 * k;
                unsigned hi = ((b0 >> s1) & 1) | (((b1 >> s1) & 1) << 1) |
                              (((b2 >> s1) & 1) << 2) | (((b3 >> s1) & 1) << 3);
                unsigned lo = ((b0 >> s0) & 1) | (((b1 >> s0) & 1) << 1) |
                              (((b2 >> s0) & 1) << 2) | (((b3 >> s0) & 1) << 3);
                *out++ = (uint8_t)((hi << 4) | lo);
            }
        }
    }
}

void lrv_shot_json_body(FILE *out, const struct lrv_frame *f, int with_data)
{
    fprintf(out, "\"pal\":");
    lrv_json_hex(out, f->rgb, (size_t)f->colours * 3);
    fprintf(out, ",\"gon\":%d,\"scroll\":%d", f->display_on ? 1 : 0, f->scroll_line);
    if (with_data) {
        size_t n = lrv_shot_indices_size(f);
        uint8_t *idx = malloc(n ? n : 1);
        fprintf(out, ",\"data\":");
        if (idx) {
            lrv_shot_indices(f, idx);
            lrv_json_b64(out, idx, n);
            free(idx);
        } else {
            fprintf(out, "null");
        }
    }
}

void lrv_shot_emit_snap(FILE *out, const struct lrv_frame *f, const char *name, uint32_t tick)
{
    fprintf(out, "{\"ev\":\"snap\",\"name\":");
    lrv_json_str(out, name);
    fprintf(out, ",\"t\":%lu,\"hash\":\"%08lx\",\"size\":[%d,%d],", (unsigned long)tick,
            (unsigned long)lrv_frame_hash32(f), f->page->w, f->page->h);
    lrv_shot_json_body(out, f, 1);
    fprintf(out, "}\n");
    fflush(out);
}

int lrv_shot_write_ppm(const char *path, const struct lrv_frame *f)
{
    const struct lrv_surface *pg = f->page;
    unsigned max = f->rgb_bits > 0 && f->rgb_bits < 8 ? (1u << f->rgb_bits) - 1u : 255u;
    FILE *out;
    int y, x, i, rc;

    if (!supported(f))
        return LRV_ERR_RANGE;
    out = fopen(path, "wb");
    if (!out)
        return LRV_ERR_IO;
    fprintf(out, "P6\n%d %d\n255\n", pg->w, pg->h);
    for (y = 0; y < pg->h; y++) {
        size_t line = (size_t)source_line(f, y) * (size_t)pg->stride;
        for (x = 0; x < pg->w; x++) {
            size_t off = line + (size_t)(x / 8);
            int bit = 7 - (x & 7);
            unsigned idx = ((pg->plane[0][off] >> bit) & 1u) |
                           (((pg->plane[1][off] >> bit) & 1u) << 1) |
                           (((pg->plane[2][off] >> bit) & 1u) << 2) |
                           (((pg->plane[3][off] >> bit) & 1u) << 3);
            uint8_t px[3] = { 0, 0, 0 };
            if (f->display_on && (int)idx < f->colours)
                for (i = 0; i < 3; i++)
                    px[i] = (uint8_t)(f->rgb[idx * 3 + (unsigned)i] * 255u / max);
            fwrite(px, 1, 3, out);
        }
    }
    rc = ferror(out) ? LRV_ERR_IO : LRV_OK;
    if (fclose(out) != 0)
        rc = LRV_ERR_IO;
    return rc;
}

/* ---- WAV --------------------------------------------------------------------------------- */

#define WAV_HEADER 44

static void put32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)(v >> 24), f);
}

int lrv_wav_open(struct lrv_wav *w, const char *path)
{
    w->frames = 0;
    w->f = fopen(path, "wb");
    if (!w->f)
        return LRV_ERR_IO;
    fseek(w->f, WAV_HEADER, SEEK_SET);
    return LRV_OK;
}

void lrv_wav_write(struct lrv_wav *w, const int16_t *pcm, int frames)
{
    int i;
    if (!w->f)
        return;
    for (i = 0; i < frames; i++) {              /* little-endian on every host */
        fputc(pcm[i] & 0xFF, w->f);
        fputc((pcm[i] >> 8) & 0xFF, w->f);
    }
    w->frames += (uint32_t)(frames > 0 ? frames : 0);
}

void lrv_wav_sync(struct lrv_wav *w, uint32_t rate)
{
    if (!w->f)
        return;
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f);
    put32(w->f, 36 + w->frames * 2);
    fwrite("WAVEfmt ", 1, 8, w->f);
    put32(w->f, 16);
    put32(w->f, 0x00010001u);                   /* PCM, one channel */
    put32(w->f, rate);
    put32(w->f, rate * 2);
    put32(w->f, 0x00100002u);                   /* block of 2 bytes, 16 bits */
    fwrite("data", 1, 4, w->f);
    put32(w->f, w->frames * 2);
    fseek(w->f, 0, SEEK_END);
    fflush(w->f);
}

void lrv_wav_close(struct lrv_wav *w, uint32_t rate)
{
    if (!w->f)
        return;
    lrv_wav_sync(w, rate);
    fclose(w->f);
    w->f = 0;
}

static void wav_sink(void *ctx, const int16_t *pcm, int frames)
{
    lrv_wav_write((struct lrv_wav *)ctx, pcm, frames);
}

int lrv_wav_render(const struct lrv_app *app, const struct lrv_hal *hal,
                   const struct lrv_run_cfg *cfg, const char *path, const char *song,
                   double seconds)
{
    struct lrv_wav w;
    uint32_t rate = 0;
    int rc;

    if (!app->render_music) {
        fprintf(stderr, "%s: this engine cannot render music to a file\n", app->name);
        return 2;
    }
    if (!song) {
        fprintf(stderr, "%s: --render-wav needs --song NAME\n", app->name);
        return 2;
    }
    if (lrv_wav_open(&w, path) != LRV_OK) {
        perror(path);
        return 1;
    }
    rc = app->render_music(app->ctx, hal, cfg, song, seconds, &rate, wav_sink, &w);
    fprintf(stderr, "%s: %lu frames at %lu Hz\n", path, (unsigned long)w.frames,
            (unsigned long)rate);
    lrv_wav_close(&w, rate);
    if (rc != LRV_OK)
        fprintf(stderr, "%s: %s: %s\n", app->name, song, lrv_strerror(rc));
    return rc == LRV_OK ? 0 : 1;
}
