/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The contract between an engine and the backends.
 *
 * A backend knows nothing about engines or games: no names, no file names, no options of
 * theirs. An engine describes itself in a struct lrv_app and its main() is one line:
 *
 *     int main(int argc, char **argv) { return lrv_app_main(&my_app, argc, argv); }
 *
 * lrv_app_main is the backend the program was linked with. It parses the command line (its
 * own options, the options every engine has, and the engine's through app->option), builds the
 * HAL and calls app->run, which plays until the game quits.
 *
 * What a backend derives from the app and the game:
 *   pack      --pak FILE; else the pack appended to the executable; else <program>.pak next to
 *             the executable (<program> = the executable's own file name, so a renamed copy
 *             of an engine finds its game); else the only *.pak there (voice packs aside);
 *             then the same in the current directory. The voice pack is <pack>_voice.pak
 *             beside the pack.
 *   storage   --storage DIR; else <user data dir>/<app->name>/<game id>, the id being what
 *             the engine passes to hal->storage_open (the profile's `id`). The headless
 *             backend keeps to the current directory instead: a test never touches a
 *             player's saves.
 *   title     what the engine puts in lrv_video_cfg.title (the profile's `title`).
 * A backend for a host with other conventions keeps to the same rules in the host's terms
 * (the Amiga family: options as AmigaDOS keywords and tool types, the pack in PROGDIR:, the
 * saves in PROGDIR:saves/<game id>; backends/amiga/README.md). */
#ifndef LRVNE_APP_H
#define LRVNE_APP_H

#include <stdint.h>
#include "lrvne/hal.h"

/* What the command line asked for, common to all engines. */
struct lrv_run_cfg {
    int fullscreen;             /* for lrv_video_cfg */
    int scale_mode;
    uint32_t host_mode;
    const char *lang;           /* --lang: a language tag, NULL = the game's default */
    int audio_rate;             /* requested output rate, 0 = audio off */
    int volume;                 /* music, percent 1..100; 0 = the default, negative = silent */
    int aux_volume;             /* voice, percent 1..100; 0 = the default */
    int opn_core;               /* FM emulation: 0 = the best built in, 1 = the light C core,
                                 * 2 = the light core oversampled (lrv_opn_select, _flags) */
};

enum lrv_opt_arg { LRV_OPT_FLAG = 0, LRV_OPT_ARG, LRV_OPT_OPTARG };

/* An option of the engine's own ("--view [PICTURE]"). */
struct lrv_option {
    const char *name;           /* without the dashes */
    int arg;                    /* enum lrv_opt_arg; OPTARG: an argument follows unless the
                                 * next word starts with '-' */
    const char *arg_name;       /* for the usage text, e.g. "PICTURE" */
    const char *help;
};

struct lrv_app {
    const char *name;           /* the engine's program name, lower case: usage, messages and
                                 * the storage directory */
    const char *version;
    void *ctx;                  /* passed back to the functions below */

    /* the engine's options; option() is called for each one given (arg NULL when it has
     * none) and returns 0, or nonzero to reject the argument */
    const struct lrv_option *options;
    int noptions;
    int (*option)(void *ctx, const char *name, const char *arg);
    const char *help;           /* optional: text appended to the usage (the keys) */
    int host_pointer;           /* 1: the game draws no mouse pointer of its own, so the
                                 * host's stays visible over the picture */

    /* Runs the game until it quits. Returns the process exit code. */
    int (*run)(void *ctx, const struct lrv_hal *hal, const struct lrv_run_cfg *cfg);

    /* optional, offline: plays song `name` for `seconds` through the sound path the game uses
     * and gives the output to sink; *rate receives the sample rate. Needs only the pack,
     * memory and log parts of the HAL. Returns 0 or enum lrv_err. */
    int (*render_music)(void *ctx, const struct lrv_hal *hal, const struct lrv_run_cfg *cfg,
                        const char *name, double seconds, uint32_t *rate,
                        void (*sink)(void *sctx, const int16_t *pcm, int frames), void *sctx);

    /* test automation: the names of the engine's trace kinds, trace_names[k - LRV_TR_ENGINE],
     * and whether a wait kind (a0 of LRV_TR_WAIT) waits for the player's input, which is
     * where the control protocol stops for commands */
    const char *const *trace_names;
    int ntrace_names;
    int (*wait_is_input)(int wait_kind);
};

/* Provided by the backend the program is linked with. Returns the process exit code. */
int lrv_app_main(const struct lrv_app *app, int argc, char **argv);

#endif
