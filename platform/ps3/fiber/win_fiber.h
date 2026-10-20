#pragma once
// The Windows fiber API the runtime's guest-thread scheduler is written
// against (fiber_manager.cpp), implemented on the PS3 with a cooperative
// context switch (fiber_ps3.S). Every guest thread is a fiber on the one game
// PPU thread, as on the Wii's single CPU.

#include <cstddef>
#include <cstdint>

#ifndef CALLBACK
#define CALLBACK
#endif

typedef void (*LPFIBER_START_ROUTINE)(void* parameter);

void* ConvertThreadToFiber(void* parameter);
void* CreateFiber(size_t stackSize, LPFIBER_START_ROUTINE start, void* parameter);
void SwitchToFiber(void* fiber);
void DeleteFiber(void* fiber);
void* GetCurrentFiber();
void* GetFiberData();
bool ConvertFiberToThread();

typedef uint32_t DWORD;
inline DWORD GetLastError() { return 0; }
