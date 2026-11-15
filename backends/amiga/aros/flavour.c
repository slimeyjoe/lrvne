/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AROS (TARGET=aros, x86_64 ABIv11): defaults and drivers (flavour.h).
 *
 * No chipset: sound goes through AHI, the screen is whatever graphics card mode BestModeID
 * finds for the page at depth 8 (video.c), and the page reaches it as chunky pixels
 * (upload_chunky.c). A PC has the speed for FM synthesis at 44 100 Hz. */
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

static const struct am_audio_drv *const g_audio[] = { &am_audio_ahi, 0 };
static const struct am_upload *const g_upload[] = { &am_upload_chunky, &am_upload_rtg, 0 };

const struct am_flavour am_flavour = {
    "AROS",
    INVALID_ID,                             /* BestModeID (video.c) */
    44100,
    MEMF_ANY,
    UNIT_MICROHZ,                           /* WAITUNTIL returns in 20 ms steps there */
    g_audio,
    g_upload,
    262144,                                 /* hosted AROS: host calls run on task stacks */
    0                                       /* no planar display */
};

ULONG am_p96_bitmap_attr(struct Library *p96base, struct BitMap *bm, ULONG attr)
{
    (void)p96base; (void)bm; (void)attr;
    return 0;
}
