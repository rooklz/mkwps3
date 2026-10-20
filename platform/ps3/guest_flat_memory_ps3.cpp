// Guest memory on the PS3: the 1 GiB arena (see guest_flat_memory.h).
//
// lv2 separates reserving address space from allocating memory:
//   sysMMapperAllocateAddress  reserves a virtual range,
//   sysMMapperAllocateMemory   creates a physical memory object,
//   sysMMapperMapMemory        binds an object into the reservation.
// Anything in the reservation never bound stays unmapped and faults. Every
// region is backed with 1 MiB pages: the PPE's TLB is small, and lv2 refuses
// to map a 64 KiB-page object into a 1 MiB-page reservation anyway.

#include "guest_flat_memory.h"

#include <cstdio>
#include <cstring>
#include <ppu-types.h>
#include <sys/memory.h>

uint8_t* g_mkwGuestArena = nullptr;

extern "C" uint8_t* MkwGuestArenaBase()
{
    return g_mkwGuestArena;
}

namespace GuestFlat {
namespace {

constexpr uint32_t kPageGranule = 0x100000u;
bool g_active = false;
FaultCounters g_counters;

struct Backed {
    uint32_t foldBase;
    uint32_t bytes;
};
Backed g_backed[16];
unsigned g_backedCount = 0;

uint32_t RoundToPage(uint64_t size)
{
    return static_cast<uint32_t>((size + kPageGranule - 1u) & ~static_cast<uint64_t>(kPageGranule - 1u));
}

bool AlreadyBacked(uint32_t foldBase)
{
    for (unsigned i = 0; i < g_backedCount; ++i)
        if (g_backed[i].foldBase == foldBase)
            return true;
    return false;
}

bool Reserve()
{
    // lv2 hands out address space in 256 MiB units and only aligns to that
    // unit or its multiples; asking for 1 MiB alignment is rejected outright.
    static const size_t kAlignments[] = {0x40000000u, 0x10000000u, 0u};
    for (size_t align : kAlignments) {
        sys_mem_addr_t addr = 0;
        if (sysMMapperAllocateAddress(kArenaSize, SYS_MEMORY_PAGE_SIZE_1M, align, &addr) == 0) {
            g_mkwGuestArena = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(addr));
            std::printf("[mem] arena: 1024 MiB reserved at %08x (alignment %u MiB)\n",
                        static_cast<unsigned>(addr), static_cast<unsigned>(align >> 20));
            return true;
        }
    }
    std::printf("[mem] arena reservation failed\n");
    return false;
}

bool Back(uint32_t foldBase, uint64_t size, const char* name)
{
    if (AlreadyBacked(foldBase))
        return true;
    if (g_backedCount >= sizeof g_backed / sizeof g_backed[0])
        return false;
    const uint32_t bytes = RoundToPage(size);
    sys_mem_id_t id = 0;
    if (sysMMapperAllocateMemory(bytes, SYS_MEMORY_PAGE_SIZE_1M, &id) != 0) {
        std::printf("[mem] %s: %u KiB allocation failed\n", name, bytes >> 10);
        return false;
    }
    const sys_mem_addr_t at =
        static_cast<sys_mem_addr_t>(reinterpret_cast<uintptr_t>(g_mkwGuestArena)) + foldBase;
    if (sysMMapperMapMemory(at, id, SYS_MEMORY_PROT_READ_WRITE) != 0) {
        std::printf("[mem] %s: map at %08x failed\n", name, static_cast<unsigned>(at));
        sysMMapperFreeMemory(id);
        return false;
    }
    std::memset(g_mkwGuestArena + foldBase, 0, bytes);
    g_backed[g_backedCount++] = Backed{foldBase, bytes};
    std::printf("[mem] %-12s %6u KiB at fold %08x\n", name, bytes >> 10, foldBase);
    return true;
}

} // namespace

bool BackShadow(uint32_t foldBase, uint32_t bytes, const char* name)
{
    return g_active && Back(foldBase, bytes, name);
}

bool IsActive()
{
    return g_active;
}

void Initialize(const std::vector<RegionRequest>& regions)
{
    if (g_active)
        return;
    if (!Reserve())
        return;
    // The mirrors fold onto the same offset; each distinct range is backed once.
    for (const RegionRequest& r : regions)
        Back(r.base & kFoldMask, r.size, "region");
    g_active = true;
    // The calling thread goes on to run translated static constructors.
    MkwPinArenaOnThisThread();
}

uint8_t* HostPointer(uint32_t guestAddress)
{
    return RangeBacked(guestAddress, 1) ? g_mkwGuestArena + (guestAddress & kFoldMask) : nullptr;
}

void ProtectDeferredRange(uint32_t, size_t) { ++g_counters.efb; }
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}

FaultCounters Counters()
{
    return g_counters;
}

void LogFaultSummary() noexcept
{
    if (g_counters.unmapped)
        std::printf("[mem] %u guest accesses touched unbacked memory\n", g_counters.unmapped);
}

void NoteUnmapped() noexcept
{
    ++g_counters.unmapped;
}

} // namespace GuestFlat
