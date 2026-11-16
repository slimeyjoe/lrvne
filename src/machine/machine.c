/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The machines' video modes by name (lrvne/machine.h). A new machine adds a line here and its
 * name to lrv_video_mode_names. */
#include <stddef.h>
#include "lrvne/machine.h"
#include "lrvne/pc98.h"

static const struct {
    const char *name;
    const struct lrv_video_mode *mode;
} modes[] = {
    { "pc98", &lrv_pc98_video_mode }
};

const char lrv_video_mode_names[] = "pc98";

/* ASCII case-insensitive equality */
static int name_is(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return *a == *b;
}

const struct lrv_video_mode *lrv_video_mode_by_name(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof modes / sizeof modes[0]; i++)
        if (name_is(name, modes[i].name))
            return modes[i].mode;
    return NULL;
}
