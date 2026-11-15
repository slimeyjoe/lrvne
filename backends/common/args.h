/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The command line of a program built on lrvne, parsed the same way by every backend.
 *
 * Three sets of options are understood: the ones every engine has (the table in args.c: the
 * pack, the language, the sound, the storage directory, the test automation), the backend's
 * own (a table it supplies) and the engine's (app->options, given to app->option). An option
 * is "--name", "--name ARG" or "--name=ARG"; the usage text is generated from the tables. */
#ifndef LRVNE_BACKENDS_ARGS_H
#define LRVNE_BACKENDS_ARGS_H

#include <stdio.h>
#include "lrvne/app.h"

/* What the common options said. Strings point into argv. */
struct lrv_args {
    struct lrv_run_cfg cfg;     /* for app->run; the backend's options fill the video part */
    const char *pak;            /* --pak */
    const char *voice_pak;      /* --voice-pak */
    int no_voice;               /* --no-voice */
    const char *storage;        /* --storage */
    const char *autoplay;       /* --autoplay: the spec (autoplay.h) */
    int control;                /* --control */
    long max_ticks;             /* --max-ticks, 0 = not given */
    long rate;                  /* --rate as given, 0 = not; cfg.audio_rate is set from this */
    int no_audio;               /* --no-audio                 and the backend's default rate */
};

/* A backend's part of the command line. */
struct lrv_args_backend {
    const struct lrv_option *options;
    int noptions;
    /* called for each of the backend's options given (arg NULL when it has none); returns 0,
     * or nonzero to reject the argument */
    int (*option)(struct lrv_args *a, const char *name, const char *arg);
    int default_rate;           /* audio rate when --rate is not given, 0 = no audio */
    const char *help;           /* optional: text appended to the usage (the backend's keys) */
};

enum lrv_args_result {
    LRV_ARGS_RUN = 0,           /* go on and run the engine */
    LRV_ARGS_DONE,              /* --help or --version was answered: exit with 0 */
    LRV_ARGS_BAD                /* an error was reported: exit with 2 */
};

/* Parses argv into *a (cleared first), calling the option functions of the backend and of the
 * engine as their options are met. Returns enum lrv_args_result. */
int lrv_args_parse(struct lrv_args *a, const struct lrv_app *app,
                   const struct lrv_args_backend *be, int argc, char **argv);

/* Writes the usage text. */
void lrv_args_usage(FILE *f, const struct lrv_app *app, const struct lrv_args_backend *be);

/* ---- for a host with a command line convention of its own -------------------------------- */

/* A backend whose host does not write options as "--name" (AmigaOS: a ReadArgs template in
 * the shell, tool types of an icon on the Workbench) walks the same three tables and gives
 * each option it was handed to the same code, so that an option means the same everywhere and
 * an engine's options work there without the engine knowing:
 *
 *     lrv_args_begin(&a);
 *     for (i = 0; (o = lrv_args_option_at(app, be, i)) != 0; i++)
 *         if (the host's syntax gave option o)
 *             bad |= lrv_args_set(&a, app, be, i, its argument or NULL);
 *     lrv_args_end(&a, be);
 *
 * The options come in the order of the usage text: the common ones (--help and --version are
 * not among them: a host has its own ways to ask), the backend's, the engine's. Nothing here
 * prints: reporting is the backend's. */

/* Clears *a. */
void lrv_args_begin(struct lrv_args *a);
/* Option i of the three tables in turn, NULL past the last. */
const struct lrv_option *lrv_args_option_at(const struct lrv_app *app,
                                            const struct lrv_args_backend *be, int i);
/* 1 when option i is one of the test automation options (the usage lists them apart). */
int lrv_args_option_is_test(const struct lrv_app *app, const struct lrv_args_backend *be, int i);
/* Option i was given, with `arg` (NULL: none; a flag takes none, an LRV_OPT_ARG option needs
 * one). Returns 0, or nonzero when the argument is missing or rejected. */
int lrv_args_set(struct lrv_args *a, const struct lrv_app *app,
                 const struct lrv_args_backend *be, int i, const char *arg);
/* After the last option: what follows from several of them (cfg.audio_rate). */
void lrv_args_end(struct lrv_args *a, const struct lrv_args_backend *be);

/* A decimal integer within [min, max] and nothing else. Returns 1 and sets *v, else 0. */
int lrv_args_int(const char *s, long min, long max, long *v);

#endif
