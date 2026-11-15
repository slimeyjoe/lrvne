/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests of what the backends share (backends/common): the command line parser, scripted
 * input, pack discovery and storage in a scratch directory, the presentation test and the
 * snapshot and WAV writers. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE        /* mkdtemp under strict POSIX flags */
#endif
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_hal.h"
#include "lrvne/app.h"
#include "lrvne/gfx.h"
#include "args.h"
#include "autoplay.h"
#include "jsonout.h"
#include "paths.h"
#include "ptest.h"
#include "shot.h"

/* error messages of the code under test are expected: keep them off the terminal */
static int quiet_fd = -1;

static void quiet(int on)
{
    fflush(stderr);
    if (on) {
        int null = open("/dev/null", O_WRONLY);
        quiet_fd = dup(2);
        dup2(null, 2);
        close(null);
    } else {
        dup2(quiet_fd, 2);
        close(quiet_fd);
    }
}

/* ---- args -------------------------------------------------------------------------------- */

static int opt_speed, opt_view_seen;
static const char *opt_view;

static int engine_option(void *ctx, const char *name, const char *arg)
{
    (void)ctx;
    if (!strcmp(name, "speed")) {
        opt_speed = atoi(arg);
        return opt_speed < 1;
    }
    opt_view_seen++;
    opt_view = arg;
    return 0;
}

static int backend_scale;

static int backend_option(struct lrv_args *a, const char *name, const char *arg)
{
    if (!strcmp(name, "fullscreen"))
        a->cfg.fullscreen = 1;
    else
        backend_scale = atoi(arg);
    return 0;
}

static const struct lrv_option engine_options[] = {
    { "speed", LRV_OPT_ARG, "N", "how fast" },
    { "view", LRV_OPT_OPTARG, "PICTURE", "browse the pictures\ninstead of playing" }
};
static const struct lrv_option backend_options[] = {
    { "fullscreen", LRV_OPT_FLAG, 0, "start in fullscreen" },
    { "scale", LRV_OPT_ARG, "N", "scale" }
};
static const struct lrv_app test_app = {
    .name = "engine", .version = "1.2", .options = engine_options, .noptions = 2,
    .option = engine_option, .help = "keys: none"
};
static const struct lrv_args_backend test_backend = {
    backend_options, 2, backend_option, 44100, "backend keys: none"
};

static int parse(struct lrv_args *a, int argc, ...)
{
    char *argv[16];
    va_list ap;
    int i;
    argv[0] = "engine";
    va_start(ap, argc);
    for (i = 1; i <= argc; i++)
        argv[i] = va_arg(ap, char *);
    va_end(ap);
    return lrv_args_parse(a, &test_app, &test_backend, argc + 1, argv);
}

static void test_args(void)
{
    struct lrv_args a;
    char text[4096];
    size_t n;
    FILE *f;

    /* nothing given: the defaults */
    CHECK(parse(&a, 0) == LRV_ARGS_RUN);
    CHECK(a.cfg.audio_rate == 44100 && a.cfg.volume == 0 && a.cfg.aux_volume == 0);
    CHECK(!a.pak && !a.cfg.lang && !a.storage && !a.autoplay && !a.control && a.max_ticks == 0);

    /* the common options, in both spellings */
    CHECK(parse(&a, 13, "--pak", "game.pak", "--lang=en", "--storage", "saves", "--volume", "40",
                "--voice-volume=70", "--rate", "22050", "--opn", "lite", "--no-voice")
          == LRV_ARGS_RUN);
    CHECK(!strcmp(a.pak, "game.pak") && !strcmp(a.cfg.lang, "en") && !strcmp(a.storage, "saves"));
    CHECK(a.cfg.volume == 40 && a.cfg.aux_volume == 70 && a.cfg.audio_rate == 22050);
    CHECK(a.cfg.opn_core == 1 && a.no_voice == 1);
    CHECK(parse(&a, 2, "--opn", "lite2") == LRV_ARGS_RUN && a.cfg.opn_core == 2);
    CHECK(parse(&a, 2, "--opn", "ymfm") == LRV_ARGS_RUN && a.cfg.opn_core == 0);
    CHECK(parse(&a, 2, "--opn", "lite3") == LRV_ARGS_BAD);
    CHECK(parse(&a, 7, "--voice-pak", "v.pak", "--autoplay", "ok:5", "--control", "--max-ticks",
                "99") == LRV_ARGS_RUN);
    CHECK(!strcmp(a.voice_pak, "v.pak") && !strcmp(a.autoplay, "ok:5") && a.control);
    CHECK(a.max_ticks == 99);

    /* volume 0 is silence, not "the default"; --no-audio wins over --rate */
    CHECK(parse(&a, 2, "--volume", "0") == LRV_ARGS_RUN && a.cfg.volume < 0);
    CHECK(parse(&a, 3, "--rate", "48000", "--no-audio") == LRV_ARGS_RUN && a.cfg.audio_rate == 0);

    /* the backend's and the engine's options reach their functions */
    opt_speed = backend_scale = opt_view_seen = 0;
    CHECK(parse(&a, 5, "--fullscreen", "--scale", "3", "--speed", "7") == LRV_ARGS_RUN);
    CHECK(a.cfg.fullscreen == 1 && backend_scale == 3 && opt_speed == 7);
    /* an optional argument is taken unless the next word is an option */
    CHECK(parse(&a, 2, "--view", "TITLE") == LRV_ARGS_RUN && opt_view);
    CHECK(opt_view && !strcmp(opt_view, "TITLE"));
    CHECK(parse(&a, 2, "--view", "--fullscreen") == LRV_ARGS_RUN && !opt_view && a.cfg.fullscreen);
    CHECK(parse(&a, 1, "--view") == LRV_ARGS_RUN && !opt_view && opt_view_seen == 3);

    quiet(1);
    CHECK(parse(&a, 1, "--nonsense") == LRV_ARGS_BAD);
    CHECK(parse(&a, 1, "stray") == LRV_ARGS_BAD);
    CHECK(parse(&a, 1, "--pak") == LRV_ARGS_BAD);               /* argument missing */
    CHECK(parse(&a, 1, "--control=1") == LRV_ARGS_BAD);         /* a flag takes none */
    CHECK(parse(&a, 2, "--volume", "loud") == LRV_ARGS_BAD);
    CHECK(parse(&a, 2, "--volume", "101") == LRV_ARGS_BAD);
    CHECK(parse(&a, 2, "--opn", "best") == LRV_ARGS_BAD);
    CHECK(parse(&a, 2, "--speed", "0") == LRV_ARGS_BAD);        /* the engine said no */
    quiet(0);

    /* the usage text names every option and carries both help texts */
    f = tmpfile();
    CHECK(f != 0);
    if (!f)
        return;
    lrv_args_usage(f, &test_app, &test_backend);
    rewind(f);
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    CHECK(strstr(text, "usage: engine [OPTION...]") == text);
    CHECK(strstr(text, "--pak FILE") && strstr(text, "--scale N") && strstr(text, "--speed N"));
    CHECK(strstr(text, "--view [PICTURE]") && strstr(text, "--max-ticks N"));
    CHECK(strstr(text, "\n                        instead of playing\n") != 0);
    CHECK(strstr(text, "keys: none\n") && strstr(text, "backend keys: none\n"));
}

/* the same tables walked by a host with a command line syntax of its own */
static int option_index(const char *name)
{
    const struct lrv_option *o;
    int i;
    for (i = 0; (o = lrv_args_option_at(&test_app, &test_backend, i)) != 0; i++)
        if (!strcmp(o->name, name))
            return i;
    return -1;
}

static int set(struct lrv_args *a, const char *name, const char *arg)
{
    return lrv_args_set(a, &test_app, &test_backend, option_index(name), arg);
}

static void test_args_walk(void)
{
    struct lrv_args a;
    const struct lrv_option *o;
    int i, n = 0, tests = 0;

    /* the common options without --help and --version, then the backend's, then the engine's */
    for (i = 0; (o = lrv_args_option_at(&test_app, &test_backend, i)) != 0; i++) {
        CHECK(strcmp(o->name, "help") && strcmp(o->name, "version"));
        tests += lrv_args_option_is_test(&test_app, &test_backend, i);
        n++;
    }
    CHECK(n == 13 + 2 + 2 && tests == 3);
    CHECK(option_index("pak") == 0 && option_index("fullscreen") == 13);
    CHECK(option_index("speed") == 15 && option_index("view") == 16);
    CHECK(lrv_args_option_is_test(&test_app, &test_backend, option_index("autoplay")));
    CHECK(!lrv_args_option_is_test(&test_app, &test_backend, option_index("scale")));
    CHECK(!lrv_args_option_at(&test_app, &test_backend, -1));

    /* nothing given */
    lrv_args_begin(&a);
    lrv_args_end(&a, &test_backend);
    CHECK(a.cfg.audio_rate == 44100 && !a.pak && !a.no_voice);

    /* options of all three sets mean what they mean to lrv_args_parse */
    opt_speed = backend_scale = opt_view_seen = 0;
    lrv_args_begin(&a);
    CHECK(set(&a, "pak", "game.pak") == 0 && set(&a, "voice-pak", "v.pak") == 0);
    CHECK(set(&a, "volume", "0") == 0 && set(&a, "rate", "22050") == 0);
    CHECK(set(&a, "no-voice", 0) == 0 && set(&a, "max-ticks", "99") == 0);
    CHECK(set(&a, "fullscreen", "ignored") == 0 && set(&a, "scale", "3") == 0);
    CHECK(set(&a, "speed", "7") == 0 && set(&a, "view", 0) == 0);
    lrv_args_end(&a, &test_backend);
    CHECK(!strcmp(a.pak, "game.pak") && !strcmp(a.voice_pak, "v.pak") && a.no_voice);
    CHECK(a.cfg.volume < 0 && a.cfg.audio_rate == 22050 && a.max_ticks == 99);
    CHECK(a.cfg.fullscreen == 1 && backend_scale == 3 && opt_speed == 7);
    CHECK(opt_view_seen == 1 && !opt_view);
    CHECK(set(&a, "view", "TITLE") == 0 && opt_view && !strcmp(opt_view, "TITLE"));

    /* --no-audio wins over --rate here too */
    lrv_args_begin(&a);
    CHECK(set(&a, "rate", "48000") == 0 && set(&a, "no-audio", 0) == 0);
    lrv_args_end(&a, &test_backend);
    CHECK(a.cfg.audio_rate == 0);

    /* what is refused, silently: reporting is the backend's */
    CHECK(set(&a, "pak", 0) != 0);                  /* argument missing */
    CHECK(set(&a, "volume", "loud") != 0);
    CHECK(set(&a, "speed", "0") != 0);              /* the engine said no */
    CHECK(lrv_args_set(&a, &test_app, &test_backend, n, 0) != 0);   /* no such option */
}

/* ---- autoplay ------------------------------------------------------------------------------ */

static void poll_at(struct lrv_autoplay *ap, uint32_t tick, struct lrv_input *in)
{
    memset(in, 0, sizeof *in);
    lrv_autoplay_poll(ap, tick, in);
}

static void test_autoplay(void)
{
    struct lrv_autoplay ap;
    struct lrv_input in;
    char err[80];
    uint32_t event;
    int key;

    /* names: keys, the two aliases, events */
    CHECK(lrv_input_by_name("ok", 2, &key, &event) && key == LRV_K_RETURN && !event);
    CHECK(lrv_input_by_name("cancel", 6, &key, &event) && key == LRV_K_ESC);
    CHECK(lrv_input_by_name("KP8", 3, &key, &event) && key == LRV_K_KP8);
    CHECK(lrv_input_by_name("reset", 5, &key, &event) && !key && event == LRV_EV_RESET);
    CHECK(!lrv_input_by_name("stop", 4, &key, &event) && !lrv_input_by_name("", 0, &key, &event));

    /* at: entries, held keys as modifiers, clicks, snapshots */
    CHECK(lrv_autoplay_parse(&ap, "at:10=a+ok,5=b,20=mouse_l+quit\tshift:8-12 button_r:1-3\n"
                                  "snap:30,7 ctrl:40-41", err, sizeof err) == LRV_OK);
    CHECK(ap.at_count == 3 && ap.at[0].tick == 5 && ap.hold_count == 3 && ap.snap_count == 2);
    CHECK(lrv_autoplay_last_tick(&ap) == 41);
    CHECK(lrv_autoplay_snap_due(&ap, 7) && lrv_autoplay_snap_due(&ap, 30));
    CHECK(!lrv_autoplay_snap_due(&ap, 8));
    poll_at(&ap, 1, &in);
    CHECK(in.nkeys == 0 && in.buttons == LRV_BTN_R && !in.events);
    poll_at(&ap, 6, &in);                           /* the entry of tick 5, one poll late */
    CHECK(in.nkeys == 1 && in.keys[0].key == LRV_K_B && in.keys[0].chr == 'b' && !in.buttons);
    poll_at(&ap, 6, &in);
    CHECK(in.nkeys == 0);                           /* once */
    poll_at(&ap, 10, &in);
    CHECK(in.nkeys == 2 && in.keys[0].key == LRV_K_A && in.keys[0].chr == 'A');
    CHECK(in.keys[0].mods == LRV_MOD_SHIFT && in.keys[1].key == LRV_K_RETURN);
    CHECK(in.keys[1].chr == '\r' && lrv_key_down(&in, LRV_K_SHIFT) && in.mods == LRV_MOD_SHIFT);
    poll_at(&ap, 12, &in);
    CHECK(!lrv_key_down(&in, LRV_K_SHIFT) && !in.mods);
    /* a flush drops the presses that are due, but never a quit */
    lrv_autoplay_flush(&ap, 20);
    poll_at(&ap, 21, &in);
    CHECK(in.events == LRV_EV_QUIT && in.buttons == 0 && in.nkeys == 0);
    poll_at(&ap, 40, &in);
    CHECK(lrv_key_down(&in, LRV_K_CTRL) && in.mods == LRV_MOD_CTRL && !in.events);
    lrv_autoplay_free(&ap);

    /* periodic presses: once per tick, however often the engine polls */
    CHECK(lrv_autoplay_parse(&ap, "ok:30 kp2:45", err, sizeof err) == LRV_OK);
    poll_at(&ap, 30, &in);
    CHECK(in.nkeys == 1 && in.keys[0].key == LRV_K_RETURN);
    poll_at(&ap, 30, &in);
    CHECK(in.nkeys == 0);
    poll_at(&ap, 31, &in);
    CHECK(in.nkeys == 0);
    poll_at(&ap, 90, &in);
    CHECK(in.nkeys == 2 && in.keys[1].key == LRV_K_KP2 && in.keys[1].chr == '2');
    lrv_autoplay_free(&ap);

    /* rand: is deterministic */
    {
        struct lrv_autoplay other;
        struct lrv_input in2;
        uint32_t t;
        int presses = 0;
        CHECK(lrv_autoplay_parse(&ap, "rand:7", err, sizeof err) == LRV_OK);
        CHECK(lrv_autoplay_parse(&other, "rand:7", err, sizeof err) == LRV_OK);
        for (t = 0; t < 400; t++) {
            poll_at(&ap, t, &in);
            poll_at(&other, t, &in2);
            CHECK(in.nkeys == in2.nkeys && in.keys[0].key == in2.keys[0].key);
            presses += in.nkeys;
        }
        CHECK(presses == 20);
        lrv_autoplay_free(&ap);
        lrv_autoplay_free(&other);
    }

    /* what is wrong is said, and nothing is left allocated (the sanitizer checks) */
    CHECK(lrv_autoplay_parse(&ap, "at:5=a,6=stop", err, sizeof err) == LRV_ERR_FORMAT);
    CHECK(!strcmp(err, "autoplay: unknown key: stop"));
    CHECK(lrv_autoplay_parse(&ap, "at:5", err, sizeof err) == LRV_ERR_FORMAT);
    CHECK(lrv_autoplay_parse(&ap, "shift:5-", err, sizeof err) == LRV_ERR_FORMAT);
    CHECK(lrv_autoplay_parse(&ap, "quit:5", err, sizeof err) == LRV_ERR_FORMAT);
    CHECK(lrv_autoplay_parse(&ap, "button_r:5", err, sizeof err) == LRV_ERR_FORMAT);
    CHECK(lrv_autoplay_parse(&ap, "faster", err, sizeof err) == LRV_ERR_FORMAT);
    CHECK(lrv_autoplay_parse(&ap, "@/nonexistent/spec", err, sizeof err) == LRV_ERR_IO);
}

/* ---- paths -------------------------------------------------------------------------------- */

static char scratch[512];

static const char *in_scratch(const char *name)
{
    static char path[4][600];
    static int next;
    next = (next + 1) % 4;
    snprintf(path[next], sizeof path[next], "%s/%s", scratch, name);
    return path[next];
}

static void put_file(const char *name, const void *data, size_t n)
{
    FILE *f = fopen(in_scratch(name), "wb");
    CHECK(f != 0);
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static void test_paths(void)
{
    static const char *const names[] = {
        "bin/engine", "bin/engine.pak", "bin/other.pak", "bin/second.pak",
        "bin/other_voice.pak", "cwd/game.pak", "store/a/b/rec", "store/a/b/rec.tmp", 0
    };
    static const char *const dirs[] = { "store/a/b", "store/a", "store", "cwd", "bin", 0 };
    const char *tmp = getenv("TMPDIR");
    char out[LRV_PATH_MAX], buf[16];
    uint8_t trailer[16];
    size_t len;
    int i;

    snprintf(scratch, sizeof scratch, "%s/lrvne-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(scratch) != 0);
    mkdir(in_scratch("bin"), 0700);
    mkdir(in_scratch("cwd"), 0700);
    put_file("bin/engine", "ELF", 3);

    /* the main pack: nothing; the only pack of the current directory; the only pack next to
     * the program (a voice pack does not count); <program>.pak before any other */
    CHECK(!lrv_paths_find_pack(out, sizeof out, in_scratch("bin/engine"), in_scratch("cwd")));
    put_file("cwd/game.pak", "XPAK", 4);
    CHECK(lrv_paths_find_pack(out, sizeof out, in_scratch("bin/engine"), in_scratch("cwd")));
    CHECK(!strcmp(out, in_scratch("cwd/game.pak")));
    CHECK(lrv_paths_find_pack(out, sizeof out, 0, in_scratch("cwd")));
    put_file("bin/other.pak", "XPAK", 4);
    put_file("bin/other_voice.pak", "XPAK", 4);
    CHECK(lrv_paths_find_pack(out, sizeof out, in_scratch("bin/engine"), in_scratch("cwd")));
    CHECK(!strcmp(out, in_scratch("bin/other.pak")));
    put_file("bin/second.pak", "XPAK", 4);          /* two: neither is "the only" one */
    CHECK(lrv_paths_find_pack(out, sizeof out, in_scratch("bin/engine"), in_scratch("cwd")));
    CHECK(!strcmp(out, in_scratch("cwd/game.pak")));
    put_file("bin/engine.pak", "XPAK", 4);
    CHECK(lrv_paths_find_pack(out, sizeof out, in_scratch("bin/engine"), in_scratch("cwd")));
    CHECK(!strcmp(out, in_scratch("bin/engine.pak")));

    /* a pack appended to the program wins over all of them */
    memcpy(trailer, "XPKT", 4);
    lrv_wr32le(trailer + 4, 100);
    lrv_wr32le(trailer + 8, ~100u);
    memcpy(trailer + 12, "XPKT", 4);
    CHECK(!lrv_paths_has_pack(in_scratch("bin/engine")));
    put_file("bin/engine", trailer, sizeof trailer);
    CHECK(lrv_paths_has_pack(in_scratch("bin/engine")));
    CHECK(lrv_paths_find_pack(out, sizeof out, in_scratch("bin/engine"), in_scratch("cwd")));
    CHECK(!strcmp(out, in_scratch("bin/engine")));
    trailer[8] ^= 1;                                /* a damaged trailer is not a pack */
    put_file("bin/engine", trailer, sizeof trailer);
    CHECK(!lrv_paths_has_pack(in_scratch("bin/engine")));

    /* the voice pack's name follows the main pack's */
    CHECK(lrv_paths_voice_pack(out, sizeof out, "dir/game.pak"));
    CHECK(!strcmp(out, "dir/game_voice.pak"));
    CHECK(lrv_paths_voice_pack(out, sizeof out, "bin/engine"));
    CHECK(!strcmp(out, "bin/engine_voice.pak"));
    CHECK(!lrv_paths_voice_pack(out, 8, "dir/game.pak"));

    /* the storage directory */
    CHECK(lrv_paths_storage_dir(out, sizeof out, 0, "engine", "game", "/xdg", "/h/u") == LRV_OK);
    CHECK(!strcmp(out, "/xdg/engine/game"));
    CHECK(lrv_paths_storage_dir(out, sizeof out, 0, "engine", "game", "", "/h/u") == LRV_OK);
    CHECK(!strcmp(out, "/h/u/.local/share/engine/game"));
    CHECK(lrv_paths_storage_dir(out, sizeof out, "mine", "engine", "game", "/xdg", 0) == LRV_OK);
    CHECK(!strcmp(out, "mine"));
    CHECK(lrv_paths_storage_dir(out, sizeof out, 0, "engine", "../x", "/xdg", 0) == LRV_ERR_RANGE);
    CHECK(lrv_paths_storage_dir(out, sizeof out, 0, "engine", "", "/xdg", 0) == LRV_ERR_RANGE);
    CHECK(lrv_paths_storage_dir(out, 10, 0, "engine", "game", "/xdg", 0) == LRV_ERR_RANGE);
    CHECK(lrv_paths_name_ok("FLAG1") && !lrv_paths_name_ok("a/b") && !lrv_paths_name_ok(".."));

    /* records */
    CHECK(lrv_paths_mkdirs(in_scratch("store/a/b")) == LRV_OK);
    CHECK(lrv_paths_mkdirs(in_scratch("store/a/b")) == LRV_OK);       /* already there */
    CHECK(lrv_paths_mkdirs(in_scratch("bin/engine/sub")) == LRV_ERR_IO);
    CHECK(lrv_paths_record_read(in_scratch("store/a/b"), "rec", buf, sizeof buf, &len)
          == LRV_ERR_NOTFOUND);
    CHECK(lrv_paths_record_write(in_scratch("store/a/b"), "rec", "first", 5) == LRV_OK);
    CHECK(lrv_paths_record_write(in_scratch("store/a/b"), "rec", "second!", 7) == LRV_OK);
    CHECK(lrv_paths_record_read(in_scratch("store/a/b"), "rec", buf, sizeof buf, &len) == LRV_OK);
    CHECK(len == 7 && !memcmp(buf, "second!", 7));
    CHECK(lrv_paths_record_read(in_scratch("store/a/b"), "rec", buf, 3, &len) == LRV_OK);
    CHECK(len == 3);
    CHECK(access(in_scratch("store/a/b/rec.tmp"), F_OK) != 0);        /* nothing left behind */
    CHECK(lrv_paths_record_write(in_scratch("store/a/b"), "../rec", "x", 1) == LRV_ERR_RANGE);
    CHECK(lrv_paths_record_write(in_scratch("store/none"), "rec", "x", 1) == LRV_ERR_IO);

    for (i = 0; names[i]; i++)
        remove(in_scratch(names[i]));
    for (i = 0; dirs[i]; i++)
        rmdir(in_scratch(dirs[i]));
    CHECK(rmdir(scratch) == 0);
}

/* ---- the presentation test and snapshots -------------------------------------------------- */

static struct lrv_video_mode pt_mode;
static int pt_presents, pt_snaps;
static uint32_t pt_first, pt_last;

static void pt_present(const struct lrv_frame *f)
{
    pt_presents++;
    CHECK(f->page->w == pt_mode.w && f->page->h == pt_mode.h);
    if (f->dirty && !lrv_rect_empty(f->dirty))
        CHECK(f->dirty->x0 >= 0 && f->dirty->y0 >= 0 && f->dirty->x1 <= f->page->stride &&
              f->dirty->y1 <= f->page->h);
}

static void pt_snap(int index, const char *name, const struct lrv_frame *f, uint32_t hash)
{
    CHECK(index == pt_snaps && name && *name);
    CHECK(hash == lrv_frame_hash32(f));
    CHECK(f->colours == pt_mode.colours && f->rgb_bits == pt_mode.rgb_bits);
    if (pt_snaps++ == 0)
        pt_first = hash;
    pt_last = hash;
}

static void test_ptest(void)
{
    static const struct lrv_ptest_io io = { pt_present, pt_snap };
    static const struct lrv_video_mode pc98 = {
        LRV_FMT_PLANAR4, 640, 400, 4, 3, 17724211u, 16, 4
    };
    static const struct lrv_video_mode small = {
        LRV_FMT_PLANAR4, 320, 200, 4, 3, 16666667u, 16, 3
    };
    struct lrv_video_mode bad = pc98;

    /* the sequence on 640x400 is a fixed one: these hashes are what every backend must show */
    pt_mode = pc98;
    pt_presents = pt_snaps = 0;
    CHECK(lrv_ptest_run(&pc98, &io) == 25 && pt_snaps == 25 && pt_presents > 200);
    CHECK(pt_first == 0x89dc5ad9u && pt_last == 0x91c98518u);

    /* another mode: the same sequence scaled, inside the page (the sanitizer checks) */
    pt_mode = small;
    pt_presents = pt_snaps = 0;
    CHECK(lrv_ptest_run(&small, &io) == 25);

    bad.format = LRV_FMT_INDEXED8;
    CHECK(lrv_ptest_run(&bad, &io) == LRV_ERR_RANGE);
}

static void test_shot(void)
{
    uint8_t planes[4][4] = {
        { 0x80, 0x00, 0x00, 0x01 }, { 0x80, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x01 }, { 0x00, 0x00, 0x00, 0x01 }
    };
    uint8_t pal[48], idx[16];
    char hex[16];
    struct lrv_surface page = {
        LRV_FMT_PLANAR4, 16, 2, 2, { planes[0], planes[1], planes[2], planes[3] }
    };
    struct lrv_frame f;
    struct lrv_shot shot;
    char text[1024];
    uint32_t h0;
    size_t n;
    FILE *out;
    int i;

    for (i = 0; i < 48; i++)
        pal[i] = (uint8_t)(i % 16);
    memset(&f, 0, sizeof f);
    f.page = &page;
    f.rgb = pal;
    f.colours = 16;
    f.rgb_bits = 4;
    f.display_on = 1;

    /* pixel (0,0) is colour 3, pixel (15,1) colour 13; two pixels a byte */
    CHECK(lrv_shot_indices_size(&f) == 16);
    lrv_shot_indices(&f, idx);
    CHECK(idx[0] == 0x30 && idx[1] == 0 && idx[15] == 0x0D);
    f.scroll_line = 1;                              /* line 1 is shown first */
    lrv_shot_indices(&f, idx);
    CHECK(idx[7] == 0x0D && idx[8] == 0x30);

    /* the hash sees the picture, the scroll line, the palette and the display switch */
    h0 = lrv_frame_hash32(&f);
    f.scroll_line = 0;
    CHECK(lrv_frame_hash32(&f) != h0);
    f.scroll_line = 1;
    pal[47] ^= 1;
    CHECK(lrv_frame_hash32(&f) != h0);
    pal[47] ^= 1;
    f.display_on = 0;
    CHECK(lrv_frame_hash32(&f) != h0);
    f.display_on = 1;
    CHECK(lrv_frame_hash32(&f) == h0);

    /* a copy outlives the page it was taken from */
    memset(&shot, 0, sizeof shot);
    CHECK(lrv_shot_capture(&shot, &f) == LRV_OK && shot.valid);
    memset(planes, 0, sizeof planes);
    CHECK(lrv_frame_hash32(&shot.frame) == h0);
    CHECK(lrv_shot_hash64(&shot.frame) != lrv_shot_hash64(&f));

    out = tmpfile();
    CHECK(out != 0);
    if (out) {
        lrv_shot_emit_snap(out, &shot.frame, "a \"name\"", 7);
        lrv_json_b64(out, (const uint8_t *)"ab", 2);
        lrv_json_b64(out, (const uint8_t *)"a", 1);
        rewind(out);
        n = fread(text, 1, sizeof text - 1, out);
        text[n] = 0;
        fclose(out);
        snprintf(hex, sizeof hex, "%08lx", (unsigned long)h0);
        CHECK(strstr(text, "{\"ev\":\"snap\",\"name\":\"a \\\"name\\\"\",\"t\":7,") == text);
        CHECK(strstr(text, hex) && strstr(text, "\"size\":[16,2],\"pal\":\"000102"));
        CHECK(strstr(text, "\"gon\":1,\"scroll\":1,\"data\":\"AAAAAAAAAA0wAAAAAAAAAA==\"}\n"));
        CHECK(strstr(text, "}\n\"YWI=\"\"YQ==\"") != 0);
    }
    lrv_shot_free(&shot);
    page.format = LRV_FMT_INDEXED8;
    CHECK(lrv_shot_capture(&shot, &f) == LRV_ERR_RANGE);
}

/* ---- WAV ---------------------------------------------------------------------------------- */

static int render_tone(void *ctx, const struct lrv_hal *hal, const struct lrv_run_cfg *cfg,
                       const char *name, double seconds, uint32_t *rate,
                       void (*sink)(void *sctx, const int16_t *pcm, int frames), void *sctx)
{
    int16_t pcm[100];
    int i;
    (void)ctx;
    (void)cfg;
    if (strcmp(name, "tone"))
        return LRV_ERR_NOTFOUND;
    lrv_log(hal, LRV_LOG_INFO, "rendering %s", name);
    for (i = 0; i < 100; i++)
        pcm[i] = (int16_t)(i * 256 - 12800);
    *rate = 8000;
    sink(sctx, pcm, (int)(seconds * 100));
    return LRV_OK;
}

static void test_wav(void)
{
    static const struct lrv_app app = { .name = "engine", .render_music = render_tone };
    static const struct lrv_app mute = { .name = "engine" };
    const char *tmp = getenv("TMPDIR");
    struct lrv_run_cfg cfg;
    struct lrv_hal hal;
    uint8_t file[512];
    char path[600];
    size_t n;
    FILE *f;
    int fd;

    th_hal(&hal);
    memset(&cfg, 0, sizeof cfg);
    snprintf(path, sizeof path, "%s/lrvne-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);
    quiet(1);
    CHECK(lrv_wav_render(&app, &hal, &cfg, path, "tone", 1.0) == 0);
    quiet(0);
    CHECK(!strcmp(th_last_log, "rendering tone"));
    f = fopen(path, "rb");
    n = f ? fread(file, 1, sizeof file, f) : 0;
    if (f)
        fclose(f);
    CHECK(n == 44 + 200 && !memcmp(file, "RIFF", 4) && !memcmp(file + 8, "WAVEfmt ", 8));
    CHECK(lrv_rd32le(file + 4) == 36 + 200 && lrv_rd16le(file + 20) == 1);
    CHECK(lrv_rd16le(file + 22) == 1);
    CHECK(lrv_rd32le(file + 24) == 8000 && lrv_rd32le(file + 28) == 16000);
    CHECK(lrv_rd16le(file + 32) == 2 && lrv_rd16le(file + 34) == 16);
    CHECK(!memcmp(file + 36, "data", 4));
    CHECK(lrv_rd32le(file + 40) == 200 && file[44] == 0x00 && file[45] == 0xCE && file[47] == 0xCF);

    quiet(1);
    CHECK(lrv_wav_render(&app, &hal, &cfg, path, "other", 1.0) == 1);     /* no such song */
    CHECK(lrv_wav_render(&app, &hal, &cfg, path, 0, 1.0) == 2);           /* --song missing */
    CHECK(lrv_wav_render(&mute, &hal, &cfg, path, "tone", 1.0) == 2);     /* cannot render */
    quiet(0);
    remove(path);
}

int main(void)
{
    test_args();
    test_args_walk();
    test_autoplay();
    test_paths();
    test_ptest();
    test_shot();
    test_wav();
    if (th_fails) {
        fprintf(stderr, "test_backends: %d failed\n", th_fails);
        return 1;
    }
    printf("test_backends: ok\n");
    return 0;
}
