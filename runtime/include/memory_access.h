#pragma once
// Translated-code memory access on the PS3. See guest_flat_memory.h for the
// arena. Every fast path here is one fold (rldicl) plus one load or store;
// stores that might reach MMIO add a mask/compare in front. Floats move
// between guest memory and FPRs with lfs/lfd/stfs/stfd directly: going through
// an integer register would be a load-hit-store on the Cell PPE.

#include "guest_flat_memory.h"
#include "memory.h"
#include "recomp_mod_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gx_pipe.h"

namespace MemoryInline {
#define MKW_MEMORY_FORCE_INLINE inline __attribute__((always_inline))
#define MKW_MEMORY_NO_INLINE __attribute__((noinline))
#define MKW_MEMORY_COLD __attribute__((cold))

inline constexpr uint32_t kPageShift = 20;
inline constexpr uint32_t kPageSize = 1u << kPageShift;
inline constexpr uint32_t kMaxFastScalarSize = 8;

MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint8_t Read8Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint16_t Read16Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint32_t Read32Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint64_t Read64Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD float ReadFloat32Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD double ReadFloat64Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write8Slow(uint32_t addr, uint8_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write16Slow(uint32_t addr, uint16_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write32Slow(uint32_t addr, uint32_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write64Slow(uint32_t addr, uint64_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteFloat32Slow(uint32_t addr, double val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteFloat64Slow(uint32_t addr, double val);
template <typename T>
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD T ReadResolvedFallback(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD float ReadResolvedFallbackFloat32(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD double ReadResolvedFallbackFloat64(uint32_t addr);
template <typename T>
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteResolvedFallback(uint32_t addr, T value);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteResolvedFallbackFloat32(uint32_t addr, double val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteResolvedFallbackFloat64(uint32_t addr, double val);
bool ResolveDeferredReads(uint32_t addr, size_t length);

constexpr bool IsMmioAddress(uint32_t addr)
{
    return addr >= 0xCC000000u && addr < 0xCE000000u;
}

constexpr bool IsGpuFifoAddress(uint32_t addr)
{
    return addr >= 0xCC008000u && addr < 0xCC008100u;
}

// An MMIO write must reach its device (the GX FIFO above all) with its value.
MKW_MEMORY_FORCE_INLINE bool FlatWriteNeedsPolicy(uint32_t address)
{
    return (address & 0xFE000000u) == 0xCC000000u;  // 0xCC000000..0xCDFFFFFF
}

// Guest byte order to host byte order. Upstream names these after what they
// do on its little-endian hosts; every caller uses them to read guest words.
// The PS3 is big-endian like the Wii, so the conversion is the identity.
inline uint16_t ByteSwap16(uint16_t value) { return value; }
inline uint32_t ByteSwap32(uint32_t value) { return value; }
inline uint64_t ByteSwap64(uint64_t value) { return value; }
template <typename T>
inline T MaybeByteSwap(T value) { return value; }

// The folded offset as a 64-bit index (one rldicl, no second zero-extension).
MKW_MEMORY_FORCE_INLINE uint64_t Fold(uint32_t address)
{
    return static_cast<uint64_t>(address) & 0x3FFFFFFFull;
}

MKW_MEMORY_FORCE_INLINE uint8_t* GuestHost(uint32_t address)
{
    uint8_t* const p = g_mkwGuestArena + Fold(address);
    // The arena is never at address zero. Without this the compiler keeps a
    // null test (cmpdi + branch) on every paired-single access, whose helpers
    // also accept the null a failed range resolution produces.
    if (!p)
        __builtin_unreachable();
    return p;
}

// The architected stfs conversion: Gekko's and the PPE's are the same, so
// this exists only for code that needs the bits rather than a store.
MKW_MEMORY_FORCE_INLINE uint32_t ConvertPpcDoubleToSingleBits(double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = static_cast<uint32_t>((bits >> 52) & 0x7FFu);
    if (exponent - 874u <= 22u) {
        uint32_t narrowed = static_cast<uint32_t>(0x80000000ULL | ((bits & 0x000FFFFFFFFFFFFFULL) >> 21));
        narrowed >>= (905u - exponent);
        narrowed |= static_cast<uint32_t>((bits >> 32) & 0x80000000ULL);
        return narrowed;
    }
    return static_cast<uint32_t>(((bits >> 32) & 0xC0000000ULL) | ((bits >> 29) & 0x3FFFFFFFULL));
}

MKW_MEMORY_FORCE_INLINE float PpcSingleBitsToFloat(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- FPR <-> memory, never through a GPR ---------------------------------

MKW_MEMORY_FORCE_INLINE float HostLoadFloat(const uint8_t* p)
{
    float v;
    __asm__("lfs%U1%X1 %0,%1" : "=f"(v) : "m"(*reinterpret_cast<const float*>(p)));
    return v;
}

MKW_MEMORY_FORCE_INLINE double HostLoadDouble(const uint8_t* p)
{
    double v;
    __asm__("lfd%U1%X1 %0,%1" : "=f"(v) : "m"(*reinterpret_cast<const double*>(p)));
    return v;
}

// stfs: the architected double->single store conversion, which does not round.
MKW_MEMORY_FORCE_INLINE void HostStoreSingle(uint8_t* p, double v)
{
    __asm__("stfs%U0%X0 %1,%0" : "=m"(*reinterpret_cast<float*>(p)) : "f"(v));
}

MKW_MEMORY_FORCE_INLINE void HostStoreDouble(uint8_t* p, double v)
{
    __asm__("stfd%U0%X0 %1,%0" : "=m"(*reinterpret_cast<double*>(p)) : "f"(v));
}

// ---- the arena helpers the runtime (HLE) uses ----------------------------

MKW_MEMORY_FORCE_INLINE bool TryGetPointerFast(uint32_t address, size_t length, uint8_t*& pointer)
{
    if (!GuestFlat::RangeBacked(address, length)) {
        pointer = nullptr;
        return false;
    }
    pointer = GuestHost(address);
    return true;
}

inline uint8_t* GetPointerFast(uint32_t address, size_t length)
{
    uint8_t* pointer = nullptr;
    return TryGetPointerFast(address, length, pointer) ? pointer : nullptr;
}

MKW_MEMORY_FORCE_INLINE bool TryGetWritablePointerFast(uint32_t address, size_t length, uint8_t*& pointer)
{
    if (FlatWriteNeedsPolicy(address))
        return pointer = nullptr, false;
    return TryGetPointerFast(address, length, pointer);
}

// A range the translator proved contiguous is resolved once; every access in
// it is then host + offset. Null (unbacked, or a write touching MMIO) sends
// each access down the checked path instead.
MKW_MEMORY_FORCE_INLINE uint8_t* ResolveRangeHost(uint32_t base, int32_t minOffset, uint32_t length,
                                                 bool needsRead, bool needsWrite)
{
    (void)needsRead;
    (void)needsWrite;
    // The common case in two compares: the whole range inside MEM1 or MEM2.
    // `length` is a constant at every call site. No separate MMIO test is
    // needed: the register blocks fold to 0x0C000000/0x0D000000, outside both
    // regions, so a range touching them falls through to the checked path.
    const uint32_t fold = (base + static_cast<uint32_t>(minOffset)) & GuestFlat::kFoldMask;
    if (length != 0 && length <= kPageSize &&
        (fold + length <= GuestFlat::kMem1Bytes ||
         fold - GuestFlat::kMem2FoldBase + length <= GuestFlat::kMem2Bytes)) {
        uint8_t* const p = g_mkwGuestArena + fold;
        if (!p)
            __builtin_unreachable();
        return p;
    }
    return nullptr;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool ReadResolvedScalar(uint8_t* host, uint32_t rangeOffset, T& outValue)
{
    if (!host)
        return false;
    std::memcpy(&outValue, host + rangeOffset, sizeof(T));
    return true;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool WriteResolvedScalar(uint8_t* host, uint32_t rangeOffset, T value)
{
    if (!host)
        return false;
    std::memcpy(host + rangeOffset, &value, sizeof(T));
    return true;
}

struct ResolvedLoadPair {
    uint32_t first = 0;
    uint32_t second = 0;
    bool valid = false;
};

MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair16(uint8_t* host, uint32_t rangeOffset)
{
    uint32_t packed = 0;
    if (!ReadResolvedScalar(host, rangeOffset, packed))
        return {};
    return {packed >> 16, packed & 0xFFFFu, true};
}

MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair32(uint8_t* host, uint32_t rangeOffset)
{
    uint64_t packed = 0;
    if (!ReadResolvedScalar(host, rangeOffset, packed))
        return {};
    return {static_cast<uint32_t>(packed >> 32), static_cast<uint32_t>(packed), true};
}

MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair16(uint8_t* host, uint32_t rangeOffset, uint32_t packed)
{
    return WriteResolvedScalar(host, rangeOffset, packed);
}

MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair32(uint8_t* host, uint32_t rangeOffset, uint64_t packed)
{
    return WriteResolvedScalar(host, rangeOffset, packed);
}

template <typename T>
inline bool TryReadMappedScalar(uint32_t address, T& outValue)
{
    uint8_t* ptr = nullptr;
    if (!TryGetPointerFast(address, sizeof(T), ptr))
        return false;
    std::memcpy(&outValue, ptr, sizeof(T));
    return true;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool TryReadGuestScalar(uint32_t address, T& outValue)
{
    return TryReadMappedScalar(address, outValue);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool TryWriteGuestScalar(uint32_t address, T value)
{
    uint8_t* ptr = nullptr;
    if (!TryGetWritablePointerFast(address, sizeof(T), ptr))
        return false;
    std::memcpy(ptr, &value, sizeof(T));
    return true;
}

template <typename T>
inline bool WriteStackScalarFast(uint32_t address, T value)
{
    std::memcpy(GuestHost(address), &value, sizeof(T));
    return true;
}

inline uint8_t ReadStack8(uint32_t address) { return *GuestHost(address); }
inline uint16_t ReadStack16(uint32_t address) { uint16_t v; std::memcpy(&v, GuestHost(address), 2); return v; }
inline uint32_t ReadStack32(uint32_t address) { uint32_t v; std::memcpy(&v, GuestHost(address), 4); return v; }
inline uint64_t ReadStack64(uint32_t address) { uint64_t v; std::memcpy(&v, GuestHost(address), 8); return v; }
inline void WriteStack8(uint32_t address, uint8_t value) { WriteStackScalarFast(address, value); }
inline void WriteStack16(uint32_t address, uint16_t value) { WriteStackScalarFast(address, value); }
inline void WriteStack32(uint32_t address, uint32_t value) { WriteStackScalarFast(address, value); }
inline void WriteStack64(uint32_t address, uint64_t value) { WriteStackScalarFast(address, value); }
inline void WriteStackFloat32(uint32_t address, double value) { HostStoreSingle(GuestHost(address), value); }
inline void WriteStackFloat64(uint32_t address, double value) { HostStoreDouble(GuestHost(address), value); }

// ---- resolved-range accessors (generated code) ---------------------------

MKW_MEMORY_FORCE_INLINE uint8_t ReadResolved8(uint8_t* r, uint32_t o, uint32_t a)
{
    if (r) return r[o];
    return ReadResolvedFallback<uint8_t>(a);
}
MKW_MEMORY_FORCE_INLINE uint16_t ReadResolved16(uint8_t* r, uint32_t o, uint32_t a)
{
    uint16_t v;
    if (ReadResolvedScalar(r, o, v)) return v;
    return ReadResolvedFallback<uint16_t>(a);
}
MKW_MEMORY_FORCE_INLINE uint32_t ReadResolved32(uint8_t* r, uint32_t o, uint32_t a)
{
    uint32_t v;
    if (ReadResolvedScalar(r, o, v)) return v;
    return ReadResolvedFallback<uint32_t>(a);
}
MKW_MEMORY_FORCE_INLINE uint64_t ReadResolved64(uint8_t* r, uint32_t o, uint32_t a)
{
    uint64_t v;
    if (ReadResolvedScalar(r, o, v)) return v;
    return ReadResolvedFallback<uint64_t>(a);
}
MKW_MEMORY_FORCE_INLINE float ReadResolvedFloat32(uint8_t* r, uint32_t o, uint32_t a)
{
    if (r) return HostLoadFloat(r + o);
    return ReadResolvedFallbackFloat32(a);
}
MKW_MEMORY_FORCE_INLINE double ReadResolvedFloat64(uint8_t* r, uint32_t o, uint32_t a)
{
    if (r) return HostLoadDouble(r + o);
    return ReadResolvedFallbackFloat64(a);
}

MKW_MEMORY_FORCE_INLINE void WriteResolved8(uint8_t* r, uint32_t o, uint32_t a, uint8_t v)
{
    if (r) { r[o] = v; return; }
    WriteResolvedFallback<uint8_t>(a, v);
}
MKW_MEMORY_FORCE_INLINE void WriteResolved16(uint8_t* r, uint32_t o, uint32_t a, uint16_t v)
{
    if (WriteResolvedScalar(r, o, v)) return;
    WriteResolvedFallback<uint16_t>(a, v);
}
MKW_MEMORY_FORCE_INLINE void WriteResolved32(uint8_t* r, uint32_t o, uint32_t a, uint32_t v)
{
    if (WriteResolvedScalar(r, o, v)) return;
    WriteResolvedFallback<uint32_t>(a, v);
}
MKW_MEMORY_FORCE_INLINE void WriteResolved64(uint8_t* r, uint32_t o, uint32_t a, uint64_t v)
{
    if (WriteResolvedScalar(r, o, v)) return;
    WriteResolvedFallback<uint64_t>(a, v);
}
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat32(uint8_t* r, uint32_t o, uint32_t a, double v)
{
    if (r) { HostStoreSingle(r + o, v); return; }
    WriteResolvedFallbackFloat32(a, v);
}
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat64(uint8_t* r, uint32_t o, uint32_t a, double v)
{
    if (r) { HostStoreDouble(r + o, v); return; }
    WriteResolvedFallbackFloat64(a, v);
}

// ---- flat accessors (generated code; the hottest paths in the port) ------

template <typename T>
MKW_MEMORY_FORCE_INLINE T FlatLoad(uint32_t address)
{
    T value;
    std::memcpy(&value, GuestHost(address), sizeof(T));
    return value;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE void FlatStore(uint32_t address, T value)
{
    std::memcpy(GuestHost(address), &value, sizeof(T));
}

MKW_MEMORY_FORCE_INLINE uint8_t FlatRead8(uint32_t address) { return FlatLoad<uint8_t>(address); }
MKW_MEMORY_FORCE_INLINE uint16_t FlatRead16(uint32_t address) { return FlatLoad<uint16_t>(address); }
MKW_MEMORY_FORCE_INLINE uint32_t FlatRead32(uint32_t address) { return FlatLoad<uint32_t>(address); }
MKW_MEMORY_FORCE_INLINE uint64_t FlatRead64(uint32_t address) { return FlatLoad<uint64_t>(address); }
MKW_MEMORY_FORCE_INLINE float FlatReadFloat32(uint32_t address) { return HostLoadFloat(GuestHost(address)); }
MKW_MEMORY_FORCE_INLINE double FlatReadFloat64(uint32_t address) { return HostLoadDouble(GuestHost(address)); }

MKW_MEMORY_FORCE_INLINE void FlatWrite8(uint32_t address, uint8_t value)
{
    if (FlatWriteNeedsPolicy(address)) { Write8Slow(address, value); return; }
    FlatStore<uint8_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite16(uint32_t address, uint16_t value)
{
    if (FlatWriteNeedsPolicy(address)) { Write16Slow(address, value); return; }
    FlatStore<uint16_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite32(uint32_t address, uint32_t value)
{
    if (FlatWriteNeedsPolicy(address)) { Write32Slow(address, value); return; }
    FlatStore<uint32_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite64(uint32_t address, uint64_t value)
{
    if (FlatWriteNeedsPolicy(address)) { Write64Slow(address, value); return; }
    FlatStore<uint64_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteFloat32(uint32_t address, double value)
{
    if (FlatWriteNeedsPolicy(address)) { WriteFloat32Slow(address, value); return; }
    HostStoreSingle(GuestHost(address), value);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteFloat64(uint32_t address, double value)
{
    if (FlatWriteNeedsPolicy(address)) { WriteFloat64Slow(address, value); return; }
    HostStoreDouble(GuestHost(address), value);
}

// Stores the translator proved hit ordinary RAM (stack slots): no MMIO test.
MKW_MEMORY_FORCE_INLINE void FlatWriteRam8(uint32_t address, uint8_t value) { FlatStore<uint8_t>(address, value); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam16(uint32_t address, uint16_t value) { FlatStore<uint16_t>(address, value); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam32(uint32_t address, uint32_t value) { FlatStore<uint32_t>(address, value); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam64(uint32_t address, uint64_t value) { FlatStore<uint64_t>(address, value); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat32(uint32_t address, double value) { HostStoreSingle(GuestHost(address), value); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat64(uint32_t address, double value) { HostStoreDouble(GuestHost(address), value); }

} // namespace MemoryInline

#undef MKW_MEMORY_FORCE_INLINE
#undef MKW_MEMORY_NO_INLINE
#undef MKW_MEMORY_COLD

// ---- the public scalar API (HLE code) -------------------------------------

inline uint8_t Memory::Read8(uint32_t addr)
{
    uint8_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value))
        return MemoryInline::Read8Slow(addr);
    return value;
}

inline uint16_t Memory::Read16(uint32_t addr)
{
    uint16_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value))
        return MemoryInline::Read16Slow(addr);
    return value;
}

inline uint32_t Memory::Read32(uint32_t addr)
{
    uint32_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value))
        return MemoryInline::Read32Slow(addr);
    return value;
}

inline uint64_t Memory::Read64(uint32_t addr)
{
    uint64_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value))
        return MemoryInline::Read64Slow(addr);
    return value;
}

inline float Memory::ReadFloat32(uint32_t addr)
{
    uint8_t* p = nullptr;
    if (!MemoryInline::TryGetPointerFast(addr, 4, p))
        return MemoryInline::ReadFloat32Slow(addr);
    return MemoryInline::HostLoadFloat(p);
}

inline double Memory::ReadFloat64(uint32_t addr)
{
    uint8_t* p = nullptr;
    if (!MemoryInline::TryGetPointerFast(addr, 8, p))
        return MemoryInline::ReadFloat64Slow(addr);
    return MemoryInline::HostLoadDouble(p);
}

inline void Memory::Write8(uint32_t addr, uint8_t val)
{
    if (!MemoryInline::TryWriteGuestScalar(addr, val))
        MemoryInline::Write8Slow(addr, val);
}

inline void Memory::Write16(uint32_t addr, uint16_t val)
{
    if (!MemoryInline::TryWriteGuestScalar(addr, val))
        MemoryInline::Write16Slow(addr, val);
}

inline void Memory::Write32(uint32_t addr, uint32_t val)
{
    if (!MemoryInline::TryWriteGuestScalar(addr, val))
        MemoryInline::Write32Slow(addr, val);
}

inline void Memory::Write64(uint32_t addr, uint64_t val)
{
    if (!MemoryInline::TryWriteGuestScalar(addr, val))
        MemoryInline::Write64Slow(addr, val);
}

inline void Memory::WriteFloat32(uint32_t addr, double val)
{
    uint8_t* p = nullptr;
    if (MemoryInline::TryGetWritablePointerFast(addr, 4, p)) {
        MemoryInline::HostStoreSingle(p, val);
        return;
    }
    MemoryInline::WriteFloat32Slow(addr, val);
}

inline void Memory::WriteFloat64(uint32_t addr, double val)
{
    uint8_t* p = nullptr;
    if (MemoryInline::TryGetWritablePointerFast(addr, 8, p)) {
        MemoryInline::HostStoreDouble(p, val);
        return;
    }
    MemoryInline::WriteFloat64Slow(addr, val);
}
