#pragma once
// PS3 (Cell PPE, GCC 7, big-endian PPC64) build configuration for the translated
// code and the runtime. Upstream targets x86-64 Clang; nothing here may pull in
// x86 intrinsics.

#include <atomic>
#include <cstdint>

#define MKW_RESTRICT __restrict

inline constexpr bool MkwStateFreeAbiEnabled(uint32_t) noexcept
{
    return true;
}

#define MKW_PPC_FORCE_INLINE inline __attribute__((always_inline))
#define MKW_PPC_NO_INLINE __attribute__((noinline))
#define MKW_PPC_ALWAYS_INLINE_BODY __attribute__((always_inline))
#define MKW_PPC_COLD __attribute__((cold))
// Upstream uses __regcall to pass more arguments in registers; the PPC64 ABI
// already passes eight integer and thirteen floating-point arguments in registers.
#define MKW_PPC_INTERNAL_CALL

// Two 64-bit results of a state-free translated function. Upstream uses a
// Clang ext_vector_type. Under the PPC64 ELFv1 ABI any aggregate or 16-byte
// vector comes back through memory, which the PPE pays for with a
// load-hit-store stall per call; a 128-bit integer is returned in r3:r4.
typedef unsigned __int128 MkwStateFreeResult2;

MKW_PPC_FORCE_INLINE MkwStateFreeResult2 MkwStateFreePack2(uint64_t first, uint64_t second)
{
    return (static_cast<MkwStateFreeResult2>(second) << 64) | first;
}

MKW_PPC_FORCE_INLINE uint64_t MkwStateFreeGet2(MkwStateFreeResult2 pair, int index)
{
    return static_cast<uint64_t>(pair >> (64 * index));
}

#if !defined(__GNUC__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "This runtime is the big-endian PS3 port; build upstream WiiCompiled for other hosts."
#endif
