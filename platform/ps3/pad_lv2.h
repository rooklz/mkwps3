#pragma once
// Raw DualShock 3 state (pad_lv2.cpp), free of SDK typedefs.

#include <stdint.h>

#define MKW_PAD_PORTS 4

enum {
    MKW_PAD_LEFT = 1u << 0,
    MKW_PAD_RIGHT = 1u << 1,
    MKW_PAD_UP = 1u << 2,
    MKW_PAD_DOWN = 1u << 3,
    MKW_PAD_START = 1u << 4,
    MKW_PAD_SELECT = 1u << 5,
    MKW_PAD_CROSS = 1u << 6,
    MKW_PAD_CIRCLE = 1u << 7,
    MKW_PAD_SQUARE = 1u << 8,
    MKW_PAD_TRIANGLE = 1u << 9,
    MKW_PAD_L1 = 1u << 10,
    MKW_PAD_R1 = 1u << 11,
    MKW_PAD_L2 = 1u << 12,
    MKW_PAD_R2 = 1u << 13,
    MKW_PAD_L3 = 1u << 14,
    MKW_PAD_R3 = 1u << 15,
};

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;   // 0..255, centre 128, y grows downward
} MkwPadRaw;

#ifdef __cplusplus
extern "C" {
#endif
int mkw_pad_init(void);
int mkw_pad_poll(unsigned port, MkwPadRaw* out);   // 1 when a pad is connected
#ifdef __cplusplus
}
#endif
