/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The part of ahi.device's device interface that audio_ahi.c uses: the I/O request of a
 * CMD_WRITE and one sample format, written out here from the device's documented interface so
 * that the backend builds without the AHI developer files (they are not part of the system
 * headers on AmigaOS 3.x, and their licence does not allow shipping them with this
 * repository). The names are the backend's own; a program that needs more of AHI should use
 * the developer kit's <devices/ahi.h> instead.
 *
 * Include it between os_begin.h and os_end.h, after <exec/io.h> and amcompat.h's IPTR: the
 * request is an OS structure (68k layout on WarpOS; on a 64-bit system the two private words
 * are pointer-sized). The WarpOS layout guard checks it (warpos/oslayout.h). */
#ifndef LRVNE_BACKENDS_AMIGA_AHIDEV_H
#define LRVNE_BACKENDS_AMIGA_AHIDEV_H

#define AM_AHI_NAME     "ahi.device"
#define AM_AHI_VERSION  4               /* the interface version asked for at OpenDevice */
#define AM_AHI_M16S     1UL             /* sample format: mono, 16 bit signed */

struct am_ahi_request {
    struct IOStdReq std;                /* io_Data, io_Length (bytes), io_Offset = 0 */
    UWORD version;                      /* set before OpenDevice */
    UWORD pad;
    IPTR  priv[2];                      /* the device's own */
    ULONG type;                         /* AM_AHI_M16S */
    ULONG frequency;                    /* Hz */
    LONG  volume;                       /* 16.16 fixed point, 0x10000 = full */
    LONG  position;                     /* 16.16 fixed point, 0x8000 = centre */
    struct am_ahi_request *link;        /* the request this one follows without a gap */
};

#endif
