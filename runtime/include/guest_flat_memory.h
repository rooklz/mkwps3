#pragma once

// Guest memory on the PS3.
//
// Upstream reserves a 4 GiB host range so a guest address is a host offset,
// and relies on page protections and a fault handler for MMIO, EFB reads and
// stray pointers. A PS3 process cannot reserve 4 GiB, and a fault there is not
// recoverable (the thread restarts the faulting access forever). So:
//
//   * a 1 GiB arena is reserved with sys_mmapper and the guest address folds
//     into it with `addr & 0x3FFFFFFF`. The Wii's cached (0x8xxxxxxx,
//     0x9xxxxxxx) and uncached (0xCxxxxxxx, 0xDxxxxxxx) mirrors of MEM1 and
//     MEM2 land on the same bytes, exactly as on the hardware;
//   * MEM1, MEM2, the locked cache and a page behind each MMIO block are
//     backed; nothing else is. Guest code under the full HLE never reads MMIO
//     (upstream treats such a read as fatal), and MMIO writes are caught by a
//     test in front of every checked store;
//   * the host is big-endian, so a guest access is a plain load or store.
//
// The arena base lives in r14 inside translated code (-ffixed-r14), saving
// a TOC load per access; runtime code sees it as an ordinary global. All code
// is built with -ffixed-r14 so nothing else ever uses the register.

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(MKW_PIN_ARENA)
register uint8_t* g_mkwGuestArena asm("r14");
#else
extern uint8_t* g_mkwGuestArena;
#endif

// Initialise r14 on the calling thread (and in every new fiber context).
extern "C" uint8_t* MkwGuestArenaBase();

// Loads the arena base into r14 on the calling thread. Everything is built with
// -ffixed-r14, so once set it stays set; a thread that runs translated code, or
// runtime code built with MKW_PIN_ARENA, calls this before its first access.
inline void MkwPinArenaOnThisThread()
{
    __asm__ volatile("mr 14, %0" : : "r"(MkwGuestArenaBase()));
}

namespace GuestFlat {

inline constexpr uint64_t kArenaSize = 0x40000000ull;     // 1 GiB of address space
inline constexpr uint32_t kFoldMask = 0x3FFFFFFFu;

enum class Backing {
    Owned,
    Mem1,
    Mem2,
};

struct RegionRequest {
    uint32_t base = 0;
    uint64_t size = 0;
    Backing backing = Backing::Owned;
};

struct FaultCounters {
    uint32_t mmio = 0;
    uint32_t efb = 0;
    uint32_t xguard = 0;
    uint32_t unmapped = 0;
    uint32_t unmappedRegions = 0;
};

bool IsActive();

// Reserves the arena and backs every requested region (by folded offset).
void Initialize(const std::vector<RegionRequest>& regions);

// Host pointer for a guest address whose folded offset is backed, or nullptr.
uint8_t* HostPointer(uint32_t guestAddress);

// Backs a device-register shadow at `foldBase` (gx_hw.cpp). It is outside
// every RangeBacked region, so checked accesses still take the MMIO path.
bool BackShadow(uint32_t foldBase, uint32_t bytes, const char* name);

// The backed regions, by folded offset. Fixed, so membership is a few
// compares rather than a table walk.
inline constexpr uint32_t kMem1FoldBase = 0x00000000u;
inline constexpr uint32_t kMem1Bytes = 24u * 1024u * 1024u;
inline constexpr uint32_t kMem2FoldBase = 0x10000000u;
inline constexpr uint32_t kMem2Bytes = 64u * 1024u * 1024u;
inline constexpr uint32_t kLockedCacheFoldBase = 0x20000000u;   // 0xE0000000 folded
inline constexpr uint32_t kLockedCacheBytes = 16u * 1024u;

// True when [address, address+length) lies inside one backed region.
inline bool RangeBacked(uint32_t address, size_t length)
{
    const uint32_t f0 = address & kFoldMask;
    if (length == 0)
        return false;
    const uint32_t f1 = f0 + static_cast<uint32_t>(length - 1);
    if (f1 < f0)
        return false;
    if (f1 < kMem1FoldBase + kMem1Bytes)
        return true;
    if (f0 >= kMem2FoldBase && f1 < kMem2FoldBase + kMem2Bytes)
        return true;
    return f0 >= kLockedCacheFoldBase && f1 < kLockedCacheFoldBase + kLockedCacheBytes;
}

// Deferred EFB reads have no fault to hang off on the PS3; these record the
// range for the renderer to materialise eagerly.
void ProtectDeferredRange(uint32_t address, size_t length);
void UnprotectDeferredRange(uint32_t address, size_t length);
void RegisterExecutableRange(uint32_t start, uint32_t end);

FaultCounters Counters();
void LogFaultSummary() noexcept;
void NoteUnmapped() noexcept;

} // namespace GuestFlat

#define MKW_FLAT_GUEST_BASE g_mkwGuestArena
