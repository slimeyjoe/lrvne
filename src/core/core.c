/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Helpers on top of the HAL (lrvne/core.h): formatted diagnostics and checked allocation. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lrvne/core.h"
#include "lrvne/hal.h"

const char *lrv_strerror(int err)
{
    switch (err) {
    case LRV_OK:           return "no error";
    case LRV_ERR_NOMEM:    return "out of memory";
    case LRV_ERR_IO:       return "read or write failed";
    case LRV_ERR_FORMAT:   return "bad data";
    case LRV_ERR_NOTFOUND: return "not found";
    case LRV_ERR_RANGE:    return "out of range";
    default:               return "failed";
    }
}

void lrv_log(const struct lrv_hal *hal, int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hal->log(level, buf);
}

void lrv_fatal(const struct lrv_hal *hal, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hal->fatal(buf);
    for (;;)            /* a backend's fatal does not return; this keeps the promise */
        ;
}

void *lrv_alloc(const struct lrv_hal *hal, size_t n)
{
    void *p = hal->mem_alloc(n ? n : 1, LRV_MEM_ANY);
    if (p)
        memset(p, 0, n);
    return p;
}

void *lrv_xalloc(const struct lrv_hal *hal, size_t n)
{
    void *p = lrv_alloc(hal, n);
    if (!p)
        lrv_fatal(hal, "out of memory (%lu bytes)", (unsigned long)n);
    return p;
}

void lrv_free(const struct lrv_hal *hal, void *p, size_t n)
{
    if (p)
        hal->mem_free(p, n ? n : 1);
}

int lrv_name_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb)
            return 0;
        if (ca == 0)
            return 1;
    }
}
