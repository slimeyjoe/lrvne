/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AmigaOS 3.x on the 68k (TARGET=amiga): defaults and drivers (flavour.h). */
#include <stddef.h>
#include "os_begin.h"
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/modeid.h>
#include <devices/timer.h>
#include "os_end.h"
#include "amiga.h"
#include "audio.h"
#include "flavour.h"
#include "upload.h"

static const struct am_audio_drv *const g_audio[] = { &am_audio_paula, &am_audio_ahi, 0 };
static const struct am_upload *const g_upload[] = { &am_upload_planar, &am_upload_rtg,
                                                    &am_upload_chunky, 0 };

const struct am_flavour am_flavour = {
    "AmigaOS 3",
    DEFAULT_MONITOR_ID | HIRESLACE_KEY,     /* Picasso96 may promote it to a graphics card mode */
    11025,                                  /* FM synthesis at 22 050 Hz can cost more than a
                                             * 68040/25 has (README.md "Audio") */
    MEMF_FAST,
    UNIT_WAITUNTIL,
    g_audio,
    g_upload,
    32768,
    &am_upload_planar_map
};

/* jsr -42(a6) with a0 = bitmap, d0 = attribute */
static ULONG p96_get_bitmap_attr(__reg("a6") struct Library *base, __reg("a0") struct BitMap *bm,
                                 __reg("d0") ULONG attr) = "\tjsr\t-42(a6)";

ULONG am_p96_bitmap_attr(struct Library *p96base, struct BitMap *bm, ULONG attr)
{
    return p96_get_bitmap_attr(p96base, bm, attr);
}

int am_snap_capture(const struct am_snapinfo *si, const char *path)
{
    (void)si; (void)path;
    return 0;                       /* the host takes it (snap.c) */
}

/* The main task's stack. A shell gives a program 4 KB unless told otherwise (`Stack`), an icon
 * what its stack field says, and an engine on lrvne needs more than 4 KB (xenon: 6.8 KB at the
 * start of a game; a stack that runs over goes on into whatever memory lies below it). vbcc's
 * startup code gives the program a stack of at least __stack bytes, allocating a new one and
 * swapping to it when it was started with less; core/main.c's high-water mark measures that
 * stack (the task's bounds, which StackSwap sets). */
size_t __stack = 32768;

int am_main_stack(int (*body)(void))
{
    return body();
}
