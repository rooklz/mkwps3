#pragma once
// psq_l / psq_st on the PS3: paired-single loads and stores through a GQR.
//
// Every entry point the translator emits reduces to one operation: move one
// or two values between guest memory and the two FPR lanes, as floats or as
// scaled 8/16-bit integers. The float form -- which is what the SDK leaves in
// GQR0, and so is nearly every psq in the game, including the prologue and
// epilogue saves of callee-saved FPRs -- is a pair of lfs/stfs and nothing
// else. Quantized forms go through the integer unit (an int<->float
// conversion has to on the PPE) and are kept out of line.
//
// Load quantization word: type = (gqr >> 16) & 7, scale = (gqr >> 24) & 63.
// Store quantization word: type = gqr & 7, scale = (gqr >> 8) & 63.
// Types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16; 1-3 reserved.

#include "ppc_isa_config.h"
#include "ppc_isa_float.h"
#include "../memory_access.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

extern "C" FprVal PPC_PsqL(uint32_t addr, uint32_t w, uint32_t i);
extern "C" void PPC_PsqSt(uint32_t addr, FprVal value, uint32_t w, uint32_t i);

namespace PsqDetail {

// 2^-scale for loads, 2^scale for stores; the 6-bit scale is signed.
inline float DequantFactor(uint32_t scale)
{
    return scale < 32u ? std::ldexp(1.0f, -static_cast<int>(scale))
                       : std::ldexp(1.0f, static_cast<int>(64u - scale));
}

inline float QuantFactor(uint32_t scale)
{
    return scale < 32u ? std::ldexp(1.0f, static_cast<int>(scale))
                       : std::ldexp(1.0f, -static_cast<int>(64u - scale));
}

template <typename T>
inline T QuantizeLane(double lane, uint32_t scale)
{
    float v = static_cast<float>(lane);
    if (scale != 0u)
        v *= QuantFactor(scale);
    constexpr float kMin = static_cast<float>(std::numeric_limits<T>::min());
    constexpr float kMax = static_cast<float>(std::numeric_limits<T>::max());
    if (!(v > kMin))            // also NaN
        return std::numeric_limits<T>::min();
    if (v >= kMax)
        return std::numeric_limits<T>::max();
    return static_cast<T>(v);
}

template <typename T>
inline double DequantizeLane(T v, uint32_t scale)
{
    const float f = static_cast<float>(v);
    return static_cast<double>(scale == 0u ? f : f * DequantFactor(scale));
}

// Host pointer for a psq access of `bytes`, or null for an MMIO or unbacked
// address (the checked path then handles it).
inline uint8_t* ReadHost(uint32_t addr, uint32_t bytes)
{
    (void)bytes;
    return MemoryInline::GuestHost(addr);
}

inline uint8_t* WriteHost(uint32_t addr, uint32_t bytes)
{
    (void)bytes;
    if (MemoryInline::FlatWriteNeedsPolicy(addr))
        return nullptr;
    return MemoryInline::GuestHost(addr);
}

template <typename T>
inline T HostRead(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <typename T>
inline void HostWrite(uint8_t* p, T v)
{
    std::memcpy(p, &v, sizeof(T));
}

// ---- float (type 0): lfs/stfs straight to and from the lanes -------------

template <uint32_t W>
inline FprVal LoadFloat(const uint8_t* p)
{
    if constexpr (W == 0u)
        return FprVal(MemoryInline::HostLoadFloat(p), MemoryInline::HostLoadFloat(p + 4));
    else
        return FprVal(MemoryInline::HostLoadFloat(p), 1.0);
}

template <uint32_t W>
inline void StoreFloat(uint8_t* p, FprVal v)
{
    MemoryInline::HostStoreSingle(p, v.ps0);
    if constexpr (W == 0u)
        MemoryInline::HostStoreSingle(p + 4, v.ps1);
}

// ---- quantized (types 4-7) ----------------------------------------------

template <uint32_t W, typename T>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline FprVal LoadQuantized(const uint8_t* p, uint32_t scale)
{
    const double ps0 = DequantizeLane(HostRead<T>(p), scale);
    if constexpr (W == 0u)
        return FprVal(ps0, DequantizeLane(HostRead<T>(p + sizeof(T)), scale));
    else
        return FprVal(ps0, 1.0);
}

template <uint32_t W, typename T>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void StoreQuantized(uint8_t* p, FprVal v, uint32_t scale)
{
    HostWrite<T>(p, QuantizeLane<T>(v.ps0, scale));
    if constexpr (W == 0u)
        HostWrite<T>(p + sizeof(T), QuantizeLane<T>(v.ps1, scale));
}

// Full dispatch on a load quantization type. `fallbackAddr` reaches the
// checked generic helper for a null host or a reserved type.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline FprVal LoadGeneric(uint32_t gqr, const uint8_t* p, uint32_t addr)
{
    if (!p)
        return PPC_PsqL(addr, W, I);
    const uint32_t scale = (gqr >> 24) & 0x3Fu;
    switch ((gqr >> 16) & 7u) {
    case 0u: return LoadFloat<W>(p);
    case 4u: return LoadQuantized<W, uint8_t>(p, scale);
    case 5u: return LoadQuantized<W, uint16_t>(p, scale);
    case 6u: return LoadQuantized<W, int8_t>(p, scale);
    case 7u: return LoadQuantized<W, int16_t>(p, scale);
    default: return PPC_PsqL(addr, W, I);
    }
}

template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void StoreGeneric(uint32_t gqr, uint8_t* p, uint32_t addr, FprVal v)
{
    if (!p) {
        PPC_PsqSt(addr, v, W, I);
        return;
    }
    const uint32_t scale = (gqr >> 8) & 0x3Fu;
    switch (gqr & 7u) {
    case 0u: StoreFloat<W>(p, v); return;
    case 4u: StoreQuantized<W, uint8_t>(p, v, scale); return;
    case 5u: StoreQuantized<W, uint16_t>(p, v, scale); return;
    case 6u: StoreQuantized<W, int8_t>(p, v, scale); return;
    case 7u: StoreQuantized<W, int16_t>(p, v, scale); return;
    default: PPC_PsqSt(addr, v, W, I); return;
    }
}

// The hot shape: a float GQR is one compare, then the lfs/stfs pair. GQR0 needs not even
// the compare: it is the compiler's -- CodeWarrior moves plain paired singles through it --
// and the OS sets it to 0 (unscaled float) in __PSInit and never changes it.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE FprVal Load(uint32_t gqr, uint8_t* p, uint32_t addr)
{
    if ((I == 0u || ((gqr >> 16) & 7u) == 0u) && p)
        return LoadFloat<W>(p);
    return LoadGeneric<W, I>(gqr, p, addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void Store(uint32_t gqr, uint8_t* p, uint32_t addr, FprVal v)
{
    if ((I == 0u || (gqr & 7u) == 0u) && p) {
        StoreFloat<W>(p, v);
        return;
    }
    StoreGeneric<W, I>(gqr, p, addr, v);
}

template <uint32_t W, uint32_t GQR>
MKW_PPC_FORCE_INLINE FprVal LoadKnown(uint8_t* p, uint32_t addr)
{
    constexpr uint32_t type = (GQR >> 16) & 7u;
    constexpr uint32_t scale = (GQR >> 24) & 0x3Fu;
    if (!p)
        return PPC_PsqL(addr, W, 0u);
    if constexpr (type == 0u) return LoadFloat<W>(p);
    else if constexpr (type == 4u) return LoadQuantized<W, uint8_t>(p, scale);
    else if constexpr (type == 5u) return LoadQuantized<W, uint16_t>(p, scale);
    else if constexpr (type == 6u) return LoadQuantized<W, int8_t>(p, scale);
    else if constexpr (type == 7u) return LoadQuantized<W, int16_t>(p, scale);
    else return PPC_PsqL(addr, W, 0u);
}

template <uint32_t W, uint32_t GQR>
MKW_PPC_FORCE_INLINE void StoreKnown(uint8_t* p, uint32_t addr, FprVal v)
{
    constexpr uint32_t type = GQR & 7u;
    constexpr uint32_t scale = (GQR >> 8) & 0x3Fu;
    if (!p) {
        PPC_PsqSt(addr, v, W, 0u);
        return;
    }
    if constexpr (type == 0u) StoreFloat<W>(p, v);
    else if constexpr (type == 4u) StoreQuantized<W, uint8_t>(p, v, scale);
    else if constexpr (type == 5u) StoreQuantized<W, uint16_t>(p, v, scale);
    else if constexpr (type == 6u) StoreQuantized<W, int8_t>(p, v, scale);
    else if constexpr (type == 7u) StoreQuantized<W, int16_t>(p, v, scale);
    else PPC_PsqSt(addr, v, W, 0u);
}

template <uint32_t W>
constexpr uint32_t Bytes() { return W == 0u ? 8u : 4u; }

} // namespace PsqDetail

// ---- entry points named by the translator --------------------------------

// GQR value passed in (hoisted by the translator out of ctx->gqr[I]).
template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLGqrInline(uint32_t gqr, uint32_t addr)
{
    return PsqDetail::Load<W, I>(gqr, PsqDetail::ReadHost(addr, PsqDetail::Bytes<W>()), addr);
}

template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLGqrInline(CpuContext*, uint32_t gqr, uint32_t addr)
{
    return PPC_PsqLGqrInline<W, I>(gqr, addr);
}

template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE void PPC_PsqStGqrInline(uint32_t gqr, uint32_t addr, FprVal value)
{
    PsqDetail::Store<W, I>(gqr, PsqDetail::WriteHost(addr, PsqDetail::Bytes<W>()), addr, value);
}

template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE void PPC_PsqStGqrInline(CpuContext*, uint32_t gqr, uint32_t addr, FprVal value)
{
    PPC_PsqStGqrInline<W, I>(gqr, addr, value);
}

// GQR read from the context.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLInline(CpuContext* cpu, uint32_t addr)
{
    return PPC_PsqLGqrInline<W, I>(cpu->gqr[I], addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLInline(uint32_t addr)
{
    return PPC_PsqLInline<W, I>(TryGetCpuContext(), addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStInline(CpuContext* cpu, uint32_t addr, FprVal value)
{
    PPC_PsqStGqrInline<W, I>(cpu->gqr[I], addr, value);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStInline(uint32_t addr, FprVal value)
{
    PPC_PsqStInline<W, I>(TryGetCpuContext(), addr, value);
}

// Stack forms: the address is the guest stack (always RAM).
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLStackInline(CpuContext* cpu, uint32_t addr)
{
    return PsqDetail::Load<W, I>(cpu->gqr[I], MemoryInline::GuestHost(addr), addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLStackInline(uint32_t addr)
{
    return PPC_PsqLStackInline<W, I>(TryGetCpuContext(), addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStStackInline(CpuContext* cpu, uint32_t addr, FprVal value)
{
    PsqDetail::Store<W, I>(cpu->gqr[I], MemoryInline::GuestHost(addr), addr, value);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStStackInline(uint32_t addr, FprVal value)
{
    PPC_PsqStStackInline<W, I>(TryGetCpuContext(), addr, value);
}

// GQR passed explicitly (state-free ABI); Stack selects the RAM-only form.
template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLStateInline(uint32_t gqr, uint32_t addr)
{
    uint8_t* p = Stack ? MemoryInline::GuestHost(addr) : PsqDetail::ReadHost(addr, PsqDetail::Bytes<W>());
    return PsqDetail::Load<W, I>(gqr, p, addr);
}

template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_FORCE_INLINE void PPC_PsqStStateInline(uint32_t gqr, uint32_t addr, FprVal value)
{
    uint8_t* p = Stack ? MemoryInline::GuestHost(addr) : PsqDetail::WriteHost(addr, PsqDetail::Bytes<W>());
    PsqDetail::Store<W, I>(gqr, p, addr, value);
}

// GQR known at translation time.
template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLKnownInline(CpuContext*, uint32_t addr)
{
    return PsqDetail::LoadKnown<W, GQR>(PsqDetail::ReadHost(addr, PsqDetail::Bytes<W>()), addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownInline(CpuContext*, uint32_t addr, FprVal value)
{
    PsqDetail::StoreKnown<W, GQR>(PsqDetail::WriteHost(addr, PsqDetail::Bytes<W>()), addr, value);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLKnownStackInline(CpuContext*, uint32_t addr)
{
    return PsqDetail::LoadKnown<W, GQR>(MemoryInline::GuestHost(addr), addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownStackInline(CpuContext*, uint32_t addr, FprVal value)
{
    PsqDetail::StoreKnown<W, GQR>(MemoryInline::GuestHost(addr), addr, value);
}

// Resolved-range forms: a host pointer the translator resolved for the whole
// range (null when the range was not plain RAM).
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                   uint32_t offset, uint32_t addr)
{
    uint8_t* p = resolvedHost ? resolvedHost + offset : nullptr;
    return PsqDetail::Load<W, I>(cpu->gqr[I], p, addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLKnownResolvedInline(CpuContext*, uint8_t* resolvedHost,
                                                        uint32_t offset, uint32_t addr)
{
    return PsqDetail::LoadKnown<W, GQR>(resolvedHost ? resolvedHost + offset : nullptr, addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                  uint32_t offset, uint32_t addr, FprVal value)
{
    uint8_t* p = resolvedHost ? resolvedHost + offset : nullptr;
    PsqDetail::Store<W, I>(cpu->gqr[I], p, addr, value);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownResolvedInline(CpuContext*, uint8_t* resolvedHost,
                                                       uint32_t offset, uint32_t addr, FprVal value)
{
    PsqDetail::StoreKnown<W, GQR>(resolvedHost ? resolvedHost + offset : nullptr, addr, value);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE FprVal PPC_PsqLResolvedStateInline(uint32_t gqr, uint8_t* resolvedHost,
                                                        uint32_t offset, uint32_t addr)
{
    return PsqDetail::Load<W, I>(gqr, resolvedHost ? resolvedHost + offset : nullptr, addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStResolvedStateInline(uint32_t gqr, uint8_t* resolvedHost,
                                                       uint32_t offset, uint32_t addr, FprVal value)
{
    PsqDetail::Store<W, I>(gqr, resolvedHost ? resolvedHost + offset : nullptr, addr, value);
}
