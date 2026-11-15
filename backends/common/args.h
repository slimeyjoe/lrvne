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

/* A decimal integer within [min, max] and nothing else. Returns 1 and sets *v, else 0. */
int lrv_args_int(const char *s, long min, long max, long *v);

#endif
