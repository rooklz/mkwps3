// NHTTPStartup reentrancy wrapper (0x801D8D30).
//
// Split out of hle/net/network_config.cpp so it is built on every platform:
// the translator generates its dispatch tables against this registration, so
// a build that drops the socket layer must keep it. It only touches guest
// memory and the guest register file.

#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <cstdint>

namespace NetworkHle {

extern "C" void func_801D8D30(CpuContext* ctx);
extern "C" void func_801D9E94(CpuContext* ctx);

static uint32_t GetNHttpSystemInfo(CpuContext* ctx, uint32_t& savedR3, uint32_t& savedR4, uint32_t& savedR5) {
    savedR3 = ctx->gpr[3];
    savedR4 = ctx->gpr[4];
    savedR5 = ctx->gpr[5];
    func_801D9E94(ctx);
    const uint32_t systemInfo = ctx->gpr[3];
    ctx->gpr[3] = savedR3;
    ctx->gpr[4] = savedR4;
    ctx->gpr[5] = savedR5;
    return systemInfo;
}

extern "C" void NHTTPStartup_Reentrant_HLE_801d8d30(CpuContext* ctx) {
    if (!ctx) {
        return;
    }

    uint32_t savedR3 = 0;
    uint32_t savedR4 = 0;
    uint32_t savedR5 = 0;
    const uint32_t systemInfo = GetNHttpSystemInfo(ctx, savedR3, savedR4, savedR5);
    if (Memory::Contains(systemInfo + 1996u, 4) && Memory::Read32(systemInfo + 1996u) != 0) {
        // ponytail: NHTTP stays active across DWC auth/GHTTP handoffs, but the
        // allocator callbacks are caller-owned and can outlive the auth object.
        if (Memory::Contains(systemInfo + 1988u, 8)) {
            const uint32_t oldAlloc = Memory::Read32(systemInfo + 1988u);
            const uint32_t oldFree = Memory::Read32(systemInfo + 1992u);
            if (oldAlloc != savedR3 || oldFree != savedR4) {
                Memory::Write32(systemInfo + 1988u, savedR3);
                Memory::Write32(systemInfo + 1992u, savedR4);
            }
        }
        ctx->gpr[3] = 0;
        return;
    }

    ctx->gpr[3] = savedR3;
    ctx->gpr[4] = savedR4;
    ctx->gpr[5] = savedR5;
    func_801D8D30(ctx);

    const int32_t result = static_cast<int32_t>(ctx->gpr[3]);
    if (result < 0) {
        uint32_t retryR3 = 0;
        uint32_t retryR4 = 0;
        uint32_t retryR5 = 0;
        const uint32_t retrySystemInfo = GetNHttpSystemInfo(ctx, retryR3, retryR4, retryR5);
        const bool becameActive =
            Memory::Contains(retrySystemInfo + 1996u, 4) && Memory::Read32(retrySystemInfo + 1996u) != 0;
        if (becameActive) {
            ctx->gpr[3] = 0;
        }
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x801D8D30, NHTTPStartup_Reentrant_HLE_801d8d30, "NHTTPStartup_Reentrant_HLE_801d8d30");

} // namespace NetworkHle
