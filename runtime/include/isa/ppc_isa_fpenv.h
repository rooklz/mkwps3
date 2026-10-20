#pragma once
// FPSCR[NI] (non-IEEE mode) on the PS3.
//
// Gekko's NI flushes single-precision denormals. The PPE implements the same
// architected FPSCR[NI] bit (bit 29), so the guest's mode is applied to the host
// FPSCR directly and the hardware flushes exactly where Gekko does; upstream's
// x86 build approximates this with MXCSR FTZ/DAZ. The mirror below is what the
// few software paths (raw lane moves, stores) consult instead of reading FPSCR.

#include "ppc_isa_config.h"

#include <cstdint>

inline bool g_mkwHostNiActive = false;

inline void MkwSetHostNiBit(bool on) noexcept
{
    if (on)
        __asm__ volatile("mtfsb1 29" ::: "memory");
    else
        __asm__ volatile("mtfsb0 29" ::: "memory");
}

inline void MkwApplyHostNiMode(uint32_t fpscr) noexcept
{
    const bool wantNi = (fpscr & 0x4u) != 0;
    if (wantNi != g_mkwHostNiActive) {
        MkwSetHostNiBit(wantNi);
        g_mkwHostNiActive = wantNi;
    }
}

// Saved host FP state for CpuContextScope: the NI bit is the only part the
// runtime changes.
inline uint32_t MkwCaptureHostFpState() noexcept
{
    return g_mkwHostNiActive ? 1u : 0u;
}

inline void MkwRestoreHostFpState(uint32_t state) noexcept
{
    const bool on = state != 0;
    if (on != g_mkwHostNiActive) {
        MkwSetHostNiBit(on);
        g_mkwHostNiActive = on;
    }
}
