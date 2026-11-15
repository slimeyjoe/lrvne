/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDL3 backend: lrv_app_main for a player's build on Linux, macOS and other SDL3 hosts.
 *
 * This file parses the command line, assembles the HAL from video.c, input.c, audio.c and the
 * pack and storage code shared with the headless backend (backends/common/paths.h), paces the
 * frames and runs the engine.
 *
 * Frames are paced by the video mode's own refresh (mode.frame_ns) with absolute deadlines, so
 * the jitter of a sleep does not add up; --tick vsync takes the display's refresh instead.
 *
 * The voice pack beside the game pack is used when it is there (--voice-pak names another,
 * --no-voice turns it off); the clips play on a second stream of the music's device.
 *
 * --control is the test automation protocol (backends/control/control.h): the engine runs as
 * fast as the driver lets it, with no audio, its input from stdin and its events on stdout,
 * and the window is rendered offscreen (SDL's "offscreen" video driver unless the environment
 * says otherwise). The protocol's `readback` returns the pixels the renderer produced, so this
 * presentation path can be checked against the engine's pages.
 *
 * --render-wav FILE --song NAME [--seconds S] plays one song offline through the engine's
 * render_music and exits without opening a window. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lrvne/core.h"
#include "args.h"
#include "paths.h"
#include "shot.h"
#include "sdl3.h"

#define DEFAULT_RATE     44100
#define DEFAULT_FRAME_NS 16666667u      /* before an engine opened its video mode */

struct lrv_sdl3 lrv_sdl3;

static struct lrv_args g_args;
static uint64_t g_deadline;             /* when the current frame ends, SDL_GetTicksNS time */
static const char *g_render, *g_song;
static double g_seconds = 30.0;

/* ---- time --------------------------------------------------------------------------------- */

static void frame_wait(void)
{
    uint64_t now, frame_ns = lrv_sdl3.mode.frame_ns ? lrv_sdl3.mode.frame_ns : DEFAULT_FRAME_NS;

    lrv_sdl3.frames++;
    if (g_args.max_ticks && (long)lrv_sdl3.frames > g_args.max_ticks) {
        if (lrv_sdl3.control)
            lrv_ctl_exit(3, "max ticks reached");
        fprintf(stderr, "%s: max ticks reached\n", lrv_sdl3.app->name);
        exit(3);
    }
    if (lrv_sdl3.control) {             /* no real time: the driver paces the engine */
        lrv_ctl_tick(lrv_sdl3.frames);
        return;
    }
    if (lrv_sdl3.vsync && lrv_sdl3.mode.w)
        return;                         /* the present already waited for the host's refresh */
    now = SDL_GetTicksNS();
    if (g_deadline == 0 || now > g_deadline + 10 * frame_ns)
        g_deadline = now;               /* (re)synchronise after a stall */
    g_deadline += frame_ns;
    if (g_deadline > now)
        SDL_DelayNS(g_deadline - now);
}

/* ---- memory and diagnostics --------------------------------------------------------------- */

static void *mem_alloc(size_t n, unsigned flags)
{
    (void)flags;
    return malloc(n);
}

static void mem_free(void *p, size_t n)
{
    (void)n;
    free(p);
}

static void log_line(int level, const char *msg)
{
    if (level >= LRV_LOG_INFO)
        fprintf(stderr, "%s\n", msg);
}

static void fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    if (lrv_sdl3.control)
        lrv_ctl_exit(1, msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, lrv_sdl3.app->name, msg, lrv_sdl3_window());
    exit(1);
}

/* ---- the command line --------------------------------------------------------------------- */

static const struct lrv_option g_options[] = {
    { "fullscreen", LRV_OPT_FLAG, 0, "start in fullscreen (F11 or Alt+Return toggles)" },
    { "window", LRV_OPT_ARG, "WxH", "the window's size (default: twice the game's screen)" },
    { "scale", LRV_OPT_ARG, "MODE",
      "integer (default), aspect (whole factors near the shape of the\n"
      "original's pixels), fit (the original's shape, smooth), stretch;\n"
      "F12 cycles" },
    { "tick", LRV_OPT_ARG, "fixed|vsync",
      "pace the frames by the original machine's refresh (default) or\n"
      "by the display's" },
    { "render-wav", LRV_OPT_ARG, "FILE", "play --song offline into FILE and exit" },
    { "song", LRV_OPT_ARG, "NAME", "the song of --render-wav" },
    { "seconds", LRV_OPT_ARG, "S", "how long --render-wav plays (default 30)" }
};

static int option(struct lrv_args *a, const char *name, const char *arg)
{
    if (!strcmp(name, "fullscreen")) {
        a->cfg.fullscreen = 1;
    } else if (!strcmp(name, "window")) {
        if (sscanf(arg, "%dx%d", &lrv_sdl3.win_w, &lrv_sdl3.win_h) != 2 || lrv_sdl3.win_w < 1 ||
            lrv_sdl3.win_h < 1)
            return 1;
    } else if (!strcmp(name, "scale")) {
        if (!strcmp(arg, "integer"))
            a->cfg.scale_mode = LRV_SCALE_INTEGER;
        else if (!strcmp(arg, "aspect"))
            a->cfg.scale_mode = LRV_SCALE_INTEGER_ASPECT;
        else if (!strcmp(arg, "fit"))
            a->cfg.scale_mode = LRV_SCALE_FIT;
        else if (!strcmp(arg, "stretch"))
            a->cfg.scale_mode = LRV_SCALE_STRETCH;
        else
            return 1;
    } else if (!strcmp(name, "tick")) {
        if (strcmp(arg, "fixed") && strcmp(arg, "vsync"))
            return 1;
        lrv_sdl3.vsync = !strcmp(arg, "vsync");
    } else if (!strcmp(name, "render-wav")) {
        g_render = arg;
    } else if (!strcmp(name, "song")) {
        g_song = arg;
    } else if (!strcmp(name, "seconds")) {
        char *end;
        g_seconds = strtod(arg, &end);
        if (end == arg || *end || g_seconds <= 0)
            return 1;
    } else {
        return 1;
    }
    return 0;
}

static const struct lrv_args_backend g_backend = {
    g_options, (int)(sizeof g_options / sizeof g_options[0]), option, DEFAULT_RATE,
    "window keys: F11 or Alt+Return fullscreen, F12 the next scale mode,\n"
    "  Ctrl+Shift+R reset (the game starts again, the saves are kept),\n"
    "  Shift+Esc quit at once; the Kana key or Scroll Lock toggles kana input"
};

int lrv_app_main(const struct lrv_app *app, int argc, char **argv)
{
    struct lrv_paths_cfg paths;
    struct lrv_hal hal;
    char err[96];
    int rc;

    lrv_sdl3.app = app;
    rc = lrv_args_parse(&g_args, app, &g_backend, argc, argv);
    if (rc != LRV_ARGS_RUN)
        return rc == LRV_ARGS_DONE ? 0 : 2;
    lrv_sdl3.control = g_args.control;
    if (lrv_sdl3.control && !lrv_ctl_available()) {
        fprintf(stderr, "%s: built without the control protocol\n", app->name);
        return 2;
    }
    if (g_args.autoplay) {
        if (lrv_autoplay_parse(&lrv_sdl3.autoplay, g_args.autoplay, err, sizeof err) != LRV_OK) {
            fprintf(stderr, "%s: %s\n", app->name, err);
            return 2;
        }
        lrv_sdl3.have_autoplay = 1;
    }

    memset(&paths, 0, sizeof paths);
    paths.app_name = app->name;
    paths.argv0 = argc > 0 ? argv[0] : 0;
    paths.pak = g_args.pak;
    paths.voice_pak = g_args.voice_pak;
    paths.no_voice = g_args.no_voice;
    paths.storage = g_args.storage;
    lrv_paths_init(&paths);

    memset(&hal, 0, sizeof hal);
    hal.video_open = lrv_sdl3_video_open;
    hal.video_close = lrv_sdl3_video_close;
    hal.video_present = lrv_sdl3_video_present;
    hal.frame_wait = frame_wait;
    hal.input_poll = lrv_sdl3_input_poll;
    hal.input_flush = lrv_sdl3_input_flush;
    hal.mouse_warp = lrv_sdl3_mouse_warp;
    hal.audio_open = lrv_sdl3_audio_open;
    hal.audio_close = lrv_sdl3_audio_close;
    hal.audio_space = lrv_sdl3_audio_space;
    hal.audio_push = lrv_sdl3_audio_push;
    hal.audio_lock = lrv_sdl3_audio_lock;
    hal.audio_unlock = lrv_sdl3_audio_unlock;
    hal.storage_open = lrv_paths_storage_open;
    hal.storage_read = lrv_paths_storage_read;
    hal.storage_write = lrv_paths_storage_write;
    hal.pack_open = lrv_paths_pack_open;
    hal.pack_read = lrv_paths_pack_read;
    hal.pack_close = lrv_paths_pack_close;
    hal.mem_alloc = mem_alloc;
    hal.mem_free = mem_free;
    hal.log = log_line;
    hal.fatal = fatal;

    if (g_render)                       /* offline: neither a window nor a device */
        return lrv_wav_render(app, &hal, &g_args.cfg, g_render, g_song, g_seconds);

    if (lrv_sdl3.control) {
        /* headless: an offscreen window (the environment variable SDL_VIDEO_DRIVER still
         * wins), no audio device, the engine paced by the driver */
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
#ifdef __APPLE__
        /* the offscreen window has no Metal view: the software renderer into the driver's
         * plain framebuffer needs no GPU (the environment still wins here too) */
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
#endif
        g_args.cfg.audio_rate = 0;
        lrv_sdl3.vsync = 0;
        lrv_ctl_init(app);
        lrv_ctl_set_readback(lrv_sdl3_readback);
        hal.trace = lrv_ctl_trace;
    }
    if (g_args.cfg.audio_rate > 0) {    /* the voice needs the audio device */
        hal.aux_mode = LRV_AUX_CHANNEL;
        hal.aux_open = lrv_sdl3_aux_open;
        hal.aux_close = lrv_sdl3_aux_close;
        hal.aux_space = lrv_sdl3_aux_space;
        hal.aux_push = lrv_sdl3_aux_push;
        hal.aux_queued = lrv_sdl3_aux_queued;
        hal.aux_flush = lrv_sdl3_aux_flush;
        hal.set_volume = lrv_sdl3_set_volume;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "%s: SDL: %s\n", app->name, SDL_GetError());
        return 1;
    }

    rc = app->run(app->ctx, &hal, &g_args.cfg);

    lrv_sdl3_watchdog_arm();            /* SDL_Quit closes the audio subsystem too */
    SDL_Quit();
    lrv_autoplay_free(&lrv_sdl3.autoplay);
    if (lrv_sdl3.control)
        lrv_ctl_exit(rc, "engine returned");
    return rc;
}
