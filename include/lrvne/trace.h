/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Trace events: what an engine reports about itself for test automation.
 *
 * An engine tells the HAL what it is doing (a script loaded, a wait entered, a picture shown)
 * when a backend installs hal->trace, which the control protocol does; otherwise an event
 * costs one pointer test. A test driver synchronises with these instead of sleeping.
 *
 * The layer defines the few kinds that mean the same in every engine. An engine numbers its
 * own from LRV_TR_ENGINE and gives their names to the backend (lrvne/app.h), so the protocol
 * stays generic. */
#ifndef LRVNE_TRACE_H
#define LRVNE_TRACE_H

#include <stdint.h>

enum lrv_trace_kind {
    LRV_TR_NONE = 0,
    LRV_TR_WAIT,        /* a0 = wait kind (the engine's), a1 = 0 enter / 1 leave,
                           a2 = ticks requested, or the reason for leaving */
    LRV_TR_ERROR,       /* text = message, a0 = code */
    LRV_TR_QUIT,        /* the engine is leaving its run function, a0 = exit code */
    LRV_TR_RESET,       /* the engine was reset: the game starts again */
    LRV_TR_ENGINE = 16  /* first kind an engine defines */
};

struct lrv_trace_event {
    int kind;                       /* enum lrv_trace_kind, or the engine's */
    uint32_t tick;                  /* frame count */
    uint32_t pos;                   /* script offset of the current statement, ~0u = none */
    const char *script;             /* current script name or NULL */
    const char *name;               /* asset / script / record name or NULL */
    const uint8_t *text;            /* text in the language's encoding, or NULL */
    const uint8_t *const *items;    /* n texts (menu items) or NULL */
    int n;
    int a0, a1, a2, a3;
    const int *ints;                /* nints integers (operands) or NULL */
    int nints;
};

#endif
