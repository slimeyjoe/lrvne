/* SPDX-License-Identifier: GPL-3.0-or-later */
/* app_leak: an engine that forgets to free, for tests/smoke.sh. Its run allocates 100 bytes
 * through the HAL, runs frames until it is asked to leave and returns without freeing them;
 * the headless backend must then print what was left and end with exit code 1
 * (lrvne/app.h, struct lrv_app run). */
#include <string.h>
#include "lrvne/app.h"
#include "lrvne/core.h"
#include "lrvne/input.h"

static int leak_run(void *ctx, const struct lrv_hal *hal, const struct lrv_run_cfg *cfg)
{
    struct lrv_input in;
    (void)ctx;
    (void)cfg;
    if (!lrv_alloc(hal, 100))
        return 2;
    for (;;) {
        memset(&in, 0, sizeof in);
        hal->input_poll(&in);
        if (in.events & (LRV_EV_QUIT | LRV_EV_EMERGENCY))
            return 0;
        hal->frame_wait();
    }
}

static const struct lrv_app leak_app = {
    .name = "app_leak",
    .version = "0",
    .run = leak_run
};

int main(int argc, char **argv)
{
    return lrv_app_main(&leak_app, argc, argv);
}
