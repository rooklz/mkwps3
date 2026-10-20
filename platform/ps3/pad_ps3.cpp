// The GameCube PAD API the PAD HLE calls (upstream gets it from aurora),
// mapped from a DualShock 3:
//
//   Cross A   Square B   Circle X   Triangle Y   Start START   Select Z
//   R1/R2 R   L1/L2 L    D-pad D-pad   left stick main stick   right stick C-stick
//
// Mario Kart Wii's GameCube layout: A accelerate, B brake, R drift, L item,
// D-pad up trick.

#include "pad_status.h"

#include "pad_lv2.h"

#include <cstdio>
#include <cstring>

namespace {

int8_t Axis(uint8_t raw, bool invert)
{
    // GC sticks sit within about +-100 of centre.
    int v = (static_cast<int>(raw) - 128) * 100 / 127;
    if (invert)
        v = -v;
    if (v > 100) v = 100;
    if (v < -100) v = -100;
    if (v > -8 && v < 8) v = 0;   // dead zone
    return static_cast<int8_t>(v);
}

} // namespace

// Remote input from the control port (`pad <gc buttons hex> <reads> [stick x y]`):
// merged into port 0 for the next <reads> PADRead calls, so menus can be
// driven without anyone at the console. Port 0 reports connected while
// injecting even with no pad attached.
namespace {
volatile uint32_t g_injectButtons = 0;
volatile int g_injectReads = 0;
volatile int g_injectStickX = 0;
volatile int g_injectStickY = 0;
// Once anything is injected, port 0 stays connected (neutral) when no real pad
// is there, so the game sees one stable controller.
volatile bool g_virtualPad = false;
uint32_t g_lastConnected = ~0u;
} // namespace

extern "C" void mkw_pad_inject(uint32_t buttons, int reads, int stickX, int stickY)
{
    g_injectButtons = buttons;
    g_injectStickX = stickX;
    g_injectStickY = stickY;
    g_injectReads = reads;
    g_virtualPad = true;
}

bool PADInit()
{
    mkw_pad_init();
    return true;
}

uint32_t PADRead(PADStatus* status)
{
    uint32_t connected = 0;
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        PADStatus& s = status[port];
        std::memset(&s, 0, sizeof s);
        MkwPadRaw raw;
        const bool injecting = port == 0 && (g_injectReads > 0 || g_virtualPad);
        if (!mkw_pad_poll(port, &raw)) {
            if (!injecting) {
                s.err = PAD_ERR_NO_CONTROLLER;
                continue;
            }
            std::memset(&raw, 0, sizeof raw);
            raw.lx = raw.ly = raw.rx = raw.ry = 128;
        }
        const uint32_t b = raw.buttons;
        uint16_t gc = 0;
        gc |= (b & MKW_PAD_LEFT) ? PAD_BUTTON_LEFT : 0;
        gc |= (b & MKW_PAD_RIGHT) ? PAD_BUTTON_RIGHT : 0;
        gc |= (b & MKW_PAD_UP) ? PAD_BUTTON_UP : 0;
        gc |= (b & MKW_PAD_DOWN) ? PAD_BUTTON_DOWN : 0;
        gc |= (b & MKW_PAD_CROSS) ? PAD_BUTTON_A : 0;
        gc |= (b & MKW_PAD_SQUARE) ? PAD_BUTTON_B : 0;
        gc |= (b & MKW_PAD_CIRCLE) ? PAD_BUTTON_X : 0;
        gc |= (b & MKW_PAD_TRIANGLE) ? PAD_BUTTON_Y : 0;
        gc |= (b & MKW_PAD_START) ? PAD_BUTTON_START : 0;
        gc |= (b & MKW_PAD_SELECT) ? PAD_TRIGGER_Z : 0;
        const bool r = (b & (MKW_PAD_R1 | MKW_PAD_R2)) != 0;
        const bool l = (b & (MKW_PAD_L1 | MKW_PAD_L2)) != 0;
        gc |= r ? PAD_TRIGGER_R : 0;
        gc |= l ? PAD_TRIGGER_L : 0;
        s.button = gc;
        s.stickX = Axis(raw.lx, false);
        s.stickY = Axis(raw.ly, true);
        s.substickX = Axis(raw.rx, false);
        s.substickY = Axis(raw.ry, true);
        s.triggerL = l ? 255 : 0;
        s.triggerR = r ? 255 : 0;
        s.analogA = (gc & PAD_BUTTON_A) ? 255 : 0;
        s.analogB = (gc & PAD_BUTTON_B) ? 255 : 0;
        if (injecting && g_injectReads > 0) {
            s.button |= static_cast<uint16_t>(g_injectButtons);
            if (g_injectStickX || g_injectStickY) {
                s.stickX = static_cast<int8_t>(g_injectStickX);
                s.stickY = static_cast<int8_t>(g_injectStickY);
            }
            if (s.button & PAD_BUTTON_A)
                s.analogA = 255;
            g_injectReads = g_injectReads - 1;
        }
        s.err = PAD_ERR_NONE;
        connected |= PAD_CHAN0_BIT >> port;
    }
    if (connected != g_lastConnected) {
        g_lastConnected = connected;
        std::printf("[pad] connected ports mask %08x%s\n", static_cast<unsigned>(connected),
                    g_virtualPad ? " (virtual pad on port 1)" : "");
    }
    return connected;
}

bool PADReset(uint32_t)
{
    return true;
}

bool PADRecalibrate(uint32_t)
{
    return true;
}

void PADControlMotor(uint32_t, uint32_t)
{
}
