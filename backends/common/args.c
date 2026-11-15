/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The command line parser of the backends (args.h). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "lrvne/core.h"
#include "args.h"

enum {
    O_PAK, O_LANG, O_STORAGE, O_VOLUME, O_NO_AUDIO, O_RATE, O_OPN, O_VOICE_PAK, O_NO_VOICE,
    O_VOICE_VOLUME,
    O_AUTOPLAY, O_CONTROL, O_MAX_TICKS,     /* testing, from O_AUTOPLAY on */
    O_HELP, O_VERSION,                      /* not listed: the usage text ends with them */
    O_COUNT
};

static const struct lrv_option g_common[O_COUNT] = {
    { "pak", LRV_OPT_ARG, "FILE",
      "the game pack (default: the one bundled into the program, else\n"
      "<program>.pak or the only .pak next to it, else in the current\n"
      "directory)" },
    { "lang", LRV_OPT_ARG, "TAG", "the language of the text (default: the game's)" },
    { "storage", LRV_OPT_ARG, "DIR", "where the saves are kept" },
    { "volume", LRV_OPT_ARG, "0..100", "the music's volume" },
    { "no-audio", LRV_OPT_FLAG, 0, "no sound at all" },
    { "rate", LRV_OPT_ARG, "HZ", "audio output rate" },
    { "opn", LRV_OPT_ARG, "ymfm|lite", "the FM chip emulation (lite: for slow machines)" },
    { "voice-pak", LRV_OPT_ARG, "FILE",
      "the voice pack (default: <pack>_voice.pak next to the game pack)" },
    { "no-voice", LRV_OPT_FLAG, 0, "no voice even when the voice pack is there" },
    { "voice-volume", LRV_OPT_ARG, "1..100", "the voice's volume" },
    { "autoplay", LRV_OPT_ARG, "SPEC", "scripted input; @FILE reads the spec from a file" },
    { "control", LRV_OPT_FLAG, 0, "the test automation protocol on stdin and stdout" },
    { "max-ticks", LRV_OPT_ARG, "N", "stop after N frames" },
    { "help", LRV_OPT_FLAG, 0, 0 },
    { "version", LRV_OPT_FLAG, 0, 0 }
};

int lrv_args_int(const char *s, long min, long max, long *v)
{
    char *end;
    long n;
    errno = 0;
    n = strtol(s, &end, 10);
    if (end == s || *end || errno || n < min || n > max)
        return 0;
    *v = n;
    return 1;
}

/* ---- usage -------------------------------------------------------------------------------- */

#define HELP_COLUMN 24

static void usage_options(FILE *f, const struct lrv_option *o, int n)
{
    int i;
    for (i = 0; i < n; i++, o++) {
        const char *h = o->help ? o->help : "";
        const char *arg = o->arg_name ? o->arg_name : "ARG";
        int col;
        if (o->arg == LRV_OPT_ARG)
            col = fprintf(f, "  --%s %s", o->name, arg);
        else if (o->arg == LRV_OPT_OPTARG)
            col = fprintf(f, "  --%s [%s]", o->name, arg);
        else
            col = fprintf(f, "  --%s", o->name);
        if (col > HELP_COLUMN - 2) {
            fputc('\n', f);
            col = 0;
        }
        fprintf(f, "%*s", HELP_COLUMN - col, "");
        for (; *h; h++) {
            fputc(*h, f);
            if (*h == '\n')
                fprintf(f, "%*s", HELP_COLUMN, "");
        }
        fputc('\n', f);
    }
}

static void usage_text(FILE *f, const char *text)
{
    if (text && *text)
        fprintf(f, "%s%s", text, text[strlen(text) - 1] == '\n' ? "" : "\n");
}

void lrv_args_usage(FILE *f, const struct lrv_app *app, const struct lrv_args_backend *be)
{
    fprintf(f, "usage: %s [OPTION...]\n", app->name);
    usage_options(f, g_common, O_AUTOPLAY);
    usage_options(f, be->options, be->noptions);
    usage_options(f, app->options, app->noptions);
    fprintf(f, "  --help, --version\n");
    usage_text(f, be->help);
    usage_text(f, app->help);
    fprintf(f, "testing:\n");
    usage_options(f, g_common + O_AUTOPLAY, O_HELP - O_AUTOPLAY);
}

/* ---- parsing ------------------------------------------------------------------------------ */

static int find(const struct lrv_option *o, int n, const char *name, size_t len)
{
    int i;
    for (i = 0; i < n; i++)
        if (strlen(o[i].name) == len && !strncmp(o[i].name, name, len))
            return i;
    return -1;
}

struct common_state {
    long rate;
    int no_audio;
};

/* one of the common options; returns 0, or nonzero when the argument is bad */
static int common_option(struct lrv_args *a, struct common_state *st, int which, const char *arg)
{
    long v;
    switch (which) {
    case O_PAK:       a->pak = arg; return 0;
    case O_LANG:      a->cfg.lang = arg; return 0;
    case O_STORAGE:   a->storage = arg; return 0;
    case O_VOICE_PAK: a->voice_pak = arg; return 0;
    case O_NO_VOICE:  a->no_voice = 1; return 0;
    case O_NO_AUDIO:  st->no_audio = 1; return 0;
    case O_AUTOPLAY:  a->autoplay = arg; return 0;
    case O_CONTROL:   a->control = 1; return 0;
    case O_VOLUME:
        if (!lrv_args_int(arg, 0, 100, &v))
            return 1;
        a->cfg.volume = v == 0 ? -1 : (int)v;
        return 0;
    case O_VOICE_VOLUME:
        if (!lrv_args_int(arg, 1, 100, &v))
            return 1;
        a->cfg.aux_volume = (int)v;
        return 0;
    case O_RATE:
        return !lrv_args_int(arg, 1000, 384000, &st->rate);
    case O_OPN:
        if (strcmp(arg, "ymfm") && strcmp(arg, "lite"))
            return 1;
        a->cfg.opn_core = !strcmp(arg, "lite");
        return 0;
    case O_MAX_TICKS:
        return !lrv_args_int(arg, 1, 0x7fffffffL, &a->max_ticks);
    default:
        return 1;
    }
}

int lrv_args_parse(struct lrv_args *a, const struct lrv_app *app,
                   const struct lrv_args_backend *be, int argc, char **argv)
{
    struct common_state st;
    int i;

    memset(a, 0, sizeof *a);
    memset(&st, 0, sizeof st);
    for (i = 1; i < argc; i++) {
        const char *word = argv[i], *name, *eq, *arg = 0;
        const struct lrv_option *opt;
        size_t len;
        int set, k, bad;

        if (!strcmp(word, "-h"))
            word = "--help";
        if (strncmp(word, "--", 2) != 0 || !word[2]) {
            fprintf(stderr, "%s: unexpected argument '%s' (try --help)\n", app->name, word);
            return LRV_ARGS_BAD;
        }
        name = word + 2;
        eq = strchr(name, '=');
        len = eq ? (size_t)(eq - name) : strlen(name);

        /* the common options first, then the backend's, then the engine's */
        if ((k = find(g_common, O_COUNT, name, len)) >= 0) {
            set = 0;
            opt = &g_common[k];
        } else if ((k = find(be->options, be->noptions, name, len)) >= 0) {
            set = 1;
            opt = &be->options[k];
        } else if ((k = find(app->options, app->noptions, name, len)) >= 0) {
            set = 2;
            opt = &app->options[k];
        } else {
            fprintf(stderr, "%s: unknown option '%s' (try --help)\n", app->name, word);
            return LRV_ARGS_BAD;
        }

        if (opt->arg == LRV_OPT_FLAG) {
            if (eq) {
                fprintf(stderr, "%s: --%s takes no argument\n", app->name, opt->name);
                return LRV_ARGS_BAD;
            }
        } else if (eq) {
            arg = eq + 1;
        } else if (opt->arg == LRV_OPT_ARG) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --%s needs an argument\n", app->name, opt->name);
                return LRV_ARGS_BAD;
            }
            arg = argv[++i];
        } else if (i + 1 < argc && argv[i + 1][0] != '-') {
            arg = argv[++i];                    /* LRV_OPT_OPTARG */
        }

        if (set == 0 && k == O_HELP) {
            lrv_args_usage(stdout, app, be);
            return LRV_ARGS_DONE;
        }
        if (set == 0 && k == O_VERSION) {
            printf("%s %s (lrvne %s)\n", app->name, app->version ? app->version : "",
                   LRV_VERSION);
            return LRV_ARGS_DONE;
        }
        if (set == 0)
            bad = common_option(a, &st, k, arg);
        else if (set == 1)
            bad = be->option ? be->option(a, opt->name, arg) : 1;
        else
            bad = app->option ? app->option(app->ctx, opt->name, arg) : 1;
        if (bad) {
            if (arg)
                fprintf(stderr, "%s: --%s: bad argument '%s'\n", app->name, opt->name, arg);
            else
                fprintf(stderr, "%s: --%s cannot be used here\n", app->name, opt->name);
            return LRV_ARGS_BAD;
        }
    }
    a->cfg.audio_rate = st.no_audio ? 0 : st.rate ? (int)st.rate : be->default_rate;
    return LRV_ARGS_RUN;
}
