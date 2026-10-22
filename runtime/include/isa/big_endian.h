#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace BigEndian {

// A big-endian host (the PS3) reads and writes guest bytes as they are: one
// load or store, and a float straight to or from an FPR. Through an integer
// register, a float costs the PPE a load-hit-store each way, and GCC takes
// that route for a memcpy'd float, so the PPU gets lfs and stfs spelt out.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline constexpr bool kHostIsBigEndian = true;
#else
inline constexpr bool kHostIsBigEndian = false;
#endif

inline uint16_t Read16(const uint8_t* src) {
    if constexpr (kHostIsBigEndian) {
        uint16_t value;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }
    return (static_cast<uint16_t>(src[0]) << 8) |
           static_cast<uint16_t>(src[1]);
}

inline uint32_t Read32(const uint8_t* src) {
    if constexpr (kHostIsBigEndian) {
        uint32_t value;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

inline float ReadFloat32(const uint8_t* src) {
    float value = 0.0f;
#if defined(__PPU__)
    // An lfs of its own: GCC moves a memcpy'd float through a GPR and the stack.
    __asm__("lfs%U1%X1 %0,%1" : "=f"(value) : "m"(*reinterpret_cast<const float*>(src)));
    return value;
#endif
    if constexpr (kHostIsBigEndian) {
        std::memcpy(&value, src, sizeof(value));
        return value;
    }
    const uint32_t bits = Read32(src);
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void Write16(uint8_t* dst, uint16_t value) {
    if constexpr (kHostIsBigEndian) {
        std::memcpy(dst, &value, sizeof(value));
        return;
    }
    dst[0] = static_cast<uint8_t>(value >> 8);
    dst[1] = static_cast<uint8_t>(value);
}

inline void Write32(uint8_t* dst, uint32_t value) {
    if constexpr (kHostIsBigEndian) {
        std::memcpy(dst, &value, sizeof(value));
        return;
    }
    dst[0] = static_cast<uint8_t>(value >> 24);
    dst[1] = static_cast<uint8_t>(value >> 16);
    dst[2] = static_cast<uint8_t>(value >> 8);
    dst[3] = static_cast<uint8_t>(value);
}

inline void Write64(uint8_t* dst, uint64_t value) {
    Write32(dst, static_cast<uint32_t>(value >> 32));
    Write32(dst + sizeof(uint32_t), static_cast<uint32_t>(value));
}

inline void WriteFloat32(uint8_t* dst, float value) {
#if defined(__PPU__)
    __asm__("stfs%U0%X0 %1,%0" : "=m"(*reinterpret_cast<float*>(dst)) : "f"(value));
    return;
#endif
    if constexpr (kHostIsBigEndian) {
        std::memcpy(dst, &value, sizeof(value));
        return;
    }
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    Write32(dst, bits);
}

inline void Write16(uint8_t* dst, std::size_t offset, uint16_t value) {
    Write16(dst + offset, value);
}

inline void Write32(uint8_t* dst, std::size_t offset, uint32_t value) {
    Write32(dst + offset, value);
}

inline void Write64(uint8_t* dst, std::size_t offset, uint64_t value) {
    Write64(dst + offset, value);
}

inline void WriteFloat32(uint8_t* dst, std::size_t offset, float value) {
    WriteFloat32(dst + offset, value);
}

template <typename Offset>
inline void Append16(uint8_t* dst, Offset& offset, uint16_t value) {
    static_assert(std::is_integral_v<Offset>);
    Write16(dst + offset, value);
    offset += static_cast<Offset>(sizeof(value));
}

template <typename Offset>
inline void Append32(uint8_t* dst, Offset& offset, uint32_t value) {
    static_assert(std::is_integral_v<Offset>);
    Write32(dst + offset, value);
    offset += static_cast<Offset>(sizeof(value));
}

template <typename Offset>
inline void AppendFloat32(uint8_t* dst, Offset& offset, float value) {
    static_assert(std::is_integral_v<Offset>);
    WriteFloat32(dst + offset, value);
    offset += static_cast<Offset>(sizeof(value));
}

} // namespace BigEndian
