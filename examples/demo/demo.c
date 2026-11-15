/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lrvdemo: a small engine on lrvne, written the way a real one is. It runs no game; it is the
 * in-tree consumer of the library and the place to read how the parts go together:
 *
 *   app.h      struct lrv_app, a main() that is one call, an option of its own (--speed N)
 *   pack.h     the main pack when there is one (the demo also runs without: an engine proper
 *   profile.h  would not), the game profile in it (the base keys and "speed"), and fonts
 *   pc98.h     the machine: its video mode, and its keyboard as the originals read it
 *   gfx.h      struct lrv_screen: two pages, dirty marking, the palette, a fade, and an
 *              overlay that draws a software pointer only into the presented picture
 *   font.h     text through a glyph table, when the pack has one ("latin8", "wide16")
 *   sys.h      the frame loop with the screen's present and the mixer's pump hooked in, the
 *              input queue, quit and reset, trace events for test automation
 *   mixer.h    the audio stream in whichever delivery model the backend has, in front of
 *   opnq.h     a register-write queue and an FM chip core, driven by a clock-driven
 *   opn.h      sequencer: a tune written directly as register writes
 *   hal.h      a storage record (how many times the demo ran)
 *
 * On the screen: sixteen colour bars whose palette rotates, a block that crosses the screen
 * `speed` byte columns a frame, and a row of cells, one per PC-98 scan code, lit while that
 * key is down (the last two are the mouse buttons). With a font the title and the last key
 * press as the PC-98 delivers it are printed. Keys: Space pauses the block, P stops the
 * palette, Tab shows the other page, Esc quits; every key press and click beeps. */
#include <stdio.h>
#include <string.h>
#include "lrvne/app.h"
#include "lrvne/core.h"
#include "lrvne/endian.h"
#include "lrvne/font.h"
#include "lrvne/gfx.h"
#include "lrvne/mixer.h"
#include "lrvne/opnq.h"
#include "lrvne/pack.h"
#include "lrvne/pc98.h"
#include "lrvne/profile.h"
#include "lrvne/sys.h"

#define DEMO_VERSION "2.0"
#define COLS         LRV_PC98_STRIDE    /* byte columns of the screen */
#define BARS_H       160
#define TEXT_Y       168                /* two lines of text, 16 lines each */
#define BLOCK_Y      216                /* the moving block: 4 byte columns x 32 lines */
#define BLOCK_H      32
#define BLOCK_COLS   4
#define CELLS_Y      300                /* the key cells: 8 lines each row */
#define NCELLS       (128 + 2)          /* the scan codes, then the two mouse buttons */
#define POINTER_H    8
#define FADE_STEPS   16
#define NOTE_CLOCKS  (LRV_OPN_CLOCK_PC98 / 5)   /* a note of the tune: 200 ms of chip time */

/* the demo's trace kinds and their names for the control protocol, and its wait kinds */
enum { DEMO_TR_KEY = LRV_TR_ENGINE, DEMO_TR_RECORD };
static const char *const demo_trace_names[] = { "key", "record" };
enum { DEMO_WAIT_KEY = 0 };

/* the profile: what every game states, and the one key this engine adds */
struct demo_profile {
    struct lrv_profile_base base;
    int speed;
};

static const struct lrv_pfield demo_fields[] = {
    LRV_PROFILE_BASE_FIELDS(struct demo_profile, base),
    LRV_PF_INT("speed", struct demo_profile, speed, 1, 8)
};

/* All of the engine's state: lrvne keeps none of its own, and neither does the demo. */
struct demo {
    int speed_option;                   /* --speed, 0 = not given */

    const struct lrv_hal *hal;
    struct lrv_sys sys;
    struct lrv_screen screen;
    struct lrv_pack pack;
    int have_pack;
    struct demo_profile profile;
    struct lrv_font latin8, wide16;     /* empty without a pack that has them */
    uint8_t palette[16 * 3];            /* at full brightness; the DAC gets it faded */
    int fade, block_col, paused, palette_stopped;
    uint8_t cells[NCELLS];              /* what each cell shows now */
    int ptr_col, ptr_y;                 /* the pointer overlay: where it was drawn, and */
    uint8_t ptr_under[4][POINTER_H][2]; /* what it covers */

    /* sound; in pull mode the sequencer runs on the audio thread, under the audio lock */
    struct lrv_mixer mixer;
    struct lrv_opnq opnq;
    uint64_t seq_next;                  /* the master clock of the next note */
    int seq_step;
    int blip;                           /* an SSG tone period asked for by the game, 0 = none */
};

/* ---- drawing ------------------------------------------------------------------------------ */

/* Fills byte columns [x0, x1) x lines [y0, y1) of the access page. Every write into a page is
 * followed by a mark: backends upload only what is dirty. */
static void demo_fill(struct demo *d, int x0, int y0, int x1, int y1, int colour)
{
    struct lrv_surface *s = lrv_screen_access(&d->screen);
    int p, y;
    for (p = 0; p < 4; p++)
        for (y = y0; y < y1; y++)
            memset(s->plane[p] + y * s->stride + x0, (colour >> p) & 1 ? 0xFF : 0,
                   (size_t)(x1 - x0));
    lrv_screen_mark(&d->screen, d->screen.access_page, x0, y0, x1, y1);
}

/* A line of UTF-8 text at byte column `col`: one or two columns a character, as the tables
 * have it. Draws nothing when the pack brought no font. */
static void demo_text(struct demo *d, int col, int y, const char *text, int colour)
{
    struct lrv_surface *s = lrv_screen_access(&d->screen);
    struct lrv_text_ch ch;
    struct lrv_rect dirty;
    uint8_t rows[32];

    if (!d->latin8.data)
        return;
    lrv_rect_clear(&dirty);
    for (; *text && col < COLS - 1; text += ch.len) {
        int w;
        lrv_text_next_utf8((const uint8_t *)text, &d->latin8, &ch);
        w = lrv_font_uni_glyph(&d->latin8, &d->wide16, ch.code, rows);
        if (w && lrv_glyph_blit(s, col, y, rows, w, LRV_GLYPH_ROWS, colour, 0, 15, &dirty))
            col += w;
    }
    lrv_screen_mark_surface(&d->screen, s, &dirty);
}

/* The software pointer, an overlay of the screen: `apply` draws it into the displayed page
 * just before a present and keeps what it covers, `restore` puts that back right after, so
 * the engine never finds the pointer in its pages. */
static void demo_pointer(struct demo *d, struct lrv_surface *page, struct lrv_rect *dirty,
                         int draw)
{
    static const uint8_t shape[POINTER_H] = { 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0xB0, 0x18 };
    int shift = d->sys.mouse_x & 7, p, r, k;
    struct lrv_rect rect;

    if (draw) {
        d->ptr_col = d->sys.mouse_x / 8;
        d->ptr_y = d->sys.mouse_y;
    }
    for (p = 0; p < 4; p++)
        for (r = 0; r < POINTER_H && d->ptr_y + r < page->h; r++)
            for (k = 0; k < 2 && d->ptr_col + k < page->stride; k++) {
                uint8_t *b = page->plane[p] + (d->ptr_y + r) * page->stride + d->ptr_col + k;
                if (draw) {
                    d->ptr_under[p][r][k] = *b;
                    *b |= (uint8_t)((shape[r] << 8 >> shift) >> (k ? 0 : 8));   /* colour 15 */
                } else {
                    *b = d->ptr_under[p][r][k];
                }
            }
    rect.x0 = d->ptr_col; rect.y0 = d->ptr_y;
    rect.x1 = d->ptr_col + 2; rect.y1 = d->ptr_y + POINTER_H;
    lrv_rect_clip(&rect, page);
    lrv_rect_union(dirty, &rect);
}

static void demo_pointer_apply(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty)
{
    demo_pointer(ctx, page, dirty, 1);
}

static void demo_pointer_restore(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty)
{
    demo_pointer(ctx, page, dirty, 0);
}

/* Both pages from nothing: page 0 is the test pattern, page 1 (Tab) the bars lying down. */
static void demo_draw_screen(struct demo *d)
{
    int i;
    lrv_screen_reset(&d->screen);                   /* cleared, page 0 shown, all black */
    for (i = 0; i < 16; i++) {                      /* a grey ramp tinted by the index's bits */
        d->palette[i * 3 + 0] = (uint8_t)(i & 1 ? 15 : i);
        d->palette[i * 3 + 1] = (uint8_t)(i & 2 ? 15 : i);
        d->palette[i * 3 + 2] = (uint8_t)(i & 4 ? 15 : i);
    }
    memset(d->palette, 0, 3);                       /* colour 0 stays black */
    lrv_screen_set_access(&d->screen, 1);           /* drawn while hidden */
    for (i = 0; i < 16; i++)
        demo_fill(d, 0, i * 25, COLS, (i + 1) * 25, i);
    demo_text(d, 2, 4, "page 1 - Tab goes back", 15);
    lrv_screen_set_access(&d->screen, 0);
    for (i = 0; i < 16; i++)
        demo_fill(d, i * COLS / 16, 0, (i + 1) * COLS / 16, BARS_H, i);
    demo_text(d, 1, TEXT_Y, d->profile.base.title, 15);
    memset(d->cells, 0xFF, sizeof d->cells);        /* every cell is drawn on the next frame */
    d->fade = d->block_col = d->paused = d->palette_stopped = 0;
}

/* ---- sound -------------------------------------------------------------------------------- */

static void demo_reg(struct demo *d, uint64_t clock, int reg, int val)
{
    lrv_opnq_write_at(&d->opnq, clock, (uint8_t)reg, (uint8_t)val);
}

/* The sequencer, what a music driver is to an engine: the queue asks it to perform everything
 * up to a master clock before it renders a block, and it stamps each register write with the
 * clock it happens at. Called by lrv_opnq_render, so with the audio lock held. */
static void demo_seq_run(void *ctx, uint64_t clock)
{
    /* F-numbers of C D E G A (block 4) and rests: a pentatonic phrase, repeated */
    static const uint16_t tune[] = { 618, 779, 926, 0, 1040, 926, 779, 694, 618, 0, 926, 0 };
    struct demo *d = ctx;

    for (; d->seq_next <= clock; d->seq_next += NOTE_CLOCKS) {
        int fnum = tune[d->seq_step++ % (int)(sizeof tune / sizeof tune[0])];
        demo_reg(d, d->seq_next, 0x28, 0x00);               /* FM channel 1: key off */
        if (fnum) {
            demo_reg(d, d->seq_next, 0xA4, 0x20 | fnum >> 8);   /* block and F-number */
            demo_reg(d, d->seq_next, 0xA0, fnum & 0xFF);
            demo_reg(d, d->seq_next, 0x28, 0xF0);           /* key on; the queue's bus spacing
                                                             * keeps it apart from the key off */
        }
    }
    if (d->blip) {                                          /* SSG channel A: the period, */
        demo_reg(d, clock, 0x00, d->blip & 0xFF);
        demo_reg(d, clock, 0x01, d->blip >> 8);
        demo_reg(d, clock, 0x0D, 0x00);                     /* and its envelope decays anew */
        d->blip = 0;
    }
}

/* The game asks for a beep. The sequencer may be running on the audio thread right now: every
 * call into what it reads is bracketed with the mixer's lock (a no-op where audio is pushed). */
static void demo_beep(struct demo *d, int hz)
{
    lrv_mixer_lock(&d->mixer);
    d->blip = (int)(LRV_OPN_CLOCK_PC98 / 64) / hz;          /* the SSG's tone period */
    lrv_mixer_unlock(&d->mixer);
}

/* The chip as the driver expects to find it, and the tune from its start. */
static void demo_sound_reset(struct demo *d)
{
    static const uint8_t patch[][2] = {     /* channel 1: one audible operator with feedback */
        { 0x30, 0x01 }, { 0x40, 0x18 }, { 0x50, 0x1F }, { 0x60, 0x0A }, { 0x80, 0x27 },
        { 0x44, 0x7F }, { 0x48, 0x7F }, { 0x4C, 0x7F }, { 0xB0, 0x2F },
        { 0x07, 0x3E }, { 0x08, 0x10 },     /* SSG: tone A alone, its level the envelope's, */
        { 0x0B, 0x49 }, { 0x0C, 0x02 }      /* which takes 150 ms */
    };
    size_t i;
    lrv_mixer_lock(&d->mixer);
    lrv_opnq_reset(&d->opnq);
    for (i = 0; i < sizeof patch / sizeof patch[0]; i++)
        lrv_opnq_poke(&d->opnq, patch[i][0], patch[i][1]);  /* at once, past the queue */
    d->seq_next = 0;
    d->seq_step = d->blip = 0;
    lrv_mixer_unlock(&d->mixer);
}

static int demo_chip(struct demo *d, const struct lrv_opn_ops *ops, uint32_t rate)
{
    struct lrv_opnq_cfg qc;
    memset(&qc, 0, sizeof qc);
    qc.ops = ops;
    qc.clock_hz = LRV_OPN_CLOCK_PC98;
    qc.rate = rate;
    qc.bus_clocks = LRV_OPN_FM_DIV;         /* writes reach the chip one FM sample apart */
    qc.queue_size = 64;
    qc.seq.ctx = d;
    qc.seq.run_until = demo_seq_run;
    return lrv_opnq_init(&d->opnq, d->hal, &qc);
}

/* The sound path: sequencer -> write queue -> chip core -> mixer -> the HAL's stream. A core
 * may insist on a rate of its own (ymfm renders at clock / 72), so the chip is created first
 * and the device asked for the chip's rate; a device that insists on another gets a core that
 * renders at any. Returns the rate the mix is rendered at, 0 without a chip. */
static uint32_t demo_sound_open(struct demo *d, const struct lrv_run_cfg *cfg, int device)
{
    struct lrv_mixer_cfg mc;
    uint32_t rate;
    int got;

    memset(&mc, 0, sizeof mc);
    mc.render = lrv_opnq_render;
    mc.skip = lrv_opnq_skip;                /* for a backend that falls behind */
    mc.ctx = &d->opnq;
    lrv_mixer_init(&d->mixer, d->hal, &mc);
    if (demo_chip(d, lrv_opn_select(cfg->opn_core), (uint32_t)cfg->audio_rate) != LRV_OK)
        lrv_fatal(d->hal, "demo: cannot create the sound chip");
    rate = lrv_opnq_rate(&d->opnq);
    got = device && rate ? lrv_mixer_open(&d->mixer, (int)rate) : 0;
    if (got && (uint32_t)got != rate &&
        lrv_opnq_set_rate(&d->opnq, (uint32_t)got) != (uint32_t)got) {
        lrv_opnq_free(&d->opnq);            /* nothing renders before lrv_mixer_start */
        if (demo_chip(d, &lrv_opn_lite_ops, (uint32_t)got) != LRV_OK)
            lrv_fatal(d->hal, "demo: cannot create the sound chip");
    }
    rate = lrv_opnq_rate(&d->opnq);
    lrv_mixer_set_volume(&d->mixer, cfg->volume < 0 ? 0 : cfg->volume ? cfg->volume : 100);
    demo_sound_reset(d);
    lrv_mixer_start(&d->mixer, rate);
    return rate;
}

static void demo_sound_close(struct demo *d)
{
    lrv_mixer_close(&d->mixer);             /* first: the sources are not called after it */
    lrv_opnq_free(&d->opnq);
}

/* ---- one frame ---------------------------------------------------------------------------- */

static void demo_cell(struct demo *d, int index, int lit)
{
    int x = index % COLS, y = CELLS_Y + 8 * (index / COLS);
    if (d->cells[index] == lit)
        return;
    d->cells[index] = (uint8_t)lit;
    demo_fill(d, x, y, x + 1, y + 6, lit ? 15 : 8);
}

/* A key press: the host-neutral key a backend delivered, as the PC-98 would have reported it. */
static void demo_key(struct demo *d, const struct lrv_keypress *k)
{
    struct lrv_pc98_key pk = { LRV_PC98_K_NONE, 0 };
    char line[80];

    lrv_pc98_keypress(k, 0, &pk);
    lrv_sys_trace(&d->sys, DEMO_TR_KEY, lrv_key_name(k->key), 0, k->key, k->chr, k->mods, pk.scan);
    snprintf(line, sizeof line, "key %-10s PC-98 scan %02X char %02X ", lrv_key_name(k->key),
             pk.scan, pk.chr);
    demo_text(d, 1, TEXT_Y + 16, line, 14);
    demo_beep(d, 440 + 20 * k->key);
}

/* Returns 0 when the player quit. */
static int demo_step(struct demo *d, int speed)
{
    struct lrv_sys *s = &d->sys;
    struct lrv_keypress k;
    uint8_t map[LRV_PC98_KEYMAP_BYTES], dac[sizeof d->palette];
    int i, running = 1;

    while (lrv_sys_key_read(s, &k)) {
        demo_key(d, &k);
        if (k.key == LRV_K_SPACE)
            d->paused = !d->paused;
        else if (k.key == LRV_K_P)
            d->palette_stopped = !d->palette_stopped;
        else if (k.key == LRV_K_TAB)                /* the other page; drawing stays on 0 */
            lrv_screen_set_display(&d->screen, !d->screen.display_page);
        else if (k.key == LRV_K_ESC)
            running = 0;
    }
    if (lrv_sys_mouse_pressed(s, LRV_EV_MOUSE_L | LRV_EV_MOUSE_R))
        demo_beep(d, 880);

    if (!d->paused) {
        demo_fill(d, d->block_col, BLOCK_Y, d->block_col + BLOCK_COLS, BLOCK_Y + BLOCK_H, 0);
        d->block_col = (d->block_col + speed) % (COLS - BLOCK_COLS + 1);
        demo_fill(d, d->block_col, BLOCK_Y, d->block_col + BLOCK_COLS, BLOCK_Y + BLOCK_H, 15);
    }
    lrv_pc98_keymap(&s->in, 0, map);                /* the keyboard as the machine shows it */
    for (i = 0; i < 128; i++)
        demo_cell(d, i, lrv_pc98_key_down(map, i));
    demo_cell(d, 128, (lrv_sys_mouse_buttons(s) & LRV_BTN_L) != 0);
    demo_cell(d, 129, (lrv_sys_mouse_buttons(s) & LRV_BTN_R) != 0);

    if (!d->palette_stopped && s->frame % 8 == 0) { /* rotate colours 1..15: no redraw needed */
        uint8_t first[3];
        memcpy(first, d->palette + 3, 3);
        memmove(d->palette + 3, d->palette + 6, 14 * 3);
        memcpy(d->palette + 15 * 3, first, 3);
    }
    if (d->fade < FADE_STEPS)                       /* fade in from black after a (re)start */
        d->fade++;
    lrv_pal_brightness(dac, d->palette, sizeof dac, d->fade, FADE_STEPS);
    lrv_screen_set_palette(&d->screen, dac);
    return running;
}

/* ---- the game ----------------------------------------------------------------------------- */

/* The pack, the profile and the fonts. The defaults are set first: a key the profile does not
 * have leaves its field alone. */
static void demo_load(struct demo *d, const struct lrv_run_cfg *cfg)
{
    uint32_t size;
    void *probe = d->hal->pack_open(LRV_PACK_MAIN, &size);

    strcpy(d->profile.base.id, "demo");
    strcpy(d->profile.base.title, "lrvne demo");
    d->profile.speed = 1;
    if (!probe) {
        lrv_log(d->hal, LRV_LOG_INFO, "demo: no pack, using the built-in profile");
        return;
    }
    d->hal->pack_close(probe);
    if (lrv_pack_open(&d->pack, d->hal, LRV_PACK_MAIN) != LRV_OK)
        return;
    d->have_pack = 1;
    lrv_profile_load(&d->pack, demo_fields, (int)(sizeof demo_fields / sizeof demo_fields[0]),
                     &d->profile);
    /* --lang, else the profile's; fonts and text are then looked up in that language first */
    lrv_pack_set_lang(&d->pack, lrv_pack_lang_id(&d->pack, cfg->lang ? cfg->lang
                                                                      : d->profile.base.lang));
    lrv_font_load(&d->latin8, &d->pack, "latin8", 8, LRV_GLYPH_ROWS);
    lrv_font_load(&d->wide16, &d->pack, "wide16", 16, LRV_GLYPH_ROWS);
}

/* counts the runs in a storage record of the game's own store */
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
    int speed;

    d->hal = hal;
    demo_load(d, cfg);
    speed = d->speed_option ? d->speed_option : d->profile.speed;

    memset(&video, 0, sizeof video);
    video.mode = lrv_pc98_video_mode;
    video.title = d->profile.base.title;
    video.fullscreen = cfg->fullscreen;
    video.scale_mode = cfg->scale_mode;
    video.host_mode = cfg->host_mode;
    if (hal->video_open(&video) != LRV_OK)
        lrv_fatal(hal, "demo: cannot open the display");
    /* two pages where the display hardware reaches them, no off-screen buffers */
    if (lrv_screen_init(&d->screen, hal, &video.mode, 2, 0, LRV_MEM_CHIP) != LRV_OK)
        lrv_fatal(hal, "demo: no memory for the screen");
    lrv_screen_add_overlay(&d->screen, demo_pointer_apply, demo_pointer_restore, d);

    /* the frame: pump the audio, present the screen, wait, poll the input */
    lrv_sys_init(&d->sys, hal, video.mode.w, video.mode.h);
    lrv_sys_set_present(&d->sys, lrv_screen_present_hook, &d->screen);
    demo_sound_open(d, cfg, 1);
    lrv_sys_add_hook(&d->sys, LRV_HOOK_PUMP, lrv_mixer_pump, &d->mixer);
    demo_count_run(d);

    for (;;) {                                      /* a reset comes back here */
        demo_draw_screen(d);
        demo_sound_reset(d);
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
    demo_sound_close(d);
    hal->video_close();
    lrv_screen_free(&d->screen);
    lrv_font_free(&d->latin8);
    lrv_font_free(&d->wide16);
    if (d->have_pack)
        lrv_pack_close(&d->pack);
    return 0;
}

/* Offline rendering (a backend's --render-wav): the one song, "tune", through the same path
 * without a device. The mixer is then only a renderer. */
static int demo_render_music(void *ctx, const struct lrv_hal *hal, const struct lrv_run_cfg *cfg,
                             const char *name, double seconds, uint32_t *rate,
                             void (*sink)(void *sctx, const int16_t *pcm, int frames), void *sctx)
{
    struct demo *d = ctx;
    int16_t buf[512];
    long left;

    if (strcmp(name, "tune") || cfg->audio_rate <= 0)
        return LRV_ERR_NOTFOUND;
    d->hal = hal;
    *rate = demo_sound_open(d, cfg, 0);
    for (left = (long)(seconds * *rate); left > 0; left -= 512) {
        int n = left > 512 ? 512 : (int)left;
        lrv_mixer_render(&d->mixer, buf, n);
        sink(sctx, buf, n);
    }
    demo_sound_close(d);
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
    .help = "keys: Space pauses the block, P stops the palette, Tab shows the other page,\n"
            "  Esc quits; --render-wav plays the song \"tune\"",
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
