/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Snapshot handshake with a test rig on the host.
 *
 * At the autoplay spec's snap: ticks (and after each step of PRESENTTEST) the program waits
 * until the display shows the current buffer (a few vertical blanks: a flip takes effect at
 * the next one, an interlaced frame needs two fields), then writes a request file
 * SNAPDIR/snap_<name>.req with what the host needs to find the image in the machine's memory:
 *
 *   name=<name> tick=<n> hash=<8 hex>|none size=<w>x<h> standard=0|1 shown=<0|1> bpr=<n>
 *   rows=<n> depth=<n> mode=<8 hex> planes=<hex>,<hex>,.. penbase=<n>
 *   [sx=<n> dsth=<n> dx=<n> dy=<n>] [window=1 pens=<n>,.. mouse=<x>,<y>]
 *   [image=<file>] [p96mem=<hex> p96bpr=<n> p96fmt=<n>]
 *
 * (one line; written as .tmp and renamed, so the host never sees half a request). hash is
 * lrv_frame_hash32 of the frame that must be on the screen, the number the headless backend
 * prints for the same input; size the page's. sx..dy only on a SCALE/ASPECT screen: the page
 * is shown sx times as wide and dsth rows tall, through the row map of lrvne/scale.h, at
 * dx,dy in the bitmap.
 *
 * Where the flavour reads the page back itself (flavour.h am_snap_capture: AROS) the image is
 * already in SNAPDIR/snap_<name>.ppm, the request says image=, and the program goes on.
 * Otherwise it waits for the host: a rig that can stop the emulated machine (a debugger
 * stub) takes its screenshot, reads the displayed bitmap from the addresses given, and
 * creates snap_<name>.ack; the program deletes the ack and goes on. It waits for the ack as
 * long as it takes, with no deadline of its own (a busy host is no reason to give up: the
 * rig's timeout is the only one); Ctrl-C ends the wait, for a run without a rig. After a
 * snapshot, read back or answered, the frame clock starts again from now, so its pause is not
 * counted as a stall (timer.c am_frame_resync). On a graphics card bitmap the pixel memory comes from Picasso96API.library
 * (p96GetBitMapAttr; the library is optional, without it the request has no p96 fields). */
#include "os_begin.h"
#include <dos/dos.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include "os_end.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "amiga.h"
#include "flavour.h"

/* attribute numbers of p96GetBitMapAttr (flavour.h am_p96_bitmap_attr) from the P96 developer
 * kit's libraries/Picasso96.h */
#define P96BMA_MEMORY 3
#define P96BMA_BYTESPERROW 4
#define P96BMA_RGBFORMAT 7

static void wait_ack(const char *ack)
{
    for (;;) {
        BPTR lock = Lock((STRPTR)ack, SHARED_LOCK);
        if (lock) {
            UnLock(lock);
            DeleteFile((STRPTR)ack);
            break;
        }
        /* read, not cleared: the frame's input gather still sees it and quits */
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            am_log("snap: no answer from the host for %s, Ctrl-C\n", ack);
            break;
        }
        Delay(5);
    }
}

void am_snap(const char *name, uint32_t tick, uint32_t hash, int have_hash)
{
    const char *dir = am_opts.snapdir ? am_opts.snapdir : "PROGDIR:";
    char tmp[160], req[160], ack[160], img[160], line[560], *o;
    int captured;
    struct am_snapinfo si;
    struct Library *p96;
    BPTR fh;
    ULONG i;

    for (i = 0; i < 4; i++)
        WaitTOF();
    am_video_snapinfo(&si);
    snprintf(tmp, sizeof tmp, "%ssnap_%s.tmp", dir, name);
    snprintf(req, sizeof req, "%ssnap_%s.req", dir, name);
    snprintf(ack, sizeof ack, "%ssnap_%s.ack", dir, name);
    snprintf(img, sizeof img, "%ssnap_%s.ppm", dir, name);
    captured = am_snap_capture(&si, img);

    o = line;
    o += sprintf(o, "name=%s tick=%lu ", name, (unsigned long)tick);
    if (have_hash)
        o += sprintf(o, "hash=%08lx", (unsigned long)hash);
    else
        o += sprintf(o, "hash=none");
    o += sprintf(o, " size=%dx%d standard=%d shown=%d bpr=%lu rows=%lu depth=%lu mode=%08lx "
                 "planes=", si.w, si.h, si.standard, si.shown, (unsigned long)si.bpr,
                 (unsigned long)si.rows, (unsigned long)si.depth, (unsigned long)si.mode);
    for (i = 0; i < si.depth && i < 8; i++)
        o += sprintf(o, "%s%lx", i ? "," : "", (unsigned long)(uintptr_t)si.planes[i]);
    o += sprintf(o, " penbase=%d", si.pen_base);
    if (si.sx)                                  /* SCALE/ASPECT (upload.h am_geom) */
        o += sprintf(o, " sx=%d dsth=%d dx=%d dy=%d", si.sx, si.dst_h, si.dx, si.dy);
    if (si.window) {                            /* WINDOW: the pens of the page's colours */
        o += sprintf(o, " window=1 pens=");
        for (i = 0; i < 16; i++)
            o += sprintf(o, "%s%d", i ? "," : "", si.pens[i]);
        o += sprintf(o, " mouse=%d,%d", si.mouse_x, si.mouse_y);
    }
    if (captured)
        o += sprintf(o, " image=snap_%s.ppm", name);
    if (!si.standard && (p96 = OpenLibrary("Picasso96API.library", 2)) != 0) {
        o += sprintf(o, " p96mem=%lx p96bpr=%lu p96fmt=%lu",
                     (unsigned long)am_p96_bitmap_attr(p96, si.bm, P96BMA_MEMORY),
                     (unsigned long)am_p96_bitmap_attr(p96, si.bm, P96BMA_BYTESPERROW),
                     (unsigned long)am_p96_bitmap_attr(p96, si.bm, P96BMA_RGBFORMAT));
        CloseLibrary(p96);
    }
    *o++ = '\n';
    *o = 0;

    fh = Open((STRPTR)tmp, MODE_NEWFILE);
    if (!fh) {
        am_log("snap: cannot write %s\n", tmp);
        return;
    }
    Write(fh, line, (LONG)strlen(line));
    Close(fh);
    DeleteFile((STRPTR)req);
    if (!Rename((STRPTR)tmp, (STRPTR)req)) {
        am_log("snap: cannot rename %s\n", tmp);
        return;
    }
    if (!captured)
        wait_ack(ack);
    am_frame_resync();
}
