// Host stand-in for the SPU intrinsics header: the vector types and the handful of
// intrinsics ax_spu_core.h's vector kernels use, so the host test runs those kernels too.
//
// A vector keeps its elements in host order, so loading one from an array of host
// int16s or int32s reads them as the SPU reads its big-endian ones. Everything that
// works on bytes (shuffles, quadword byte shifts, casts between vector types) goes
// through the big-endian byte image, as on the SPU.
#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace spu_stub {

template <typename T, int N>
struct alignas(16) V;

template <typename T, int N>
inline void ToBytes(const V<T, N>& v, uint8_t b[16])
{
    for (int i = 0; i < N; ++i) {
        uint64_t x = static_cast<uint64_t>(static_cast<typename std::make_unsigned<T>::type>(v.e[i]));
        for (int k = static_cast<int>(sizeof(T)) - 1; k >= 0; --k) {
            b[i * sizeof(T) + k] = static_cast<uint8_t>(x);
            x >>= 8;
        }
    }
}

template <typename T, int N>
inline void FromBytes(V<T, N>& v, const uint8_t b[16])
{
    for (int i = 0; i < N; ++i) {
        uint64_t x = 0;
        for (size_t k = 0; k < sizeof(T); ++k)
            x = (x << 8) | b[i * sizeof(T) + k];
        v.e[i] = static_cast<T>(x);
    }
}

template <typename T, int N>
struct alignas(16) V {
    T e[N];
    V() = default;
    // Element list, as the SPU compiler's vector literals.
    template <typename... A, typename = typename std::enable_if<(sizeof...(A) > 1)>::type>
    V(A... a) : e{static_cast<T>(a)...} {}
    template <typename U, int M>
    explicit V(const V<U, M>& other)
    {
        uint8_t b[16];
        ToBytes(other, b);
        FromBytes(*this, b);
    }
};

} // namespace spu_stub

typedef spu_stub::V<uint8_t, 16> vec_uchar16;
typedef spu_stub::V<int16_t, 8> vec_short8;
typedef spu_stub::V<uint16_t, 8> vec_ushort8;
typedef spu_stub::V<int32_t, 4> vec_int4;
typedef spu_stub::V<uint32_t, 4> vec_uint4;

inline vec_int4 spu_splats(int x) { vec_int4 v; for (auto& e : v.e) e = x; return v; }
inline vec_uint4 spu_splats(unsigned x) { vec_uint4 v; for (auto& e : v.e) e = x; return v; }
inline vec_short8 spu_splats(int16_t x) { vec_short8 v; for (auto& e : v.e) e = x; return v; }

template <typename T, int N>
inline spu_stub::V<T, N> spu_add(const spu_stub::V<T, N>& a, const spu_stub::V<T, N>& b)
{
    spu_stub::V<T, N> r;
    for (int i = 0; i < N; ++i)
        r.e[i] = static_cast<T>(static_cast<uint64_t>(a.e[i]) + static_cast<uint64_t>(b.e[i]));
    return r;
}

template <typename T, int N>
inline spu_stub::V<T, N> spu_and(const spu_stub::V<T, N>& a, const spu_stub::V<T, N>& b)
{
    spu_stub::V<T, N> r;
    for (int i = 0; i < N; ++i) r.e[i] = static_cast<T>(a.e[i] & b.e[i]);
    return r;
}

template <typename T, int N>
inline spu_stub::V<T, N> spu_or(const spu_stub::V<T, N>& a, const spu_stub::V<T, N>& b)
{
    spu_stub::V<T, N> r;
    for (int i = 0; i < N; ++i) r.e[i] = static_cast<T>(a.e[i] | b.e[i]);
    return r;
}

inline vec_int4 spu_sl(const vec_int4& a, unsigned n)
{
    vec_int4 r;
    for (int i = 0; i < 4; ++i) r.e[i] = static_cast<int32_t>(static_cast<uint32_t>(a.e[i]) << n);
    return r;
}

inline vec_int4 spu_rlmaska(const vec_int4& a, int n)
{
    vec_int4 r;
    for (int i = 0; i < 4; ++i) r.e[i] = a.e[i] >> -n;
    return r;
}

template <typename T>
inline vec_uint4 spu_cmpgt(const spu_stub::V<T, 4>& a, const spu_stub::V<T, 4>& b)
{
    vec_uint4 r;
    for (int i = 0; i < 4; ++i) r.e[i] = a.e[i] > b.e[i] ? 0xffffffffu : 0u;
    return r;
}

inline vec_int4 spu_sel(const vec_int4& a, const vec_int4& b, const vec_uint4& mask)
{
    vec_int4 r;
    for (int i = 0; i < 4; ++i)
        r.e[i] = static_cast<int32_t>((static_cast<uint32_t>(a.e[i]) & ~mask.e[i]) |
                                      (static_cast<uint32_t>(b.e[i]) & mask.e[i]));
    return r;
}

// Products of the odd (mulo) or even (mule) halfword elements, one per word.
inline vec_int4 spu_mulo(const vec_short8& a, const vec_short8& b)
{
    vec_int4 r;
    for (int i = 0; i < 4; ++i) r.e[i] = static_cast<int32_t>(a.e[2 * i + 1]) * b.e[2 * i + 1];
    return r;
}

inline vec_uint4 spu_mulo(const vec_ushort8& a, const vec_ushort8& b)
{
    vec_uint4 r;
    for (int i = 0; i < 4; ++i) r.e[i] = static_cast<uint32_t>(a.e[2 * i + 1]) * b.e[2 * i + 1];
    return r;
}

inline vec_int4 spu_mule(const vec_short8& a, const vec_short8& b)
{
    vec_int4 r;
    for (int i = 0; i < 4; ++i) r.e[i] = static_cast<int32_t>(a.e[2 * i]) * b.e[2 * i];
    return r;
}

template <typename T, int N>
inline spu_stub::V<T, N> spu_shuffle(const spu_stub::V<T, N>& a, const spu_stub::V<T, N>& b,
                                     const vec_uchar16& pattern)
{
    uint8_t x[32], out[16];
    spu_stub::ToBytes(a, x);
    spu_stub::ToBytes(b, x + 16);
    for (int i = 0; i < 16; ++i) {
        const uint8_t p = pattern.e[i];
        if ((p & 0xc0) == 0x80) out[i] = 0x00;
        else if ((p & 0xe0) == 0xc0) out[i] = 0xff;
        else if ((p & 0xe0) == 0xe0) out[i] = 0x80;
        else out[i] = x[p & 31];
    }
    spu_stub::V<T, N> r;
    spu_stub::FromBytes(r, out);
    return r;
}

// Quadword shifts by bytes: left by n, and right by -n (the rlmask form takes the
// negated count). Sixteen or more bytes leaves zero.
template <typename T, int N>
inline spu_stub::V<T, N> spu_slqwbyte(const spu_stub::V<T, N>& a, unsigned n)
{
    uint8_t x[16], out[16] = {};
    spu_stub::ToBytes(a, x);
    n &= 31;
    for (unsigned i = 0; i + n < 16; ++i) out[i] = x[i + n];
    spu_stub::V<T, N> r;
    spu_stub::FromBytes(r, out);
    return r;
}

template <typename T, int N>
inline spu_stub::V<T, N> spu_rlmaskqwbyte(const spu_stub::V<T, N>& a, int n)
{
    uint8_t x[16], out[16] = {};
    spu_stub::ToBytes(a, x);
    const unsigned s = static_cast<unsigned>(-n) & 31;
    for (unsigned i = s; i < 16; ++i) out[i] = x[i - s];
    spu_stub::V<T, N> r;
    spu_stub::FromBytes(r, out);
    return r;
}

template <typename T, int N>
inline T spu_extract(const spu_stub::V<T, N>& v, int i) { return v.e[i & (N - 1)]; }
