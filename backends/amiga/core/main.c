/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Amiga family's backend: lrv_app_main for AmigaOS 3.x, WarpOS and AROS. Startup, the
 * options, the HAL table, memory and diagnostics.
 *
 * Options. The program is started from a shell with AmigaDOS arguments or from the Workbench
 * with the tool types of its icon; both use the same keywords, and both are driven by the
 * option tables every backend shares (backends/common/args.h): the options every engine has,
 * this backend's own (g_options below) and the engine's (app->options). An option "voice-pak"
 * is the keyword VOICEPAK: upper case, dashes dropped. In the shell's template a flag is
 * KEYWORD/S and an option with an argument KEYWORD/K; as tool types they are KEYWORD and
 * KEYWORD=value. An option whose argument is optional takes KEYWORD="" (shell) or a bare
 * KEYWORD (tool type) for "no argument". `?` prints the template as for any AmigaDOS command,
 * HELP the list with a line about each option.
 *
 * Test options: the autoplay spec's snap: ticks make the program pause and hand a snapshot to
 * a test rig on the host (snap.c); FAST drops the frame pacing (an engine is deterministic
 * tick for tick, so the snapshots do not change); PRESENTTEST runs the synthetic presentation
 * test (backends/common/ptest.h) in the video mode the engine opens, instead of the game;
 * PRESENTTESTMODE=pc98 runs it in that machine's mode without starting the engine at all.
 *
 * README.md describes the backend as a whole. */
#include "os_begin.h"
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/icon.h>
#include "os_end.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga.h"
#include "lrvne/gfx.h"
#include "lrvne/machine.h"
#include "lrvne/scale.h"
#include "args.h"
#include "autoplay.h"
#include "control.h"
#include "ptest.h"
#include "amsys.h"
#include "flavour.h"
#include "frameos.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *IconBase;

struct am_opts am_opts;
const struct lrv_app *am_app;
struct Task *am_main_task;
int am_from_wb;
int am_audio_rate;
int am_no_voice;
char am_name_audio[48], am_name_aux[48];

static struct lrv_args g_args;
static struct lrv_autoplay g_ap;
static int g_autoplay;
static uint32_t g_frames;
static int g_out_of_ticks;                      /* MAXTICKS ran out: the next poll says quit */
static uint32_t g_snap_hash, g_snap_hash_tick;  /* hash of the frame presented for a snap tick */
static struct RDArgs *g_rda;
static struct DiskObject *g_dobj;               /* the icon: its tool types stay in use */

/* memory statistics: the engine's allocations by where they landed ([1] = chip), and the
 * system's free memory low-water mark ([0] = fast) */
static ULONG g_mem_cur[2], g_mem_peak[2], g_mem_peak_sum, g_mem_peak_frame, g_mem_big;
static ULONG g_free0[2], g_free_min[2];

static void shutdown(void);

/* ---- console ------------------------------------------------------------------------------ */

static void con_write(const char *s, int n)
{
    BPTR out = am_from_wb ? 0 : Output();
    if (out && n > 0)
        Write(out, (APTR)s, n);
}

static void con_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_write(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

void am_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    if (am_opts.quiet || am_from_wb || FindTask(NULL) != am_main_task)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_write(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

int am_strieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
        if (!ca)
            return 1;
    }
}

/* ---- the backend's options ---------------------------------------------------------------- */

static const struct lrv_option g_options[] = {
    { "mode", LRV_OPT_ARG, "0xID", "the display mode id of the program's own screen" },
    { "window", LRV_OPT_FLAG, 0,
      "in a window on a public screen (needs 16 free pens: a 256-colour\n"
      "or graphics card screen); F10 steps through its sizes" },
    { "pubscreen", LRV_OPT_ARG, "NAME", "that screen (default: the default public screen)" },
    { "scale", LRV_OPT_ARG, "1|2|3|auto", "the picture's pixels shown that many times as large" },
    { "aspect", LRV_OPT_FLAG, 0, "the picture in the shape it had on the original's monitor" },
    { "upload", LRV_OPT_ARG, "NAME", "how a graphics card screen is drawn (rtg, chunky)" },
    { "audio", LRV_OPT_ARG, "DRIVER", "the sound output: paula, ahi or none" },
    { "audiobuf", LRV_OPT_ARG, "FRAMES", "samples per audio block (default: 93 ms)" },
    { "audiocpu", LRV_OPT_ARG, "10..100",
      "percent of real time the sound may cost before blocks are\n"
      "skipped (default 75)" },
    { "ahiunit", LRV_OPT_ARG, "N", "the ahi.device unit (default 0)" },
    { "paula14", LRV_OPT_FLAG, 0, "14-bit Paula output on all four channels" },
    { "voicemix", LRV_OPT_FLAG, 0, "the voice mixed into the music, not on channels of its own" },
    { "preload", LRV_OPT_ARG, "auto|yes|no", "the whole game pack read into memory at start" },
    { "quiet", LRV_OPT_FLAG, 0, "no console output" },
    { "help", LRV_OPT_FLAG, 0, "this list" },
    { "fast", LRV_OPT_FLAG, 0, "testing: no frame pacing" },
    { "snapdir", LRV_OPT_ARG, "DIR", "testing: where snapshot requests go (default PROGDIR:)" },
    { "present-test", LRV_OPT_FLAG, 0, "testing: run the presentation test and exit" },
    { "present-test-mode", LRV_OPT_ARG, "MACHINE",
      "testing: the same in that machine's mode (pc98), without the game" }
};

/* a decimal or 0x hexadecimal number and nothing else */
static int parse_ulong(const char *s, ULONG *v)
{
    char *end;
    unsigned long n;
    if (!s || !*s)
        return 0;
    n = strtoul(s, &end, 0);
    if (*end)
        return 0;
    *v = (ULONG)n;
    return 1;
}

static int option(struct lrv_args *a, const char *name, const char *arg)
{
    ULONG v = 0;
    (void)a;
    if (!strcmp(name, "mode")) {
        if (!parse_ulong(arg, &v))
            return 1;
        am_opts.mode_id = v;
    } else if (!strcmp(name, "window")) {
        am_opts.window = 1;
    } else if (!strcmp(name, "pubscreen")) {
        am_opts.pubscreen = arg;
    } else if (!strcmp(name, "scale")) {
        if (am_strieq(arg, "auto")) {
            am_opts.scale = 0;          /* the largest that fits */
        } else {
            if (!parse_ulong(arg, &v) || v < 1 || v > LRV_SCALE_MAX)
                return 1;
            am_opts.scale = (int)v;
        }
    } else if (!strcmp(name, "aspect")) {
        am_opts.aspect = 1;
    } else if (!strcmp(name, "upload")) {
        am_opts.upload = arg;
    } else if (!strcmp(name, "audio")) {
        am_opts.audio = arg;
    } else if (!strcmp(name, "audiobuf")) {
        if (!parse_ulong(arg, &v) || v < 64 || v > 65534)
            return 1;
        am_opts.audiobuf = (LONG)v;
    } else if (!strcmp(name, "audiocpu")) {
        if (!parse_ulong(arg, &v) || v < 10 || v > 100)
            return 1;
        am_opts.audio_cpu = (LONG)v;
    } else if (!strcmp(name, "ahiunit")) {
        if (!parse_ulong(arg, &v) || v > 255)
            return 1;
        am_opts.ahi_unit = (LONG)v;
    } else if (!strcmp(name, "paula14")) {
        am_opts.paula14 = 1;
    } else if (!strcmp(name, "voicemix")) {
        am_opts.voicemix = 1;
    } else if (!strcmp(name, "preload")) {
        if (!am_strieq(arg, "auto") && !am_strieq(arg, "yes") && !am_strieq(arg, "no"))
            return 1;
        am_opts.preload = arg;
    } else if (!strcmp(name, "quiet")) {
        am_opts.quiet = 1;
    } else if (!strcmp(name, "help")) {
        am_opts.help = 1;
    } else if (!strcmp(name, "fast")) {
        am_opts.fast = 1;
    } else if (!strcmp(name, "snapdir")) {
        am_opts.snapdir = arg;
    } else if (!strcmp(name, "present-test")) {
        am_opts.present_test = 1;
    } else if (!strcmp(name, "present-test-mode")) {
        if (!lrv_video_mode_by_name(arg))
            return 1;
        am_opts.present_test = 1;
        am_opts.present_mode = arg;
    } else {
        return 1;
    }
    return 0;
}

static struct lrv_args_backend g_backend = {
    g_options, (int)(sizeof g_options / sizeof g_options[0]), option, 0,
    "keys: Shift+Esc quits at once, Ctrl+Shift+R resets (the game starts again,\n"
    "  the saves are kept), right Amiga+K toggles kana input, F10 in a window is\n"
    "  its next size; Ctrl-C in the shell asks the game to quit"
};

/* ---- the options in AmigaDOS form --------------------------------------------------------- */

#define KEYWORD_MAX 32

/* the keyword of an option: upper case, dashes dropped ("voice-pak" -> VOICEPAK) */
static void keyword(char *out, const char *name)
{
    int n = 0;
    for (; *name && n < KEYWORD_MAX - 1; name++) {
        int c = *name;
        if (c == '-')
            continue;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[n++] = (char)c;
    }
    out[n] = 0;
}

static int option_count(void)
{
    int n = 0;
    while (lrv_args_option_at(am_app, &g_backend, n))
        n++;
    return n;
}

static void defaults(void)
{
    memset(&am_opts, 0, sizeof am_opts);
    am_opts.audio_cpu = 75;
    am_opts.scale = 1;
    g_backend.default_rate = (int)am_flavour.rate;
    lrv_args_begin(&g_args);
}

/* an option was given; says so itself when the argument is no good. 0 or -1 */
static int option_given(int i, const char *arg)
{
    const struct lrv_option *o = lrv_args_option_at(am_app, &g_backend, i);
    char kw[KEYWORD_MAX];

    if (o->arg == LRV_OPT_OPTARG && arg && !*arg)
        arg = 0;                        /* KEYWORD="" or a bare tool type: no argument */
    if (lrv_args_set(&g_args, am_app, &g_backend, i, arg) == 0)
        return 0;
    keyword(kw, o->name);
    if (arg)
        con_printf("%s: %s: bad argument '%s'\n", am_app->name, kw, arg);
    else
        con_printf("%s: %s cannot be used here\n", am_app->name, kw);
    return -1;
}

/* the shell: a ReadArgs template made from the tables */
static int parse_cli(void)
{
    int n = option_count(), i, rc = 0;
    size_t cap = (size_t)n * (KEYWORD_MAX + 3) + 1;
    char *tmpl = malloc(cap), *t;
    IPTR *args = calloc((size_t)n + 1, sizeof *args);

    if (!tmpl || !args) {
        free(tmpl);
        free(args);
        return -1;
    }
    for (t = tmpl, i = 0; i < n; i++) {
        const struct lrv_option *o = lrv_args_option_at(am_app, &g_backend, i);
        if (i)
            *t++ = ',';
        keyword(t, o->name);
        t += strlen(t);
        strcpy(t, o->arg == LRV_OPT_FLAG ? "/S" : "/K");
        t += 2;
    }
    *t = 0;
    g_rda = ReadArgs((STRPTR)tmpl, AM_RDARGS_ARRAY(args), NULL);
    if (!g_rda) {
        PrintFault(IoErr(), (STRPTR)am_app->name);
        rc = -1;
    }
    /* the strings belong to g_rda, which lives until the program ends */
    for (i = 0; g_rda && i < n; i++)
        if (args[i] && option_given(i, lrv_args_option_at(am_app, &g_backend, i)->arg ==
                                           LRV_OPT_FLAG ? 0 : (const char *)args[i]) != 0)
            rc = -1;
    free(tmpl);
    free(args);
    return rc;
}

/* the Workbench: the tool types of the program's icon. One that makes no sense is skipped:
 * there is nowhere to say so, and the game should still start. */
static void parse_tooltypes(void)
{
    struct WBArg *wa = am_wbmsg->sm_ArgList;
    BPTR old;
    int n = option_count(), i;

    if (!IconBase || !wa->wa_Name)
        return;
    old = CurrentDir(wa->wa_Lock);
    g_dobj = GetDiskObject((STRPTR)wa->wa_Name);
    CurrentDir(old);
    if (!g_dobj)
        return;
    for (i = 0; i < n; i++) {
        const struct lrv_option *o = lrv_args_option_at(am_app, &g_backend, i);
        char kw[KEYWORD_MAX];
        const char *v;
        keyword(kw, o->name);
        v = (const char *)FindToolType((UBYTE **)g_dobj->do_ToolTypes, (UBYTE *)kw);
        if (v)
            option_given(i, o->arg == LRV_OPT_FLAG ? 0 : v);
    }
}

#define HELP_COLUMN 26

static void help_text(const char *h)
{
    for (; *h; h++) {
        con_write(h, 1);
        if (*h == '\n')
            con_printf("%*s", HELP_COLUMN, "");
    }
    con_write("\n", 1);
}

/* HELP: what lrv_args_usage prints elsewhere, in this host's spelling */
static void print_help(void)
{
    int pass, i;
    const struct lrv_option *o;

    con_printf("%s %s (lrvne %s, %s)\n", am_app->name, am_app->version ? am_app->version : "",
               LRV_VERSION, am_flavour.name);
    for (pass = 0; pass < 2; pass++) {
        if (pass)
            con_printf("testing:\n");
        for (i = 0; (o = lrv_args_option_at(am_app, &g_backend, i)) != 0; i++) {
            char kw[KEYWORD_MAX], head[80];
            const char *an = o->arg_name ? o->arg_name : "ARG";
            int test = lrv_args_option_is_test(am_app, &g_backend, i) ||
                       !strncmp(o->help ? o->help : "", "testing: ", 9);
            int col;
            if (test != pass)
                continue;
            keyword(kw, o->name);
            if (o->arg == LRV_OPT_ARG)
                col = sprintf(head, "  %s=%.40s", kw, an);
            else if (o->arg == LRV_OPT_OPTARG)
                col = sprintf(head, "  %s[=%.40s]", kw, an);
            else
                col = sprintf(head, "  %s", kw);
            con_write(head, col);
            if (col > HELP_COLUMN - 2) {
                con_write("\n", 1);
                col = 0;
            }
            con_printf("%*s", HELP_COLUMN - col, "");
            help_text(o->help ? o->help + (pass && !strncmp(o->help, "testing: ", 9) ? 9 : 0)
                              : "");
        }
        if (!pass) {
            con_printf("%s\n", g_backend.help);
            if (am_app->help && *am_app->help)
                con_printf("%s%s", am_app->help,
                           am_app->help[strlen(am_app->help) - 1] == '\n' ? "" : "\n");
        }
    }
    con_printf("From the Workbench the same keywords are tool types of the program's icon.\n");
}

/* ---- HAL: video / time / input ----------------------------------------------------------- */

static void pt_snap(int index, const char *name, const struct lrv_frame *frame, uint32_t hash)
{
    char nm[48];
    (void)frame;
    snprintf(nm, sizeof nm, "%02d-%s", index, name);
    am_snap(nm, (uint32_t)index, hash, 1);
}

static int v_open(const struct lrv_video_cfg *cfg)
{
    int rc = am_video_open(cfg);

    if (rc != LRV_OK)
        return rc;
    am_timer_set_period(cfg->mode.frame_ns);
    if (am_opts.present_test) {
        static const struct lrv_ptest_io io = { am_video_present, pt_snap };
        int n = lrv_ptest_run(&cfg->mode, &io);
        am_log("present test: %d snapshots\n", n);
        shutdown();
        exit(n > 0 ? RETURN_OK : RETURN_FAIL);
    }
    return LRV_OK;
}

static void v_present(const struct lrv_frame *frame)
{
    ULONG t0;
    /* the frame shown at the next tick: hash it when that tick takes a snapshot (overlays
     * are in the page only during the present) */
    if (g_autoplay && lrv_autoplay_snap_due(&g_ap, g_frames + 1)) {
        g_snap_hash = lrv_frame_hash32(frame);
        g_snap_hash_tick = g_frames + 1;
    }
    t0 = am_prof_now();
    am_video_present(frame);
    am_prof_present(t0, frame->page_switched || frame->scroll_line != 0);
}

static void mem_note(ULONG c, ULONG f)
{
    if (c < g_free_min[1]) g_free_min[1] = c;
    if (f < g_free_min[0]) g_free_min[0] = f;
}

static void t_frame_wait(void)
{
    /* the frame clock, the input gather and the memory sample: one OS call (frameos.h) */
    am_frame_wait((am_opts.fast ? 0 : AM_F_WAIT) | AM_F_INPUT | AM_F_MEM);
    g_frames++;
    mem_note(am_fr->avail_chip, am_fr->avail_fast);
    if (g_autoplay && lrv_autoplay_snap_due(&g_ap, g_frames)) {
        char name[24];
        snprintf(name, sizeof name, "t%lu", (unsigned long)g_frames);
        am_snap(name, g_frames, g_snap_hash, g_snap_hash_tick == g_frames);
    }
    if (g_args.max_ticks > 0 && !g_out_of_ticks && (long)g_frames > g_args.max_ticks) {
        am_log("max ticks reached\n");
        /* a quit request lets the engine shut down normally on its next input poll */
        g_out_of_ticks = 1;
    }
}

static void i_poll(struct lrv_input *in)
{
    am_input_poll(in);
    if (g_autoplay)
        lrv_autoplay_poll(&g_ap, g_frames, in);
    if (g_out_of_ticks)
        in->events |= LRV_EV_EMERGENCY;
}

static void i_flush(void)
{
    am_input_flush();
    if (g_autoplay)
        lrv_autoplay_flush(&g_ap, g_frames);
}

/* ---- memory and diagnostics -------------------------------------------------------------- */

#define STACK_PAINT 0x5354414BUL     /* "STAK" */
static struct am_stack g_main_stack;

void am_stack_paint(struct am_stack *st)
{
    void *lower, *upper;
    uintptr_t here = (uintptr_t)&lower;
    ULONG *p;
    am_task_stack(&lower, &upper);  /* the PPC stack on WarpOS */
    st->lower = (ULONG *)(((uintptr_t)lower + 3) & ~(uintptr_t)3);
    st->upper = (ULONG *)upper;
    if (here <= (uintptr_t)st->lower + 512 || here > (uintptr_t)st->upper) {
        st->lower = st->upper = 0;
        return;
    }
    for (p = st->lower; (uintptr_t)p < here - 256; p++)
        *p = STACK_PAINT;
}

ULONG am_stack_used(const struct am_stack *st)
{
    const ULONG *p = st->lower;
    if (!p)
        return 0;
    while (p < st->upper && *p == STACK_PAINT)
        p++;
    return (ULONG)((const UBYTE *)st->upper - (const UBYTE *)p);
}

ULONG am_stack_size(const struct am_stack *st)
{
    return (ULONG)((const UBYTE *)st->upper - (const UBYTE *)st->lower);
}

/* The engine's memory. Nothing gives memory back to an Amiga when a program ends, so every
 * block is kept on a list (a header in front of it) and what the engine still holds when the
 * backend shuts down is released there: after a fatal error, after PRESENTTEST, after an
 * engine that forgot something. Only the main task allocates (code above the HAL is
 * single-threaded), so the list needs no lock. */
union mem_head {
    struct {
        union mem_head *prev, *next;
        ULONG size;                     /* of the whole allocation, this header included */
    } h;
    char pad[4 * sizeof(void *)];       /* keeps the block's alignment: 16 or 32 bytes */
};
static union mem_head *g_mem_list;

static void *m_alloc(size_t n, unsigned flags)
{
    ULONG size = (ULONG)n + (ULONG)sizeof(union mem_head);
    union mem_head *m;
    int k;

    if (n > 0x7FFFFFFFUL - sizeof(union mem_head))
        return 0;
    m = AllocMem(size, (flags & LRV_MEM_CHIP) ? MEMF_CHIP : MEMF_ANY);
    if (!m)
        return 0;
    m->h.size = size;
    m->h.prev = 0;
    m->h.next = g_mem_list;
    if (g_mem_list)
        g_mem_list->h.prev = m;
    g_mem_list = m;
    k = (TypeOfMem(m) & MEMF_CHIP) != 0;
    g_mem_cur[k] += (ULONG)n;
    if (g_mem_cur[k] > g_mem_peak[k]) g_mem_peak[k] = g_mem_cur[k];
    if (g_mem_cur[0] + g_mem_cur[1] > g_mem_peak_sum) {
        g_mem_peak_sum = g_mem_cur[0] + g_mem_cur[1];
        g_mem_peak_frame = g_frames;
    }
    if ((ULONG)n > g_mem_big) g_mem_big = (ULONG)n;
    mem_note(AvailMem(MEMF_CHIP), AvailMem(MEMF_FAST));
    return m + 1;
}

static void m_free(void *p, size_t n)
{
    union mem_head *m = p;

    (void)n;                            /* the header knows */
    if (!p)
        return;
    m--;
    if (m->h.prev)
        m->h.prev->h.next = m->h.next;
    else
        g_mem_list = m->h.next;
    if (m->h.next)
        m->h.next->h.prev = m->h.prev;
    g_mem_cur[(TypeOfMem(m) & MEMF_CHIP) != 0] -= m->h.size - (ULONG)sizeof(union mem_head);
    FreeMem(m, m->h.size);
}

/* what the engine did not free (shutdown) */
static void mem_release_all(void)
{
    while (g_mem_list) {
        union mem_head *m = g_mem_list;
        g_mem_list = m->h.next;
        FreeMem(m, m->h.size);
    }
    g_mem_cur[0] = g_mem_cur[1] = 0;
}

static void l_log(int level, const char *msg)
{
    if (level >= LRV_LOG_INFO)
        am_log("%s\n", msg);
}

static void l_fatal(const char *msg)
{
    struct EasyStruct es;
    am_log("fatal: %s\n", msg);
    am_video_close();                       /* the requester must be visible */
    es.es_StructSize = sizeof es;
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)am_app->name;
    es.es_TextFormat = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)"Quit";
    if (IntuitionBase)
        EasyRequest(NULL, &es, NULL, msg);
    shutdown();
    exit(RETURN_FAIL);
}

/* ---- main ---------------------------------------------------------------------------------- */

/* everything the backend holds, in the order it must go; safe to call twice and half-started */
static void shutdown(void)
{
    am_video_close();
    am_audio_close();
    am_audio_exit();
    am_timer_close();
    am_frame_free();
    mem_release_all();                  /* nothing of the engine runs any more */
    if (g_autoplay)
        lrv_autoplay_free(&g_ap);
    g_autoplay = 0;
    if (g_rda)
        FreeArgs(g_rda);
    g_rda = 0;
    if (g_dobj && IconBase)
        FreeDiskObject(g_dobj);
    g_dobj = 0;
    if (IconBase) CloseLibrary(IconBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    IconBase = 0;
    GfxBase = 0;
    IntuitionBase = 0;
}

static void report(int rc)
{
    ULONG t = am_ticks(), us = am_timer_period_us();
    am_log("exit %d after %lu frames in %lu frame periods of real time (%lu us each), "
           "%lu ms per frame\n",
           rc, (unsigned long)g_frames, (unsigned long)t, (unsigned long)us,
           (unsigned long)(g_frames ? (unsigned long long)t * us / 1000ULL / g_frames : 0));
    am_log("memory: engine peak %lu KB at frame %lu (fast %lu, chip %lu), largest block %lu KB, "
           "%lu bytes not freed by it; free low-water: chip %lu KB, fast %lu KB below the start "
           "(of %lu, %lu KB)\n",
           (unsigned long)(g_mem_peak_sum >> 10), (unsigned long)g_mem_peak_frame,
           (unsigned long)(g_mem_peak[0] >> 10), (unsigned long)(g_mem_peak[1] >> 10),
           (unsigned long)(g_mem_big >> 10), (unsigned long)(g_mem_cur[0] + g_mem_cur[1]),
           (unsigned long)((g_free0[1] - g_free_min[1]) >> 10),
           (unsigned long)((g_free0[0] - g_free_min[0]) >> 10),
           (unsigned long)(g_free0[1] >> 10), (unsigned long)(g_free0[0] >> 10));
    am_log("stack: main %lu of %lu bytes used, audio %lu of %lu\n",
           (unsigned long)am_stack_used(&g_main_stack), (unsigned long)am_stack_size(&g_main_stack),
           (unsigned long)am_audio_stack_used, (unsigned long)am_audio_stack_total);
}

static int run(void)
{
    struct am_paths_cfg paths;
    struct lrv_hal hal;
    int rc;

    am_main_task = FindTask(NULL);
    am_stack_paint(&g_main_stack);
    g_free_min[1] = g_free0[1] = AvailMem(MEMF_CHIP);
    g_free_min[0] = g_free0[0] = AvailMem(MEMF_FAST);
    am_from_wb = am_wbmsg != 0;
    snprintf(am_name_audio, sizeof am_name_audio, "%.32s audio", am_app->name);
    snprintf(am_name_aux, sizeof am_name_aux, "%.32s voice", am_app->name);
    defaults();
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IconBase = OpenLibrary("icon.library", 37);
    if (!IntuitionBase || !GfxBase) {
        con_printf("%s: needs AmigaOS 3.0 or newer (intuition and graphics V39)\n", am_app->name);
        shutdown();
        return RETURN_FAIL;
    }
    if (am_from_wb) {
        parse_tooltypes();
    } else if (parse_cli() != 0) {
        shutdown();
        return RETURN_ERROR;
    }
    lrv_args_end(&g_args, &g_backend);
    if (am_opts.help) {
        print_help();
        shutdown();
        return RETURN_OK;
    }
    if (g_args.control && !lrv_ctl_available()) {
        con_printf("%s: built without the control protocol\n", am_app->name);
        shutdown();
        return RETURN_ERROR;
    }
    am_log("%s %s (lrvne %s, %s)\n", am_app->name, am_app->version ? am_app->version : "",
           LRV_VERSION, am_flavour.name);
    if (am_sys_check() != 0) {          /* WarpOS: OS structures as the 68k lays them out */
        shutdown();
        return RETURN_FAIL;
    }
    /* the once-per-frame call's struct before anything opens a window (input.c fills it in) */
    if (am_frame_init() != 0)
        l_fatal("out of memory");
    if (am_timer_open() != 0)
        l_fatal("cannot open timer.device");
    am_audio_init();
    if (g_args.autoplay) {
        char err[96];
        if (lrv_autoplay_parse(&g_ap, g_args.autoplay, err, sizeof err) != LRV_OK)
            l_fatal(err);
        g_autoplay = 1;
        am_log("autoplay: %d presses (first at %lu), %d held ranges, %d snapshots\n",
               g_ap.at_count, (unsigned long)(g_ap.at_count ? g_ap.at[0].tick : 0),
               g_ap.hold_count, g_ap.snap_count);
    }

    if (am_opts.audio && am_strieq(am_opts.audio, "none"))
        g_args.cfg.audio_rate = 0;
    am_audio_rate = g_args.cfg.audio_rate;
    am_no_voice = g_args.no_voice;
    g_args.cfg.fullscreen = !am_opts.window;
    g_args.cfg.host_mode = am_opts.mode_id;

    memset(&paths, 0, sizeof paths);
    paths.pak = g_args.pak;
    paths.voice_pak = g_args.voice_pak;
    paths.no_voice = g_args.no_voice;
    paths.storage = g_args.storage;
    am_paths_init(&paths);

    memset(&hal, 0, sizeof hal);
    hal.video_open = v_open;
    hal.video_close = am_video_close;
    hal.video_present = v_present;
    hal.frame_wait = t_frame_wait;
    hal.input_poll = i_poll;
    hal.input_flush = i_flush;
    hal.audio_open = am_audio_open;
    hal.audio_close = am_audio_close;
    hal.audio_lock = am_audio_lock;
    hal.audio_unlock = am_audio_unlock;
    hal.set_volume = am_audio_set_volume;
    /* the auxiliary stream: the driver's own output (Paula's free channels, a second AHI
     * stream), or mixed into the main stream by the library (PAULA14, VOICEMIX) */
    hal.aux_mode = am_audio_rate > 0 ? am_audio_aux_mode() : LRV_AUX_NONE;
    if (hal.aux_mode == LRV_AUX_CHANNEL) {
        hal.aux_open = am_aux_open;
        hal.aux_close = am_aux_close;
        hal.aux_space = am_aux_space;
        hal.aux_push = am_aux_push;
        hal.aux_queued = am_aux_queued;
        hal.aux_flush = am_aux_flush;
        /* two blocks in flight, as long as the main stream's, and as much again in the FIFO */
        hal.aux_lead_ms = (int)(4 * 1000L * am_audio_block_frames(am_audio_rate) / am_audio_rate);
    }
    hal.storage_open = am_storage_open;
    hal.storage_read = am_storage_read;
    hal.storage_write = am_storage_write;
    hal.pack_open = am_pack_open;
    hal.pack_read = am_pack_read;
    hal.pack_close = am_pack_close;
    hal.mem_alloc = m_alloc;
    hal.mem_free = m_free;
    hal.log = l_log;
    hal.fatal = l_fatal;

    if (am_opts.present_mode) {
        struct lrv_video_cfg vc;
        lrv_ptest_video_cfg(&vc, am_opts.present_mode, &g_args.cfg, am_app->name);
        v_open(&vc);                    /* runs the test and exits, unless the mode is refused */
        am_log("%s: PRESENTTESTMODE: the video mode could not be opened\n", am_app->name);
        report(RETURN_FAIL);
        shutdown();
        return RETURN_FAIL;
    }
    rc = am_app->run(am_app->ctx, &hal, &g_args.cfg);

    if (am_opts.present_test) {
        am_log("%s: PRESENTTEST: the engine opened no video mode\n", am_app->name);
        rc = RETURN_FAIL;
    }
    report(rc);
    shutdown();
    return rc;
}

int lrv_app_main(const struct lrv_app *app, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    am_app = app;
    return am_main_stack(run);
}
