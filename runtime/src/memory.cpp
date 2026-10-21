// Guest memory: region setup, the checked slow paths and deferred reads (PS3).
//
// Upstream's Windows build maps a 4 GiB flat view, keeps page tables for the
// checked accessors and resolves MMIO and deferred-copy reads in a vectored
// fault handler. The PS3 arena (guest_flat_memory.h) makes backing a matter of
// a few constant compares, and there is no fault to hook, so:
//   - a gather-pipe store reaches the GX pipe; every other MMIO access goes to
//     the register shadow the GX front end keeps (gx_hw.h), since the game's
//     own GX library programs CP/PE/PI here;
//   - a read or write of unbacked memory is counted and answered with zero /
//     dropped, as upstream's on-demand commit would leave it;
//   - deferred reads materialise only through the checked API (HLE code); the
//     renderer is responsible for copies the CPU reads with plain loads.

#include "memory.h"
#include "gx_hw.h"
#include "slow_path_census.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "guest_flat_memory.h"
#include "recomp_mod_loader.h"

namespace GuestFlat {
void NoteUnmapped() noexcept;
}

// ---- checked fallbacks for resolved ranges --------------------------------

template <typename T>
T MemoryInline::ReadResolvedFallback(uint32_t addr)
{
    MKW_SLOW_NOTE(kMkwSlowResolvedFallback);
    if constexpr (sizeof(T) == 1) return Memory::Read8(addr);
    if constexpr (sizeof(T) == 2) return Memory::Read16(addr);
    if constexpr (sizeof(T) == 4) return Memory::Read32(addr);
    return Memory::Read64(addr);
}
template uint8_t MemoryInline::ReadResolvedFallback<uint8_t>(uint32_t);
template uint16_t MemoryInline::ReadResolvedFallback<uint16_t>(uint32_t);
template uint32_t MemoryInline::ReadResolvedFallback<uint32_t>(uint32_t);
template uint64_t MemoryInline::ReadResolvedFallback<uint64_t>(uint32_t);
float MemoryInline::ReadResolvedFallbackFloat32(uint32_t addr) { return Memory::ReadFloat32(addr); }
double MemoryInline::ReadResolvedFallbackFloat64(uint32_t addr) { return Memory::ReadFloat64(addr); }

template <typename T>
void MemoryInline::WriteResolvedFallback(uint32_t addr, T value)
{
    MKW_SLOW_NOTE(kMkwSlowResolvedFallback);
    if constexpr (sizeof(T) == 1) Memory::Write8(addr, value);
    else if constexpr (sizeof(T) == 2) Memory::Write16(addr, value);
    else if constexpr (sizeof(T) == 4) Memory::Write32(addr, value);
    else Memory::Write64(addr, value);
}
template void MemoryInline::WriteResolvedFallback<uint8_t>(uint32_t, uint8_t);
template void MemoryInline::WriteResolvedFallback<uint16_t>(uint32_t, uint16_t);
template void MemoryInline::WriteResolvedFallback<uint32_t>(uint32_t, uint32_t);
template void MemoryInline::WriteResolvedFallback<uint64_t>(uint32_t, uint64_t);
void MemoryInline::WriteResolvedFallbackFloat32(uint32_t addr, double val) { Memory::WriteFloat32(addr, val); }
void MemoryInline::WriteResolvedFallbackFloat64(uint32_t addr, double val) { Memory::WriteFloat64(addr, val); }

// ---- access violations ----------------------------------------------------

Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error(std::string(reason)), address_(address), length_(length), reason_(reason)
{
}

// ---- deferred reads -------------------------------------------------------

namespace {

struct DeferredRead {
    uint64_t token;
    uint32_t start;
    size_t length;
    Memory::DeferredReadCallback callback;
    void* user;
};

std::mutex& DeferredReadMutex()
{
    static std::mutex m;
    return m;
}

std::vector<DeferredRead>& DeferredReads()
{
    static std::vector<DeferredRead> reads;
    return reads;
}

uint64_t g_nextDeferredToken = 1;

bool RangesOverlap(uint32_t a, size_t alen, uint32_t b, size_t blen)
{
    const uint64_t a0 = a, a1 = a0 + alen;
    const uint64_t b0 = b, b1 = b0 + blen;
    return a0 < b1 && b0 < a1;
}

std::vector<Memory::RegionConfig>& ConfiguredRegions()
{
    static std::vector<Memory::RegionConfig> regions;
    return regions;
}

unsigned g_unmappedReports = 0;

void ReportUnmapped(const char* what, uint32_t addr)
{
    GuestFlat::NoteUnmapped();
    if (g_unmappedReports++ < 16)
        std::printf("[mem] %s of unbacked guest address %08x (active=0x%08X)\n", what, addr,
                    RecompMod::CurrentTranslatedExecutionAddress());
}

template <typename T>
T ReadScalar(uint32_t addr)
{
    if (!GuestFlat::RangeBacked(addr, sizeof(T))) {
        ReportUnmapped("read", addr);
        return T{};
    }
    T v;
    std::memcpy(&v, MemoryInline::GuestHost(addr), sizeof(T));
    return v;
}

template <typename T>
void WriteScalar(uint32_t addr, T v)
{
    if (!GuestFlat::RangeBacked(addr, sizeof(T))) {
        ReportUnmapped("write", addr);
        return;
    }
    std::memcpy(MemoryInline::GuestHost(addr), &v, sizeof(T));
}

} // namespace

uint64_t Memory::RegisterDeferredRead(uint32_t addr, size_t length, DeferredReadCallback callback, void* user)
{
    if (length == 0 || callback == nullptr)
        return 0;
    std::lock_guard<std::mutex> lock(DeferredReadMutex());
    uint64_t token = g_nextDeferredToken++;
    DeferredReads().push_back({token, addr, length, callback, user});
    GuestFlat::ProtectDeferredRange(addr, length);
    return token;
}

void Memory::ClearDeferredReads()
{
    std::lock_guard<std::mutex> lock(DeferredReadMutex());
    DeferredReads().clear();
}

bool MemoryInline::ResolveDeferredReads(uint32_t addr, size_t length)
{
    std::vector<DeferredRead> matches;
    {
        std::lock_guard<std::mutex> lock(DeferredReadMutex());
        auto& reads = DeferredReads();
        for (auto it = reads.begin(); it != reads.end();) {
            if (RangesOverlap(addr, length, it->start, it->length)) {
                matches.push_back(*it);
                it = reads.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& match : matches)
        match.callback(match.user);
    return !matches.empty();
}

// ---- regions --------------------------------------------------------------

Memory::Config Memory::Config::WiiDefaults()
{
    Config config;
    config.regions.push_back(RegionConfig{"MEM1_PHYS", kMem1PhysicalBase, kMem1Size});
    config.regions.push_back(RegionConfig{"MEM1", kMem1CachedBase, kMem1Size});
    config.regions.push_back(RegionConfig{"MEM1_UNCACHED", kMem1UncachedBase, kMem1Size});
    config.regions.push_back(RegionConfig{"MEM2_PHYS", kMem2PhysicalBase, kMem2Size});
    config.regions.push_back(RegionConfig{"MEM2", kMem2CachedBase, kMem2Size});
    config.regions.push_back(RegionConfig{"MEM2_UNCACHED", kMem2UncachedBase, kMem2Size});
    config.regions.push_back(RegionConfig{"LOCKED_CACHE", 0xE0000000u, 16u * 1024u});
    return config;
}

void Memory::Init(const Config& config)
{
    std::vector<GuestFlat::RegionRequest> requests;
    for (const auto& r : config.regions)
        requests.push_back(GuestFlat::RegionRequest{r.baseAddress, r.sizeBytes, GuestFlat::Backing::Owned});
    GuestFlat::Initialize(requests);
    ConfiguredRegions() = config.regions;
}

void Memory::Init(size_t)
{
    Init(Config::WiiDefaults());
}

void Memory::Reset()
{
    ClearDeferredReads();
}

void Memory::RefreshWritableFastPathsForExecutableRanges() {}

uint8_t* Memory::GetPointer(uint32_t addr)
{
    return GetPointer(addr, 1);
}

uint8_t* Memory::GetPointer(uint32_t addr, size_t length)
{
    if (!GuestFlat::RangeBacked(addr, length))
        throw AccessViolation(addr, length, "no mapped region");
    return MemoryInline::GuestHost(addr);
}

bool Memory::Contains(uint32_t addr, size_t length)
{
    return GuestFlat::RangeBacked(addr, length);
}

std::vector<Memory::RegionConfig> Memory::DescribeRegions()
{
    return ConfiguredRegions();
}

// ---- slow paths -----------------------------------------------------------

uint8_t MemoryInline::Read8Slow(uint32_t addr)
{
    if (IsMmioAddress(addr)) {
        MKW_SLOW_NOTE(kMkwSlowReadMmio);
        return static_cast<uint8_t>(GxHw::MmioRead(addr, 1));
    }
    MKW_SLOW_NOTE(kMkwSlowReadRam);
    ResolveDeferredReads(addr, 1);
    return ReadScalar<uint8_t>(addr);
}

uint16_t MemoryInline::Read16Slow(uint32_t addr)
{
    if (IsMmioAddress(addr)) {
        MKW_SLOW_NOTE(kMkwSlowReadMmio);
        return static_cast<uint16_t>(GxHw::MmioRead(addr, 2));
    }
    MKW_SLOW_NOTE(kMkwSlowReadRam);
    ResolveDeferredReads(addr, 2);
    return ReadScalar<uint16_t>(addr);
}

uint32_t MemoryInline::Read32Slow(uint32_t addr)
{
    if (IsMmioAddress(addr)) {
        MKW_SLOW_NOTE(kMkwSlowReadMmio);
        return static_cast<uint32_t>(GxHw::MmioRead(addr, 4));
    }
    MKW_SLOW_NOTE(kMkwSlowReadRam);
    ResolveDeferredReads(addr, 4);
    return ReadScalar<uint32_t>(addr);
}

uint64_t MemoryInline::Read64Slow(uint32_t addr)
{
    if (IsMmioAddress(addr)) {
        MKW_SLOW_NOTE(kMkwSlowReadMmio);
        return static_cast<uint64_t>(GxHw::MmioRead(addr, 8));
    }
    MKW_SLOW_NOTE(kMkwSlowReadRam);
    ResolveDeferredReads(addr, 8);
    return ReadScalar<uint64_t>(addr);
}

float MemoryInline::ReadFloat32Slow(uint32_t addr)
{
    const uint32_t bits = Read32Slow(addr);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

double MemoryInline::ReadFloat64Slow(uint32_t addr)
{
    const uint64_t bits = Read64Slow(addr);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

void MemoryInline::Write8Slow(uint32_t addr, uint8_t val)
{
    MKW_SLOW_NOTE(IsGpuFifoAddress(addr) ? kMkwSlowWriteFifo : IsMmioAddress(addr) ? kMkwSlowWriteMmio : kMkwSlowWriteRam);
    if (IsGpuFifoAddress(addr)) { GX_HLE_FIFO_Write8(val); return; }
    if (IsMmioAddress(addr)) { GxHw::MmioWrite(addr, val, 1); return; }
    WriteScalar(addr, val);
}

void MemoryInline::Write16Slow(uint32_t addr, uint16_t val)
{
    MKW_SLOW_NOTE(IsGpuFifoAddress(addr) ? kMkwSlowWriteFifo : IsMmioAddress(addr) ? kMkwSlowWriteMmio : kMkwSlowWriteRam);
    if (IsGpuFifoAddress(addr)) { GX_HLE_FIFO_Write16(val); return; }
    if (IsMmioAddress(addr)) { GxHw::MmioWrite(addr, val, 2); return; }
    WriteScalar(addr, val);
}

void MemoryInline::Write32Slow(uint32_t addr, uint32_t val)
{
    MKW_SLOW_NOTE(IsGpuFifoAddress(addr) ? kMkwSlowWriteFifo : IsMmioAddress(addr) ? kMkwSlowWriteMmio : kMkwSlowWriteRam);
    if (IsGpuFifoAddress(addr)) { GX_HLE_FIFO_Write32(val); return; }
    if (IsMmioAddress(addr)) { GxHw::MmioWrite(addr, val, 4); return; }
    WriteScalar(addr, val);
}

void MemoryInline::Write64Slow(uint32_t addr, uint64_t val)
{
    MKW_SLOW_NOTE(IsGpuFifoAddress(addr) ? kMkwSlowWriteFifo : IsMmioAddress(addr) ? kMkwSlowWriteMmio : kMkwSlowWriteRam);
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val >> 32));
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val));
        return;
    }
    if (IsMmioAddress(addr)) { GxHw::MmioWrite(addr, val, 8); return; }
    WriteScalar(addr, val);
}

void MemoryInline::WriteFloat32Slow(uint32_t addr, double val)
{
    const uint32_t bits = ConvertPpcDoubleToSingleBits(val);
    MKW_SLOW_NOTE(IsGpuFifoAddress(addr) ? kMkwSlowWriteFifo : IsMmioAddress(addr) ? kMkwSlowWriteMmio : kMkwSlowWriteRam);
    if (IsGpuFifoAddress(addr)) { GX_HLE_FIFO_WriteFloat(PpcSingleBitsToFloat(bits)); return; }
    if (IsMmioAddress(addr)) { GxHw::MmioWrite(addr, bits, 4); return; }
    WriteScalar(addr, bits);
}

void MemoryInline::WriteFloat64Slow(uint32_t addr, double val)
{
    uint64_t bits;
    std::memcpy(&bits, &val, 8);
    MKW_SLOW_NOTE(IsGpuFifoAddress(addr) ? kMkwSlowWriteFifo : IsMmioAddress(addr) ? kMkwSlowWriteMmio : kMkwSlowWriteRam);
    if (IsGpuFifoAddress(addr)) { GX_HLE_FIFO_WriteFloat(static_cast<float>(val)); return; }
    if (IsMmioAddress(addr)) { GxHw::MmioWrite(addr, bits, 8); return; }
    WriteScalar(addr, bits);
}
