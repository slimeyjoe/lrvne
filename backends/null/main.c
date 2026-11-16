/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Null backend: lrv_app_main for a headless run of an engine, for tests and automation.
 *
 * No display, no audio device, no real time: frame_wait returns at once, so an engine runs as
 * fast as the host computes it and every run of the same input is the same, tick for tick.
 *
 * Input comes from the autoplay spec (autoplay.h; default "ok:30", a press of return every
 * 30 ticks), or from the driver in control mode (control.h). The run ends when the engine
 * quits, or after --max-ticks frames (default 200000): the engine is then asked to quit at
 * once (an emergency quit, as a machine's backend does at its tick limit), leaves through its
 * own shutdown, and the exit code is 3.
 *
 * What a run can leave behind:
 *   snapshots     the frames at the spec's snap: ticks, as lines on stdout (shot.h)
 *   pictures      --shot-every N writes every N-th frame as <--shot-dir>/shot_<tick>.ppm
 *   sound         --wav FILE records what the engine plays, taken at the pace of the video
 *                 mode's frames: pushed by the engine, or with --audio pull asked for by the
 *                 backend as a device with a callback does (the audio lock then only checks
 *                 the contract: taking it twice is fatal); --render-wav FILE --song NAME [--seconds S] plays one song
 *                 offline through the engine's render_music and exits
 *   memory        --memstat prints the peak of the engine's allocations on stderr
 * --present-test runs the synthetic presentation test (ptest.h) in the video mode the engine
 * opens, prints its snapshots and exits: the reference for backends with a real display.
 * --present-test-mode MACHINE runs it in that machine's mode (lrvne/machine.h) instead,
 * without starting the engine, so it needs no game data.
 *
 * Differences from a player's backend, all for the sake of repeatable tests: the saves are
 * kept in the current directory unless --storage names another (never in the user's data
 * directory), and a voice pack is used only when --voice-pak names it. With one, the engine
 * mixes the clips into its own stream (LRV_AUX_MIX); --voice-out channel is a pretend
 * LRV_AUX_CHANNEL device that plays what is queued at the pace of the frames, holding back a
 * few frames until the end of a clip as a resampling device does, and --voice-out stuck one
 * that never plays anything (an engine must not wait for it for ever). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lrvne/app.h"
#include "lrvne/core.h"
#include "lrvne/machine.h"
#include "args.h"
#include "autoplay.h"
#include "control.h"
#include "paths.h"
#include "ptest.h"
#include "shot.h"

#define DEFAULT_MAX_TICKS 200000L
#define DEFAULT_RATE      44100
#define DEFAULT_FRAME_NS  16666667u     /* before an engine opened its video mode */
#define AUX_HELD_BACK     7             /* frames a resampler keeps until the clip ends */
#define AUX_QUEUE_MAX     65536L

enum { AUX_OUT_MIX = 0, AUX_OUT_CHANNEL, AUX_OUT_STUCK };

static const struct lrv_app *g_app;
static struct lrv_args g_args;
static int g_control;
static long g_max_ticks = DEFAULT_MAX_TICKS;
static uint32_t g_tick;
static int g_out_of_ticks;              /* --max-ticks ran out: the next poll asks for a quit */
static struct lrv_video_mode g_mode;
static struct lrv_autoplay g_ap;
static struct lrv_shot g_snap;          /* the last frame, when the spec takes snapshots */

/* the backend's options */
static int g_shot_every;
static const char *g_shot_dir = ".";
static int g_trace;
static int g_present_test, g_present_ran;
static const char *g_present_mode;      /* --present-test-mode: the machine, without the engine */
static const char *g_render, *g_song;
static double g_seconds = 30.0;
static const char *g_game_wav;
static int g_aux_out;
static int g_audio_pull;
static int g_memstat;

static void l_fatal(const char *msg);

static uint32_t frame_ns(void)
{
    return g_mode.frame_ns ? g_mode.frame_ns : DEFAULT_FRAME_NS;
}

/* frames of sound at `rate` that one video frame lasts; *acc carries the remainder */
static long frames_per_tick(int rate, uint64_t *acc)
{
    long n;
    *acc += (uint64_t)rate * frame_ns();
    n = (long)(*acc / 1000000000u);
    *acc %= 1000000000u;
    return n;
}

/* ---- video -------------------------------------------------------------------------------- */

static void pt_present(const struct lrv_frame *frame)
{
    (void)frame;
}

static void pt_snap(int index, const char *name, const struct lrv_frame *frame, uint32_t hash)
{
    (void)hash;
    lrv_shot_emit_snap(stdout, frame, name, (uint32_t)index);
}

static int v_open(const struct lrv_video_cfg *cfg)
{
    if (cfg->mode.format != LRV_FMT_PLANAR4 || cfg->mode.w <= 0 || cfg->mode.h <= 0) {
        fprintf(stderr, "%s: video mode not supported by this backend\n", g_app->name);
        return LRV_ERR_RANGE;
    }
    g_mode = cfg->mode;
    lrv_ctl_video(&g_mode);
    if (g_present_test) {
        static const struct lrv_ptest_io io = { pt_present, pt_snap };
        g_present_ran = 1;
        exit(lrv_ptest_run(&g_mode, &io) > 0 ? 0 : 1);
    }
    return LRV_OK;
}

static void v_close(void)
{
}

static void v_present(const struct lrv_frame *frame)
{
    if (g_control)
        lrv_ctl_present(frame);
    else if (g_ap.snap_count || g_ap.snap_wait)
        lrv_shot_capture(&g_snap, frame);
    if (g_shot_every && g_tick % (uint32_t)g_shot_every == 0) {
        char path[LRV_PATH_MAX];
        snprintf(path, sizeof path, "%s/shot_%08lu.ppm", g_shot_dir, (unsigned long)g_tick);
        lrv_shot_write_ppm(path, frame);
    }
}

/* ---- the pretend auxiliary channel (--voice-out channel|stuck) ---------------------------- */

static long g_aux_queued;               /* frames a device would still have to play */
static long g_aux_held;                 /* of the last push, held back until the clip ends */
static int g_aux_rate;
static uint64_t g_aux_acc;

static int aux_open(int rate)
{
    g_aux_rate = rate;
    g_aux_queued = g_aux_held = 0;
    return rate;
}

static void aux_close(void)
{
    g_aux_queued = g_aux_held = 0;
}

static int aux_space(void)
{
    return g_aux_queued < AUX_QUEUE_MAX ? (int)(AUX_QUEUE_MAX - g_aux_queued) : 0;
}

static void aux_push(const int16_t *pcm, int frames)
{
    (void)pcm;
    if (frames > 0) {
        g_aux_queued += g_aux_held + frames - AUX_HELD_BACK;
        g_aux_held = AUX_HELD_BACK;
        if (g_aux_queued < 0) {
            g_aux_held += g_aux_queued;
            g_aux_queued = 0;
        }
    } else {
        g_aux_queued += g_aux_held;     /* end of the clip */
        g_aux_held = 0;
    }
}

static int aux_queued(void)
{
    return (int)(g_aux_queued + g_aux_held);
}

static void aux_flush(void)
{
    g_aux_queued = g_aux_held = 0;
}

static void aux_tick(void)
{
    if (g_aux_out != AUX_OUT_CHANNEL || !g_aux_rate)
        return;
    g_aux_queued -= frames_per_tick(g_aux_rate, &g_aux_acc);
    if (g_aux_queued < 0)
        g_aux_queued = 0;
}

/* ---- audio: into --wav, or nowhere -------------------------------------------------------- */

static struct lrv_wav g_wav;
static int g_audio_rate;
static uint64_t g_audio_acc;
static long g_audio_owed;               /* frames the pretend device takes now */
static struct lrv_audio_source g_audio_src;     /* --audio pull */
static int g_audio_locked;

static int a_open(int rate, const struct lrv_audio_source *src)
{
    if (!g_game_wav || rate <= 0)
        return 0;
    g_audio_src = *src;
    if (lrv_wav_open(&g_wav, g_game_wav) != LRV_OK) {
        perror(g_game_wav);
        return 0;
    }
    g_audio_rate = rate;
    g_audio_owed = 0;
    return rate;
}

static void a_close(void)
{
    if (!g_audio_rate)
        return;
    fprintf(stderr, "%s: %lu frames at %d Hz\n", g_game_wav, (unsigned long)g_wav.frames,
            g_audio_rate);
    lrv_wav_close(&g_wav, (uint32_t)g_audio_rate);
    g_audio_rate = 0;
}

static int a_space(void)
{
    return g_audio_owed > 0 ? (int)g_audio_owed : 0;
}

static void a_push(const int16_t *pcm, int frames)
{
    if (g_audio_rate) {
        uint32_t before = g_wav.frames / (uint32_t)g_audio_rate;
        lrv_wav_write(&g_wav, pcm, frames);
        /* the header again about every second: a driver may kill the process, and the file
         * is whole up to there */
        if (g_wav.frames / (uint32_t)g_audio_rate != before)
            lrv_wav_sync(&g_wav, (uint32_t)g_audio_rate);
    }
    g_audio_owed -= frames;
}

/* --audio pull: the device's callback, at the end of a frame and without the lock */
static void a_pull(void)
{
    int16_t pcm[1024];
    while (g_audio_owed > 0) {
        int n = g_audio_owed > 1024 ? 1024 : (int)g_audio_owed;
        g_audio_src.render(g_audio_src.ctx, pcm, n);
        a_push(pcm, n);
    }
}

/* nothing runs beside the engine here, so the lock only holds the engine to the contract */
static void a_lock(void)
{
    if (g_audio_locked++)
        l_fatal("the audio lock was taken twice");
}

static void a_unlock(void)
{
    g_audio_locked--;
}

/* ---- time and input ----------------------------------------------------------------------- */

static void t_frame_wait(void)
{
    g_tick++;
    aux_tick();
    if (g_audio_rate) {
        g_audio_owed += frames_per_tick(g_audio_rate, &g_audio_acc);
        if (g_audio_pull)
            a_pull();
    }
    if ((long)g_tick > g_max_ticks) {
        /* the engine leaves through its own shutdown, as on the machines (an emergency quit
         * at the next poll; a frame no longer waits once it is seen); exit code 3 */
        if (!g_out_of_ticks)
            fprintf(stderr, "%s: max ticks reached\n", g_app->name);
        g_out_of_ticks = 1;
        return;
    }
    if (g_control) {
        lrv_ctl_tick(g_tick);
        return;
    }
    lrv_autoplay_end_tick(&g_ap, g_tick);
    if (g_snap.valid && lrv_autoplay_snap_due(&g_ap, g_tick)) {
        char name[24];
        lrv_autoplay_snap_name(&g_ap, g_tick, name, sizeof name);
        lrv_shot_emit_snap(stdout, &g_snap.frame, name, g_tick);
    }
}

/* the engine's input waits, for the autoplay words driven by them */
static void autoplay_trace(const struct lrv_trace_event *ev)
{
    if (ev->kind == LRV_TR_WAIT && (!g_app->wait_is_input || g_app->wait_is_input(ev->a0)))
        lrv_autoplay_wait(&g_ap, ev->a1 == 0);
}

static void i_poll(struct lrv_input *in)
{
    if (g_control) {
        lrv_ctl_input(in);
    } else {
        memset(in, 0, sizeof *in);
        lrv_autoplay_poll(&g_ap, g_tick, in);
    }
    if (g_out_of_ticks)
        in->events |= LRV_EV_EMERGENCY;
}

static void i_flush(void)
{
    if (g_control)
        lrv_ctl_input_flush();
    else
        lrv_autoplay_flush(&g_ap, g_tick);
}

/* ---- memory and diagnostics --------------------------------------------------------------- */

static size_t g_mem_now, g_mem_peak, g_mem_largest;
static uint32_t g_mem_peak_tick;

static void *m_alloc(size_t n, unsigned flags)
{
    void *p = malloc(n);
    (void)flags;
    if (p) {
        g_mem_now += n;
        if (n > g_mem_largest)
            g_mem_largest = n;
        if (g_mem_now > g_mem_peak) {
            g_mem_peak = g_mem_now;
            g_mem_peak_tick = g_tick;
        }
    }
    return p;
}

static void m_free(void *p, size_t n)
{
    if (p)
        g_mem_now -= n;
    free(p);
}

static void mem_report(void)
{
    fprintf(stderr, "memory: peak %lu bytes at frame %lu, largest block %lu, %lu still allocated\n",
            (unsigned long)g_mem_peak, (unsigned long)g_mem_peak_tick,
            (unsigned long)g_mem_largest, (unsigned long)g_mem_now);
}

static void l_log(int level, const char *msg)
{
    if (g_trace || level >= LRV_LOG_WARN)
        fprintf(stderr, "[%d] %s\n", level, msg);
}

static void l_fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    if (g_control)
        lrv_ctl_exit(1, msg);
    exit(1);
}

/* ---- the command line --------------------------------------------------------------------- */

static const struct lrv_option g_options[] = {
    { "shot-every", LRV_OPT_ARG, "N", "write every N-th frame as a PPM picture" },
    { "shot-dir", LRV_OPT_ARG, "DIR", "where the pictures go (default: here)" },
    { "present-test", LRV_OPT_FLAG, 0, "print the snapshots of the presentation test and exit" },
    { "present-test-mode", LRV_OPT_ARG, "MACHINE",
      "the same in that machine's video mode, without the game (pc98)" },
    { "wav", LRV_OPT_ARG, "FILE", "record what the engine plays" },
    { "audio", LRV_OPT_ARG, "push|pull", "how the pretend device gets the sound (default push)" },
    { "render-wav", LRV_OPT_ARG, "FILE", "play --song offline into FILE and exit" },
    { "song", LRV_OPT_ARG, "NAME", "the song of --render-wav" },
    { "seconds", LRV_OPT_ARG, "S", "how long --render-wav plays (default 30)" },
    { "voice-out", LRV_OPT_ARG, "mix|channel|stuck", "how the pretend device takes the voice" },
    { "trace", LRV_OPT_FLAG, 0, "log everything the engine logs, not only warnings" },
    { "memstat", LRV_OPT_FLAG, 0, "print the peak of the engine's allocations at exit" }
};

static int option(struct lrv_args *a, const char *name, const char *arg)
{
    long v;
    (void)a;
    if (!strcmp(name, "shot-every")) {
        if (!lrv_args_int(arg, 1, 0x7fffffffL, &v))
            return 1;
        g_shot_every = (int)v;
    } else if (!strcmp(name, "shot-dir")) {
        g_shot_dir = arg;
    } else if (!strcmp(name, "present-test")) {
        g_present_test = 1;
    } else if (!strcmp(name, "present-test-mode")) {
        if (!lrv_video_mode_by_name(arg))
            return 1;
        g_present_test = 1;
        g_present_mode = arg;
    } else if (!strcmp(name, "wav")) {
        g_game_wav = arg;
    } else if (!strcmp(name, "audio")) {
        if (strcmp(arg, "push") && strcmp(arg, "pull"))
            return 1;
        g_audio_pull = !strcmp(arg, "pull");
    } else if (!strcmp(name, "render-wav")) {
        g_render = arg;
    } else if (!strcmp(name, "song")) {
        g_song = arg;
    } else if (!strcmp(name, "seconds")) {
        char *end;
        g_seconds = strtod(arg, &end);
        if (end == arg || *end || g_seconds <= 0)
            return 1;
    } else if (!strcmp(name, "voice-out")) {
        if (!strcmp(arg, "mix"))
            g_aux_out = AUX_OUT_MIX;
        else if (!strcmp(arg, "channel"))
            g_aux_out = AUX_OUT_CHANNEL;
        else if (!strcmp(arg, "stuck"))
            g_aux_out = AUX_OUT_STUCK;
        else
            return 1;
    } else if (!strcmp(name, "trace")) {
        g_trace = 1;
    } else if (!strcmp(name, "memstat")) {
        g_memstat = 1;
    } else {
        return 1;
    }
    return 0;
}

static const struct lrv_args_backend g_backend = {
    g_options, (int)(sizeof g_options / sizeof g_options[0]), option, 0,
    "headless: no window, no sound device, no real time; input from --autoplay\n"
    "(default ok:30) or --control; the saves go to the current directory"
};

int lrv_app_main(const struct lrv_app *app, int argc, char **argv)
{
    struct lrv_paths_cfg paths;
    struct lrv_hal hal;
    char err[96];
    int rc;

    g_app = app;
    rc = lrv_args_parse(&g_args, app, &g_backend, argc, argv);
    if (rc != LRV_ARGS_RUN)
        return rc == LRV_ARGS_DONE ? 0 : 2;
    g_control = g_args.control;
    if (g_control && !lrv_ctl_available()) {
        fprintf(stderr, "%s: built without the control protocol\n", app->name);
        return 2;
    }
    if (g_args.max_ticks)
        g_max_ticks = g_args.max_ticks;
    if (lrv_autoplay_parse(&g_ap, g_args.autoplay ? g_args.autoplay : "ok:30", err,
                           sizeof err) != LRV_OK) {
        fprintf(stderr, "%s: %s\n", app->name, err);
        return 2;
    }

    memset(&paths, 0, sizeof paths);
    paths.app_name = app->name;
    paths.argv0 = argc > 0 ? argv[0] : 0;
    paths.pak = g_args.pak;
    paths.voice_pak = g_args.voice_pak;
    paths.no_voice = g_args.no_voice;
    paths.voice_explicit = 1;
    paths.storage = g_args.storage;
    paths.storage_default = ".";
    lrv_paths_init(&paths);

    memset(&hal, 0, sizeof hal);
    hal.video_open = v_open;
    hal.video_close = v_close;
    hal.video_present = v_present;
    hal.frame_wait = t_frame_wait;
    hal.input_poll = i_poll;
    hal.input_flush = i_flush;
    hal.audio_open = a_open;
    hal.audio_close = a_close;
    if (g_audio_pull) {
        hal.audio_lock = a_lock;
        hal.audio_unlock = a_unlock;
    } else {
        hal.audio_space = a_space;
        hal.audio_push = a_push;
    }
    hal.storage_open = lrv_paths_storage_open;
    hal.storage_read = lrv_paths_storage_read;
    hal.storage_write = lrv_paths_storage_write;
    hal.pack_open = lrv_paths_pack_open;
    hal.pack_read = lrv_paths_pack_read;
    hal.pack_close = lrv_paths_pack_close;
    hal.mem_alloc = m_alloc;
    hal.mem_free = m_free;
    hal.log = l_log;
    hal.fatal = l_fatal;
    if (g_args.voice_pak && !g_args.no_voice) {
        hal.aux_mode = LRV_AUX_MIX;     /* into the engine's stream: silent without --wav */
        if (g_aux_out != AUX_OUT_MIX) {
            hal.aux_mode = LRV_AUX_CHANNEL;
            hal.aux_open = aux_open;
            hal.aux_close = aux_close;
            hal.aux_space = aux_space;
            hal.aux_push = aux_push;
            hal.aux_queued = aux_queued;
            hal.aux_flush = aux_flush;
        }
    }
    if (g_control) {
        lrv_ctl_init(app);
        hal.trace = lrv_ctl_trace;
    } else if (lrv_autoplay_uses_waits(&g_ap)) {
        hal.trace = autoplay_trace;
    }
    if (g_memstat)
        atexit(mem_report);

    if (g_render) {
        if (!g_args.cfg.audio_rate)
            g_args.cfg.audio_rate = DEFAULT_RATE;
        return lrv_wav_render(app, &hal, &g_args.cfg, g_render, g_song, g_seconds);
    }
    if (g_present_mode) {
        struct lrv_video_cfg vc;
        lrv_ptest_video_cfg(&vc, g_present_mode, &g_args.cfg, app->name);
        v_open(&vc);                    /* runs the test and exits, unless the mode is refused */
        return 1;
    }
    if (g_game_wav && !g_args.cfg.audio_rate)
        g_args.cfg.audio_rate = DEFAULT_RATE;

    rc = app->run(app->ctx, &hal, &g_args.cfg);
    if (g_out_of_ticks)
        rc = 3;

    lrv_autoplay_free(&g_ap);
    lrv_shot_free(&g_snap);
    if (g_present_test && !g_present_ran) {
        fprintf(stderr, "%s: --present-test: the engine opened no video mode\n", app->name);
        rc = 1;
    }
    if (g_control)
        lrv_ctl_exit(rc, g_out_of_ticks ? "max ticks reached" : "engine returned");
    return rc;
}
