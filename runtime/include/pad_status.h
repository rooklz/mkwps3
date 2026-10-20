#pragma once
// The GameCube controller as the PAD library reports it: one status record per
// port, as PADRead fills them, and the four calls the PAD HLE makes. A host
// frontend implements the calls (platform/ps3/pad_ps3.cpp on the console).

#include <cstdint>

#define PAD_CHANMAX 4
#define PAD_CHAN0_BIT 0x80000000u

#define PAD_ERR_NONE 0
#define PAD_ERR_NO_CONTROLLER (-1)
#define PAD_ERR_NOT_READY (-2)
#define PAD_ERR_TRANSFER (-3)

#define PAD_BUTTON_LEFT 0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN 0x0004
#define PAD_BUTTON_UP 0x0008
#define PAD_TRIGGER_Z 0x0010
#define PAD_TRIGGER_R 0x0020
#define PAD_TRIGGER_L 0x0040
#define PAD_BUTTON_A 0x0100
#define PAD_BUTTON_B 0x0200
#define PAD_BUTTON_X 0x0400
#define PAD_BUTTON_Y 0x0800
#define PAD_BUTTON_START 0x1000

struct PADStatus {
    uint16_t button;
    int8_t stickX;
    int8_t stickY;
    int8_t substickX;
    int8_t substickY;
    uint8_t triggerL;
    uint8_t triggerR;
    uint8_t analogA;
    uint8_t analogB;
    int8_t err;
};

bool PADInit();
// Fills PAD_CHANMAX records; returns the rumble mask the host wants applied.
uint32_t PADRead(PADStatus* status);
bool PADReset(uint32_t mask);
bool PADRecalibrate(uint32_t mask);
void PADControlMotor(uint32_t chan, uint32_t command);
