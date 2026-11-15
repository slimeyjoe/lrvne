/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lrvdemo: the smallest engine built on lrvne. It is the in-tree consumer of the contract
 * between an engine and a backend (lrvne/app.h) and shows every part of it at work:
 *
 *   - struct lrv_app and a main() that is one call;
 *   - an option of its own, --speed N;
 *   - the main pack, when there is one, and a game profile read from it (keys: the base ones
 *     and "speed"); the demo also runs without a pack;
 *   - a video mode like the PC-98's (640x400, four bit-planes, 16 colours of 4 bits per gun,
 *     56.42 Hz), a page it draws into, dirty rectangles and palette changes;
 *   - the frame loop and the input model of lrvne/sys.h;
 *   - sound through the audio HAL in whichever of the two delivery models the backend has;
 *   - a storage record (how many times the demo ran);
 *   - trace events for test automation, and quit and reset.
 *
 * On the screen: sixteen colour bars whose palette rotates, a block that crosses the screen
 * `speed` byte columns a frame, a row of cells that light up while keys are held (one per key
 * of lrvne/input.h; the last two are the mouse buttons), and an inverted square at the pointer.
 * Keys: Space pauses, P stops the palette, Esc quits; every key press beeps. */
#include <string.h>
#include "lrvne/app.h"
#include "lrvne/core.h"
#include "lrvne/endian.h"
#include "lrvne/pack.h"
#include "lrvne/profile.h"
#include "lrvne/sys.h"

#define DEMO_VERSION "1.0"
#define DEMO_COLOURS 16
#define DEMO_PLANES  4
#define BLOCK_Y      216        /* the moving block: 4 byte columns x 32 lines */
#define BLOCK_H      32
#define BLOCK_COLS   4
#define CELLS_Y      300        /* the key cells: 8 lines each row */
#define POINTER_SIZE 8

/* the machine the demo pretends to be */
static const struct lrv_video_mode demo_mode = {
    LRV_FMT_PLANAR4, 640, 400, 4, 3, 17724193u, DEMO_COLOURS, 4
};

/* the demo's trace kinds and their names for the control protocol, and its wait kinds */
enum { DEMO_TR_KEY = LRV_TR_ENGINE, DEMO_TR_RECORD };
static const char *const demo_trace_names[] = { "key", "record" };
enum { DEMO_WAIT_KEY = 0 };

struct demo_profile {
    struct lrv_profile_base base;
    int speed;
};

static const struct lrv_pfield demo_fields[] = {
    LRV_PROFILE_BASE_FIELDS(struct demo_profile, base),
    LRV_PF_INT("speed", struct demo_profile, speed, 1, 8)
};

struct demo {
    int speed_option;                   /* --speed, 0 = not given */

    const struct lrv_hal *hal;
    struct lrv_sys sys;
    struct demo_profile profile;
    struct lrv_surface page;
    struct lrv_rect dirty;              /* what changed since the last present */
    uint8_t palette[DEMO_COLOURS * 3];
    int block_col;
    int paused, palette_stopped;
    int pointer_x, pointer_y, pointer_shown;
    uint8_t cells[LRV_K_COUNT + 2];     /* what each cell shows now */

    /* the beep; in pull mode the audio thread reads these, under the audio lock */
    int rate, amplitude;
    uint32_t tone_phase, tone_step;     /* 32-bit phase: the top bit is the square wave */
    int tone_left;                      /* frames the beep still lasts */
};

/* ---- drawing ------------------------------------------------------------------------------ */

/* fills byte columns [x0, x1) x lines [y0, y1) with a colour; invert != 0 flips them instead */
static void demo_fill(struct demo *d, int x0, int y0, int x1, int y1, int colour, int invert)
{
    int p, y;
    for (p = 0; p < DEMO_PLANES; p++)
        for (y = y0; y < y1; y++) {
            uint8_t *row = d->page.plane[p] + y * d->page.stride;
            int x;
            if (!invert)
                memset(row + x0, (colour >> p) & 1 ? 0xFF : 0x00, (size_t)(x1 - x0));
            else
                for (x = x0; x < x1; x++)
                    row[x] ^= 0xFF;
        }
    lrv_rect_add(&d->dirty, x0, y0, x1, y1);
}

static void demo_pointer(struct demo *d)
{
    int col = d->pointer_x / 8, y1 = d->pointer_y + POINTER_SIZE;
    if (y1 > d->page.h)
        y1 = d->page.h;
    demo_fill(d, col, d->pointer_y, col + 1, y1, 0, 1);
    d->pointer_shown = !d->pointer_shown;
}

static void demo_default_palette(struct demo *d)
{
    int i;
    for (i = 0; i < DEMO_COLOURS; i++) {            /* a grey ramp tinted by the index's bits */
        d->palette[i * 3 + 0] = (uint8_t)(i & 1 ? 15 : i);
        d->palette[i * 3 + 1] = (uint8_t)(i & 2 ? 15 : i);
        d->palette[i * 3 + 2] = (uint8_t)(i & 4 ? 15 : i);
    }
    memset(d->palette, 0, 3);                       /* colour 0 stays black */
}

static void demo_draw_screen(struct demo *d)
{
    int cols = d->page.w / 8, i;
    demo_fill(d, 0, 0, cols, d->page.h, 0, 0);
    for (i = 0; i < DEMO_COLOURS; i++)
        demo_fill(d, i * cols / DEMO_COLOURS, 0, (i + 1) * cols / DEMO_COLOURS, 200, i, 0);
    memset(d->cells, 0xFF, sizeof d->cells);        /* every cell is drawn on the next frame */
    d->pointer_shown = 0;
    d->block_col = 0;
}

/* the hook lrv_sys calls to show a frame */
static void demo_present(void *ctx)
{
    struct demo *d = ctx;
    struct lrv_frame frame;
    memset(&frame, 0, sizeof frame);
    frame.page = &d->page;
    frame.dirty = &d->dirty;
    frame.rgb = d->palette;
    frame.colours = demo_mode.colours;
    frame.rgb_bits = demo_mode.rgb_bits;
    frame.display_on = 1;
    d->hal->video_present(&frame);
    lrv_rect_clear(&d->dirty);
}

/* ---- sound -------------------------------------------------------------------------------- */

/* The render callback: called by the backend's audio thread in pull mode, by demo_pump below
 * in push mode. */
static void demo_render(void *ctx, int16_t *pcm, int frames)
{
    struct demo *d = ctx;
    int i;
    for (i = 0; i < frames; i++) {
        pcm[i] = 0;
        if (d->tone_left > 0) {
            d->tone_left--;
            d->tone_phase += d->tone_step;
            pcm[i] = (int16_t)(d->tone_phase & 0x80000000u ? d->amplitude : -d->amplitude);
        }
    }
}

/* push mode: fill what the backend's buffer can take, once a frame */
static void demo_pump(void *ctx)
{
    struct demo *d = ctx;
    int16_t buf[512];
    int n;
    while ((n = d->hal->audio_space()) > 0) {
        if (n > (int)(sizeof buf / sizeof buf[0]))
            n = (int)(sizeof buf / sizeof buf[0]);
        demo_render(d, buf, n);
        d->hal->audio_push(buf, n);
    }
}

static void demo_beep(struct demo *d, int hz)
{
    if (!d->rate)
        return;
    if (d->hal->audio_lock)             /* pull mode: the audio thread reads the tone */
        d->hal->audio_lock();
    d->tone_step = (uint32_t)(((uint64_t)hz << 32) / (uint32_t)d->rate);
    d->tone_left = d->rate / 8;
    if (d->hal->audio_lock)
        d->hal->audio_unlock();
}

/* ---- one frame ---------------------------------------------------------------------------- */

static void demo_cell(struct demo *d, int index, int lit)
{
    int cols = d->page.w / 8, x = index % cols, y = CELLS_Y + 8 * (index / cols);
    if (d->cells[index] == lit)
        return;
    d->cells[index] = (uint8_t)lit;
    demo_fill(d, x, y, x + 1, y + 6, lit ? 15 : 8, 0);
}

/* Returns 0 when the player quit. */
static int demo_step(struct demo *d, int speed)
{
    struct lrv_sys *s = &d->sys;
    struct lrv_keypress k;
    int cols = d->page.w / 8, key, running = 1;

    while (lrv_sys_key_read(s, &k)) {
        lrv_sys_trace(s, DEMO_TR_KEY, lrv_key_name(k.key), 0, k.key, k.chr, k.mods, 0);
        demo_beep(d, 220 + 8 * k.key);
        if (k.key == LRV_K_SPACE)
            d->paused = !d->paused;
        else if (k.key == LRV_K_P)
            d->palette_stopped = !d->palette_stopped;
        else if (k.key == LRV_K_ESC)
            running = 0;
    }
    if (lrv_sys_mouse_pressed(s, LRV_EV_MOUSE_L | LRV_EV_MOUSE_R))
        demo_beep(d, 880);

    if (d->pointer_shown)
        demo_pointer(d);                            /* take the pointer off the page */
    if (!d->paused) {
        demo_fill(d, d->block_col, BLOCK_Y, d->block_col + BLOCK_COLS, BLOCK_Y + BLOCK_H, 0, 0);
        d->block_col = (d->block_col + speed) % (cols - BLOCK_COLS + 1);
        demo_fill(d, d->block_col, BLOCK_Y, d->block_col + BLOCK_COLS, BLOCK_Y + BLOCK_H, 15, 0);
    }
    for (key = 1; key < LRV_K_COUNT; key++)
        demo_cell(d, key, lrv_sys_key_down(s, key));
    demo_cell(d, LRV_K_COUNT, (lrv_sys_mouse_buttons(s) & LRV_BTN_L) != 0);
    demo_cell(d, LRV_K_COUNT + 1, (lrv_sys_mouse_buttons(s) & LRV_BTN_R) != 0);
    d->pointer_x = s->mouse_x;
    d->pointer_y = s->mouse_y;
    demo_pointer(d);

    if (!d->palette_stopped && s->frame % 8 == 0) { /* rotate colours 1..15: no redraw needed */
        uint8_t first[3];
        memcpy(first, d->palette + 3, 3);
        memmove(d->palette + 3, d->palette + 6, (DEMO_COLOURS - 2) * 3);
        memcpy(d->palette + (DEMO_COLOURS - 1) * 3, first, 3);
    }
    return running;
}

/* ---- the game ----------------------------------------------------------------------------- */

/* the pack is optional here; an engine proper would fail without one */
static void demo_load_profile(struct demo *d, const struct lrv_run_cfg *cfg)
{
    struct lrv_pack pack;
    uint32_t size;
    void *probe = d->hal->pack_open(LRV_PACK_MAIN, &size);

    strcpy(d->profile.base.id, "demo");             /* the defaults a profile overrides */
    strcpy(d->profile.base.title, "lrvne demo");
    d->profile.speed = 1;
    if (!probe) {
        lrv_log(d->hal, LRV_LOG_INFO, "demo: no pack, using the built-in profile");
        return;
    }
    d->hal->pack_close(probe);
    if (lrv_pack_open(&pack, d->hal, LRV_PACK_MAIN) != LRV_OK)
        return;
    if (cfg->lang)
        lrv_pack_set_lang(&pack, lrv_pack_lang_id(&pack, cfg->lang));
    lrv_profile_load(&pack, demo_fields, (int)(sizeof demo_fields / sizeof demo_fields[0]),
                     &d->profile);
    lrv_pack_close(&pack);
}

/* counts the runs in a storage record */
static void demo_count_run(struct demo *d)
{
    uint8_t rec[4] = { 0, 0, 0, 0 };
    size_t len = 0;
    uint32_t runs = 0;

    if (d->hal->storage_open(d->profile.base.id) != LRV_OK) {
        lrv_log(d->hal, LRV_LOG_WARN, "demo: no storage");
        return;
    }
    if (d->hal->storage_read("runs", rec, sizeof rec, &len) == LRV_OK && len == sizeof rec)
        runs = lrv_rd32le(rec);
    lrv_wr32le(rec, ++runs);
    if (d->hal->storage_write("runs", rec, sizeof rec) != LRV_OK)
        lrv_log(d->hal, LRV_LOG_WARN, "demo: cannot write the record");
    lrv_sys_trace(&d->sys, DEMO_TR_RECORD, "runs", 0, (int)runs, 0, 0, 0);
}

static int demo_run(void *ctx, const struct lrv_hal *hal, const struct lrv_run_cfg *cfg)
{
    struct demo *d = ctx;
    struct lrv_video_cfg video;
    struct lrv_audio_source src;
    size_t plane;
    int p, speed, volume = cfg->volume < 0 ? 0 : cfg->volume ? cfg->volume : 100;

    d->hal = hal;
    demo_load_profile(d, cfg);
    speed = d->speed_option ? d->speed_option : d->profile.speed;

    memset(&video, 0, sizeof video);
    video.mode = demo_mode;
    video.title = d->profile.base.title;
    video.fullscreen = cfg->fullscreen;
    video.scale_mode = cfg->scale_mode;
    video.host_mode = cfg->host_mode;
    if (hal->video_open(&video) != LRV_OK)
        lrv_fatal(hal, "demo: cannot open the display");

    d->page.format = demo_mode.format;
    d->page.w = demo_mode.w;
    d->page.h = demo_mode.h;
    d->page.stride = demo_mode.w / 8;
    plane = lrv_surface_plane_bytes(&d->page);
    d->page.plane[0] = lrv_xalloc(hal, DEMO_PLANES * plane);
    for (p = 1; p < DEMO_PLANES; p++)
        d->page.plane[p] = d->page.plane[0] + (size_t)p * plane;

    lrv_sys_init(&d->sys, hal, demo_mode.w, demo_mode.h);
    lrv_sys_set_present(&d->sys, demo_present, d);
    demo_count_run(d);

    /* the backend may accept another rate than the one asked for, or none */
    d->amplitude = 3000 * volume / 100;
    src.render = demo_render;
    src.skip = 0;
    src.ctx = d;
    d->rate = cfg->audio_rate > 0 ? hal->audio_open(cfg->audio_rate, &src) : 0;
    if (d->rate && !hal->audio_lock)
        lrv_sys_add_hook(&d->sys, LRV_HOOK_PUMP, demo_pump, d);

    for (;;) {                                      /* a reset comes back here */
        lrv_rect_clear(&d->dirty);
        demo_default_palette(d);
        demo_draw_screen(d);
        d->paused = d->palette_stopped = 0;
        lrv_sys_trace(&d->sys, LRV_TR_WAIT, 0, 0, DEMO_WAIT_KEY, 0, 0, 0);
        do
            lrv_sys_frame_wait(&d->sys);
        while (!lrv_sys_quitting(&d->sys) && demo_step(d, speed));
        lrv_sys_trace(&d->sys, LRV_TR_WAIT, 0, 0, DEMO_WAIT_KEY, 1, 0, 0);
        if (!lrv_sys_resetting(&d->sys))
            break;
        lrv_sys_reset_done(&d->sys);
        lrv_sys_trace(&d->sys, LRV_TR_RESET, 0, 0, 0, 0, 0, 0);
    }

    lrv_sys_trace(&d->sys, LRV_TR_QUIT, 0, 0, 0, 0, 0, 0);
    if (d->rate)
        hal->audio_close();
    hal->video_close();
    lrv_free(hal, d->page.plane[0], DEMO_PLANES * plane);
    return 0;
}

/* Offline rendering (a backend's --render-wav): the "song" is a frequency in Hz. */
static int demo_render_music(void *ctx, const struct lrv_hal *hal, const struct lrv_run_cfg *cfg,
                             const char *name, double seconds, uint32_t *rate,
                             void (*sink)(void *sctx, const int16_t *pcm, int frames), void *sctx)
{
    struct demo *d = ctx;
    int16_t buf[512];
    long left, hz = 0;

    (void)hal;
    for (; *name >= '0' && *name <= '9' && hz < 20000; name++)
        hz = hz * 10 + (*name - '0');
    if (*name || hz < 20 || hz > 20000 || cfg->audio_rate <= 0)
        return LRV_ERR_NOTFOUND;
    *rate = (uint32_t)cfg->audio_rate;
    d->amplitude = 3000;
    d->tone_step = (uint32_t)(((uint64_t)hz << 32) / *rate);
    d->tone_left = (int)(seconds * cfg->audio_rate);
    for (left = d->tone_left; left > 0; left -= 512) {
        int n = left > 512 ? 512 : (int)left;
        demo_render(d, buf, n);
        sink(sctx, buf, n);
    }
    return LRV_OK;
}

/* ---- the engine as a backend sees it ------------------------------------------------------ */

static int demo_option(void *ctx, const char *name, const char *arg)
{
    struct demo *d = ctx;
    if (!strcmp(name, "speed") && arg[0] >= '1' && arg[0] <= '8' && !arg[1]) {
        d->speed_option = arg[0] - '0';
        return 0;
    }
    return 1;
}

static int demo_wait_is_input(int wait_kind)
{
    return wait_kind == DEMO_WAIT_KEY;
}

static const struct lrv_option demo_options[] = {
    { "speed", LRV_OPT_ARG, "1..8", "byte columns the block moves in a frame" }
};

static struct demo demo;

static const struct lrv_app demo_app = {
    .name = "lrvdemo",
    .version = DEMO_VERSION,
    .ctx = &demo,
    .options = demo_options,
    .noptions = 1,
    .option = demo_option,
    .help = "keys: Space pauses the block, P stops the palette, Esc quits",
    .run = demo_run,
    .render_music = demo_render_music,
    .trace_names = demo_trace_names,
    .ntrace_names = 2,
    .wait_is_input = demo_wait_is_input
};

int main(int argc, char **argv)
{
    return lrv_app_main(&demo_app, argc, argv);
}
