// SPDX-License-Identifier: GPL-3.0-or-later
// ymfm's YM2203 behind the C chip-core interface (lrvne/opn.h). Built with LRV_HAVE_YMFM.
//
// ymfm generates at input_clock / 24 with OPN_FIDELITY_MIN (166 400 Hz for the 3.9936 MHz
// clock of the PC-98 boards), repeating each FM sample 3x and running the SSG at its own rate.
// The FM engine's real rate is clock / 72 = 55 467 Hz, so three generated samples are averaged
// into one output sample: FM samples are identical within a group, the SSG gets a 3-tap box
// filter. The host resamples from 55 467 Hz.
//
// The instance itself is allocated through the HAL; ymfm allocates its channels and operators
// with the C++ runtime when the chip object is constructed.
#include <new>
#include "ymfm/ymfm_opn.h"
extern "C" {
#include "lrvne/opn.h"
}

namespace {

class intf : public ymfm::ymfm_interface {
public:
    // Timers are modelled by the music driver; the chip's own timers are ignored.
    void ymfm_set_timer(uint32_t, int32_t) override {}
};

struct core {
    intf iface;
    ymfm::ym2203 chip;
    const struct lrv_hal *hal;
    uint32_t rate;
    int32_t hp_x1;               // DC-blocking high-pass state
    int64_t hp_y1;               // 16.16
    core() : chip(iface), hal(nullptr), rate(0), hp_x1(0), hp_y1(0) {}
};

struct lrv_opn *ymfm_create(const struct lrv_hal *hal, uint32_t clock_hz, uint32_t want_rate,
                            unsigned flags, uint32_t *rate)
{
    (void)want_rate;
    (void)flags;
    void *mem = lrv_alloc(hal, sizeof(core));
    if (!mem)
        return nullptr;
    core *c = new (mem) core();
    c->hal = hal;
    c->chip.set_fidelity(ymfm::OPN_FIDELITY_MIN);
    c->rate = c->chip.sample_rate(clock_hz) / 3;       // 166 400 / 3 = 55 467
    c->chip.reset();
    *rate = c->rate;
    return reinterpret_cast<struct lrv_opn *>(c);
}

void ymfm_destroy(struct lrv_opn *oc)
{
    core *c = reinterpret_cast<core *>(oc);
    if (!c)
        return;
    const struct lrv_hal *hal = c->hal;
    c->~core();
    lrv_free(hal, c, sizeof(core));
}

void ymfm_reset(struct lrv_opn *oc)
{
    reinterpret_cast<core *>(oc)->chip.reset();
}

void ymfm_write(struct lrv_opn *oc, uint8_t reg, uint8_t val)
{
    core *c = reinterpret_cast<core *>(oc);
    c->chip.write_address(reg);
    c->chip.write_data(val);
}

void ymfm_render(struct lrv_opn *oc, int16_t *mono, int frames)
{
    core *c = reinterpret_cast<core *>(oc);
    ymfm::ym2203::output_data out[3];
    for (int i = 0; i < frames; i++) {
        c->chip.generate(out, 3);
        // FM output (index 0) plus the three SSG channels (1-3). ymfm's SSG channels are
        // unipolar; the board's output coupling removes the DC on hardware, modelled by a
        // one-pole high-pass (~45 Hz) on the mix. Balance FM : SSG = 1 : 1/4 per channel
        // (the balance on hardware is not known).
        int32_t fm = (out[0].data[0] + out[1].data[0] + out[2].data[0]) / 3;
        int32_t ssg = 0;
        for (int k = 0; k < 3; k++)
            ssg += out[k].data[1] + out[k].data[2] + out[k].data[3];
        ssg = ssg / 12;
        int32_t x = (fm + ssg) * 2;          // level: about that of the libvgm reference
        // y = x - x1 + pole * y1 (pole = 1 - 334/65536, ~45 Hz at 55 467 Hz, same as opn_lite),
        // y1 kept in 16.16 so the feedback does not stall at a DC
        int64_t y = ((int64_t)(x - c->hp_x1) * 65536) + c->hp_y1 - ((c->hp_y1 * 334) >> 16);
        c->hp_x1 = x;
        c->hp_y1 = y;
        y >>= 16;
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        mono[i] = (int16_t)y;
    }
}

} // namespace

extern "C" const struct lrv_opn_ops lrv_opn_ymfm_ops = {
    "ymfm", ymfm_create, ymfm_destroy, ymfm_reset, ymfm_write, ymfm_render, nullptr
};
