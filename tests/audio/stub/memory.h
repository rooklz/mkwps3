// Just enough of runtime/include/memory.h for ax_internal.h on a host compiler.
#pragma once
#include <cstdint>
#include <cstddef>
namespace Memory {
constexpr size_t kMem1Size = 0x01800000;
constexpr size_t kMem2Size = 0x04000000;
constexpr uint32_t kMem1CachedBase = 0x80000000u, kMem1UncachedBase = 0xC0000000u;
constexpr uint32_t kMem2PhysicalBase = 0x10000000u, kMem2CachedBase = 0x90000000u, kMem2UncachedBase = 0xD0000000u;
inline uint16_t Read16(uint32_t) { return 0; }
inline uint32_t Read32(uint32_t) { return 0; }
inline void Write16(uint32_t, uint16_t) {}
inline void Write32(uint32_t, uint32_t) {}
inline bool Contains(uint32_t, size_t) { return false; }
template <typename T> inline T Read(uint32_t) { return T{}; }
}
namespace MemoryInline {
inline uint16_t ByteSwap16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t ByteSwap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint8_t* GetPointerFast(uint32_t) { return nullptr; }
}
