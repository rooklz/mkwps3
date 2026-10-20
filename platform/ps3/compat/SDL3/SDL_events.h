#pragma once
// SDL is not used on the PS3. aurora's public headers name a few SDL types in
// signatures the port never calls; these opaque declarations let them parse.
#include <stdint.h>
typedef uint32_t SDL_JoystickID;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_IOStream SDL_IOStream;
typedef union SDL_Event {
    uint32_t type;
    uint8_t padding[128];
} SDL_Event;
