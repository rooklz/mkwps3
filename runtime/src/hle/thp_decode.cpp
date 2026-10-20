// Native THPVideoDecode (THPDec). As translated code the decoder takes most of
// the game thread while a THP movie plays; ThpCore produces the same tiled
// Y/U/V output (see thp_decode_core.h for the derived semantics).

#include "hle_stubs.h"
#include "memory.h"
#include "abi_bridge.h"
#include "ppc_runtime.h"
#include "gx_hw.h"
#include "runtime_log.h"
#include "generated/RuntimeConfig.h"
#include "hle/thp_decode_core.h"

#include <cstdint>
#include <iostream>

extern "C" uint32_t PPCMfhid2_8012e630_impl();

namespace {

// THPVideoDecode status codes for its argument and environment checks.
constexpr int32_t kThpErrNoInput = 25;
constexpr int32_t kThpErrNoWork = 26;
constexpr int32_t kThpErrNoOutput = 27;
constexpr int32_t kThpErrLockedCacheOff = 28;
constexpr int32_t kThpErrNotInitialized = 29;

constexpr uint32_t kHid2LockedCacheEnable = 0x10000000u;
// __THPInitFlag (r13-25088), set by THPInit.
constexpr uint32_t kThpInitFlagAddr = RuntimeConfig::SDA1_BASE - 25088u;
// Upper bound for the readable span after the frame pointer; THP frames are
// tens of KB.
constexpr uint32_t kMaxFrameSpan = 16u << 20;

// Game thread only. THPDec's own state (file info and Huffman scratch in the
// work buffer, its r13 statics, the locked-cache bands) is only read by THPDec
// functions this override replaces, so it is not reproduced in guest memory.
ThpCore::Frame g_frame;
ThpCore::Workspace g_work;
uint32_t g_failureLogs = 0;
uint32_t g_loggedSize = 0; // width << 16 | height of the last movie logged

// Largest n <= limit with [addr, addr + n) backed.
uint32_t BackedSpan(uint32_t addr, uint32_t limit)
{
    if (!Memory::Contains(addr, 1))
        return 0;
    if (Memory::Contains(addr, limit))
        return limit;
    uint32_t lo = 1, hi = limit;
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (Memory::Contains(addr, mid))
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

void LogFailure(const char* what, uint32_t value)
{
    if (g_failureLogs >= 8)
        return;
    ++g_failureLogs;
    RT_LOG(RT_TAG_HLE) << "THPVideoDecode: " << what << " 0x" << std::hex << value << std::dec << std::endl;
}

} // namespace

extern "C" int32_t THPVideoDecode_HLE(uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work)
{
    if (file == 0)
        return kThpErrNoInput;
    if (tileY == 0 || tileU == 0 || tileV == 0)
        return kThpErrNoOutput;
    if (work == 0)
        return kThpErrNoWork;
    if ((PPCMfhid2_8012e630_impl() & kHid2LockedCacheEnable) == 0)
        return kThpErrLockedCacheOff;
    if (Memory::Read32(kThpInitFlagAddr) == 0)
        return kThpErrNotInitialized;

    const uint32_t span = BackedSpan(file, kMaxFrameSpan);
    if (span == 0) {
        LogFailure("frame not in guest memory at", file);
        return ThpCore::kErrSyntax;
    }
    const uint8_t* src = Memory::GetPointer(file, span);
    const uint8_t* end = src + span;
    const int32_t status = ThpCore::ParseHeaders(src, end, g_frame);
    if (status != ThpCore::kOk)
        return status;

    const uint32_t size = (static_cast<uint32_t>(g_frame.width) << 16) | g_frame.height;
    if (size != g_loggedSize) {
        g_loggedSize = size;
        RT_LOG(RT_TAG_HLE) << "THPVideoDecode: native decode " << g_frame.width << "x" << g_frame.height
                           << (THP_CORE_VMX ? " (VMX)" : "") << std::endl;
    }

    const uint32_t bands = ThpCore::BandCount(g_frame);
    const uint32_t sizeY = bands * ThpCore::LumaBandBytes(g_frame);
    const uint32_t sizeC = bands * ThpCore::ChromaBandBytes(g_frame);
    if (sizeY == 0 && sizeC == 0)
        return ThpCore::kOk;
    if (!Memory::Contains(tileY, sizeY) || !Memory::Contains(tileU, sizeC) || !Memory::Contains(tileV, sizeC)) {
        LogFailure("output planes not in guest memory, Y at", tileY);
        return ThpCore::kOk;
    }
    uint8_t* y = Memory::GetPointer(tileY, sizeY);
    uint8_t* u = Memory::GetPointer(tileU, sizeC);
    uint8_t* v = Memory::GetPointer(tileV, sizeC);
    if (!ThpCore::DecodeScan(g_frame, end, g_work, y, u, v)) {
        LogFailure("needs staging but too wide, width", g_frame.width);
        return ThpCore::kOk;
    }

    // THPDec delivers each band with LCStoreData, a DMA write with no flush
    // after it, so tell the renderer the planes changed.
    GxHw::NotifyGuestRamWrite(tileY, sizeY);
    GxHw::NotifyGuestRamWrite(tileU, sizeC);
    GxHw::NotifyGuestRamWrite(tileV, sizeC);
    return ThpCore::kOk;
}

PPC_NATIVE_OVERRIDE(801B3BAC, THPVideoDecode_HLE, int32_t,
                    (uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work),
                    (file, tileY, tileU, tileV, work));
