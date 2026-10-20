#pragma once
// The one xxHash entry point the runtime uses (display-list reuse digests),
// implemented as XXH64. The digest only has to be strong and stable within a
// run, not equal to upstream's XXH3 values.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mkw_xxh {
constexpr uint64_t P1 = 11400714785074694791ull;
constexpr uint64_t P2 = 14029467366897019727ull;
constexpr uint64_t P3 = 1609587929392839161ull;
constexpr uint64_t P4 = 9650029242287828579ull;
constexpr uint64_t P5 = 2870177450012600261ull;

inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t read64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t round(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
inline uint64_t merge(uint64_t acc, uint64_t v) { return (acc ^ round(0, v)) * P1 + P4; }

inline uint64_t hash64(const void* data, size_t len, uint64_t seed = 0)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* const end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* const limit = end - 32;
        do {
            v1 = round(v1, read64(p)); p += 8;
            v2 = round(v2, read64(p)); p += 8;
            v3 = round(v3, read64(p)); p += 8;
            v4 = round(v4, read64(p)); p += 8;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += static_cast<uint64_t>(len);
    while (p + 8 <= end) {
        h ^= round(0, read64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(read32(p)) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * P5;
        h = rotl(h, 11) * P1;
        ++p;
    }
    h ^= h >> 33; h *= P2;
    h ^= h >> 29; h *= P3;
    h ^= h >> 32;
    return h;
}
} // namespace mkw_xxh

typedef uint64_t XXH64_hash_t;

inline XXH64_hash_t XXH3_64bits(const void* data, size_t len) { return mkw_xxh::hash64(data, len); }
inline XXH64_hash_t XXH64(const void* data, size_t len, uint64_t seed) { return mkw_xxh::hash64(data, len, seed); }
