// DualShock 3 polling through libio. Kept apart from pad_ps3.cpp because the
// PSL1GHT and dolphin SDK headers disagree on the s32/u32 typedefs.

#include "pad_lv2.h"

#include <cstring>
#include <io/pad.h>

namespace {
bool g_ready = false;
padData g_last[MKW_PAD_PORTS];
}

extern "C" int mkw_pad_init(void)
{
    if (!g_ready)
        g_ready = ioPadInit(MKW_PAD_PORTS) == 0;
    std::memset(g_last, 0, sizeof g_last);
    return g_ready ? 1 : 0;
}

extern "C" int mkw_pad_poll(unsigned port, MkwPadRaw* out)
{
    std::memset(out, 0, sizeof *out);
    if (!g_ready || port >= MKW_PAD_PORTS)
        return 0;
    padInfo info;
    if (ioPadGetInfo(&info) != 0 || !info.status[port])
        return 0;
    padData data;
    if (ioPadGetData(port, &data) == 0 && data.len > 0)
        g_last[port] = data;
    const padData& d = g_last[port];
    if (d.len == 0) {
        out->lx = out->ly = out->rx = out->ry = 128;
        return 1;
    }
    uint32_t b = 0;
    b |= d.BTN_LEFT ? MKW_PAD_LEFT : 0;
    b |= d.BTN_RIGHT ? MKW_PAD_RIGHT : 0;
    b |= d.BTN_UP ? MKW_PAD_UP : 0;
    b |= d.BTN_DOWN ? MKW_PAD_DOWN : 0;
    b |= d.BTN_START ? MKW_PAD_START : 0;
    b |= d.BTN_SELECT ? MKW_PAD_SELECT : 0;
    b |= d.BTN_CROSS ? MKW_PAD_CROSS : 0;
    b |= d.BTN_CIRCLE ? MKW_PAD_CIRCLE : 0;
    b |= d.BTN_SQUARE ? MKW_PAD_SQUARE : 0;
    b |= d.BTN_TRIANGLE ? MKW_PAD_TRIANGLE : 0;
    b |= d.BTN_L1 ? MKW_PAD_L1 : 0;
    b |= d.BTN_R1 ? MKW_PAD_R1 : 0;
    b |= d.BTN_L2 ? MKW_PAD_L2 : 0;
    b |= d.BTN_R2 ? MKW_PAD_R2 : 0;
    b |= d.BTN_L3 ? MKW_PAD_L3 : 0;
    b |= d.BTN_R3 ? MKW_PAD_R3 : 0;
    out->buttons = b;
    out->lx = static_cast<uint8_t>(d.ANA_L_H);
    out->ly = static_cast<uint8_t>(d.ANA_L_V);
    out->rx = static_cast<uint8_t>(d.ANA_R_H);
    out->ry = static_cast<uint8_t>(d.ANA_R_V);
    return 1;
}
