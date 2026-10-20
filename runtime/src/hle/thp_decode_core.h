#pragma once

// Native decoder for THP video frames, shared by the THPVideoDecode override
// (thp_decode.cpp) and the host test (tools/thp-test). Host pointers only; the
// stream is read as big-endian bytes/words, so the core is endian-clean.
//
// Semantics follow THPDec as translated (func_801B3BAC and its callees):
//  - Markers: SOI, DQT, SOF0, DHT, DRI, SOS; APP0-15/COM are skipped, anything
//    else is status 11. The scan starts right after SOS; EOI is never read.
//  - Entropy data is a raw MSB-first bit stream: no 0xFF00 unstuffing and no
//    RSTn markers. At the end of a restart interval the reader only moves to
//    the next byte boundary and the DC predictors reset.
//  - 4:2:0 only: 16x16 MCUs decoded Y0 Y1 Y2 Y3 U V, baseline Huffman.
//  - Output: GX I8 tiles (8x4 texels, 32 bytes) for a Y texture `width` wide
//    and U/V textures `width/2` wide, one band of 16 Y / 8 U,V rows per MCU
//    row; bands are (width*16)&~255 and (width*4)&~63 bytes apart, the sizes
//    THPDec's LCStoreData copies.
//  - IDCT: AAN float with the dequantisation pre-scaled as THPDec does; a
//    sample is clamp(trunc(x + 128)) like its psq_st u8 stores. The VMX path
//    keeps the whole block in vector registers; other hosts run the same
//    algorithm on a lane-exact emulation of the VMX operations used.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ALTIVEC__) && !defined(THP_CORE_NO_VMX)
#include <altivec.h>
// Without __APPLE_ALTIVEC__ these are macros that break std::vector; the core
// only spells __vector.
#undef vector
#undef pixel
#undef bool
#define THP_CORE_VMX 1
#else
#define THP_CORE_VMX 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define THP_INLINE inline __attribute__((always_inline))
#define THP_NOINLINE __attribute__((noinline))
#define THP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define THP_ASSUME_ALIGNED(p, n) __builtin_assume_aligned((p), (n))
#else
#define THP_INLINE inline
#define THP_NOINLINE
#define THP_UNLIKELY(x) (x)
#define THP_ASSUME_ALIGNED(p, n) (p)
#endif

namespace ThpCore {

// THPVideoDecode status codes raised while parsing the frame.
enum : int32_t {
    kOk = 0,
    kErrSyntax = 3,      // marker does not start with 0xFF
    kErrPrecision = 10,  // SOF0 sample precision != 8
    kErrMarker = 11,     // marker THPDec does not handle
    kErrComponents = 12, // SOF0/SOS component count != 3
    kErrHuffman = 15,    // SOS selects a Huffman table no DHT defined
    kErrSampling = 19,   // sampling factors other than 2x2, 1x1, 1x1
};

// Widest frame the staging path (unaligned buffers, width % 16 != 0) handles;
// THPDec's locked-cache bands overflow long before this.
constexpr uint32_t kMaxStagedWidth = 1024;

struct HuffTable {
    uint16_t lut[512];    // 9-bit peek: (code length << 8) | symbol; 0 = longer code
    int16_t fastAc[512];  // AC: (value << 8) | (run << 4) | (code + magnitude bits); 0 = no
    uint32_t maxCode[18]; // first code past each length, left-aligned to 16 bits
    int32_t delta[17];    // symbol index = (code >> (16 - length)) + delta[length]
    uint8_t symbols[256];
    uint8_t invalidLength; // bits an undecodable code consumes (0 for undefined tables)
};

struct alignas(16) Frame {
    // Transposed (column-major) AAN-scaled dequantisation factors, / 8.
    alignas(16) float quant[4][64];
    int32_t quantDc[4]; // unscaled DC quantiser, for DC-only blocks
    HuffTable huff[4];  // THPDec's index: 2 * Th + Tc
    uint16_t width;
    uint16_t height;
    uint16_t mcusPerRow;
    uint16_t restartInterval;
    bool restart;
    uint8_t validHuff;
    uint8_t compQuant[3];
    uint8_t compDc[3]; // huff[] index of each component's DC table
    uint8_t compAc[3];
    const uint8_t* scan; // first entropy-coded byte
};

struct alignas(128) Workspace {
    alignas(128) int16_t coef[6][64]; // transposed coefficients, kept zero between blocks
    alignas(128) uint8_t stageY[16 * kMaxStagedWidth + 256];
    alignas(128) uint8_t stageU[4 * kMaxStagedWidth + 128];
    alignas(128) uint8_t stageV[4 * kMaxStagedWidth + 128];
};

inline uint32_t BandCount(const Frame& f) { return (static_cast<uint32_t>(f.height) + 15u) >> 4; }
inline uint32_t LumaBandBytes(const Frame& f) { return (static_cast<uint32_t>(f.width) << 4) & ~255u; }
inline uint32_t ChromaBandBytes(const Frame& f) { return (static_cast<uint32_t>(f.width) << 2) & ~63u; }

namespace detail {

// Zigzag index -> transposed natural index (column * 8 + row). THPDec's table
// has 16 extra entries pointing at 63 so a run past the end stays in the block.
constexpr uint8_t kZigzagT[80] = {
    0,  8,  1,  2,  9,  16, 24, 17, 10, 3,  4,  11, 18, 25, 32, 40,
    33, 26, 19, 12, 5,  6,  13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
    28, 21, 14, 7,  15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
    23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

// AAN scale factors exactly as THPDec stores them (doubles at 0x80252E90).
constexpr double kAanScale[8] = {
    1.0, 1.3870398998260498, 1.3065630197525024, 1.1758755445480347,
    1.0, 0.78569495677948,   0.5411961078643799, 0.27589938044548035,
};

// JPEG EXTEND offsets: a magnitude field with a leading 0 bit is negative.
constexpr int32_t kExtendOffset[17] = {
    0,      -1,     -3,     -7,      -15,     -31,     -63,     -127,   -255,
    -511,   -1023,  -2047,  -4095,   -8191,   -16383,  -32767,  -65535,
};

THP_INLINE uint32_t LoadBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

THP_INLINE uint32_t LoadBE16(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
}

// Aligned big-endian word read; a plain lwz on big-endian hosts.
THP_INLINE uint32_t LoadWord(const uint8_t* p)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint32_t w;
    std::memcpy(&w, THP_ASSUME_ALIGNED(p, 4), 4);
    return w;
#else
    return LoadBE32(p);
#endif
}

// THPDec reads aligned 32-bit words starting at the word that holds the first
// scan byte. Here the bits sit left-aligned in a 64-bit buffer refilled one
// word at a time; words at or past `end` read as zero.
// Counters are pointer-sized so 64-bit hosts need no sign/zero extensions.
struct BitReader {
    uint64_t buf;
    intptr_t count; // valid bits in buf
    const uint8_t* p;
    const uint8_t* end;

    THP_INLINE void Refill()
    {
        if (count <= 32) {
            uint32_t w = 0;
            if (p < end) {
                w = LoadWord(p);
                p += 4;
            }
            buf |= static_cast<uint64_t>(w) << (32 - count);
            count += 32;
        }
    }
    THP_INLINE uintptr_t Peek9() const { return static_cast<uintptr_t>(buf >> 55); }
    THP_INLINE void Drop(uintptr_t n)
    {
        buf <<= n;
        count -= static_cast<intptr_t>(n);
    }
    // n in 1..16 bits, EXTENDed.
    THP_INLINE int32_t Receive(uintptr_t n)
    {
        const uint64_t b = buf;
        const int32_t v = static_cast<int32_t>(b >> (64 - n));
        const int32_t positive = static_cast<int32_t>(static_cast<int64_t>(b) >> 63);
        Drop(n);
        return v + (kExtendOffset[n] & ~positive);
    }
    // Restart interval: skip to the next byte boundary (THPDec rounds its bit
    // position up; the RSTn bytes themselves are not skipped).
    THP_INLINE void AlignToByte() { Drop(static_cast<uintptr_t>(count) & 7u); }
};

THP_INLINE uintptr_t DecodeSymbol(BitReader& br, const HuffTable& t)
{
    const uintptr_t e = t.lut[br.Peek9()];
    if (!THP_UNLIKELY(e == 0)) {
        br.Drop(e >> 8);
        return e & 0xFFu;
    }
    const uint32_t code = static_cast<uint32_t>(br.buf >> 48);
    uintptr_t len = 10;
    while (code >= t.maxCode[len])
        ++len;
    if (THP_UNLIKELY(len > 16)) {
        br.Drop(t.invalidLength);
        return 0;
    }
    br.Drop(len);
    return t.symbols[((code >> (16 - len)) + static_cast<uint32_t>(t.delta[len])) & 0xFFu];
}

// One 8x8 block: DC difference then AC run/levels into the transposed block
// (blk[0] keeps the DC value). Returns 1 if any AC coefficient was stored.
// Each symbol plus its magnitude bits is at most 31 bits, so one refill per
// symbol suffices.
THP_INLINE uint32_t DecodeBlock(BitReader& br, const HuffTable& dc, const HuffTable& ac, int32_t& pred,
                                int16_t* blk)
{
    br.Refill();
    const uintptr_t size = DecodeSymbol(br, dc) & 15u;
    if (size != 0)
        pred += br.Receive(size); // THPDec's s16 predictor: only the low 16 bits are ever used
    blk[0] = static_cast<int16_t>(pred);

    uint32_t any = 0;
    uintptr_t k = 1;
    while (k < 64) {
        br.Refill();
        const intptr_t fast = ac.fastAc[br.Peek9()];
        if (fast != 0) {
            k += (static_cast<uintptr_t>(fast) >> 4) & 15u;
            br.Drop(static_cast<uintptr_t>(fast) & 15u);
            blk[kZigzagT[k]] = static_cast<int16_t>(fast >> 8);
            ++k;
            any = 1;
            continue;
        }
        const uintptr_t rs = DecodeSymbol(br, ac);
        const uintptr_t size2 = rs & 15u;
        const uintptr_t run = rs >> 4;
        if (size2 != 0) {
            k += run;
            blk[kZigzagT[k]] = static_cast<int16_t>(br.Receive(size2));
            ++k;
            any = 1;
        } else if (run == 15) {
            k += 16;
        } else {
            break; // EOB
        }
    }
    return any;
}

struct ScanState {
    BitReader br;
    int32_t pred[3];
    const HuffTable* dc[3];
    const HuffTable* ac[3];
};

// The six blocks of one MCU into coef[0..5]; returns a mask of the blocks with
// AC coefficients. Out of line with the reader state in locals, so it stays in
// registers and is written back once per MCU.
THP_NOINLINE inline uint32_t DecodeMcu(ScanState& s, int16_t (*coef)[64])
{
    BitReader br = s.br;
    int32_t predY = s.pred[0], predU = s.pred[1], predV = s.pred[2];
    const HuffTable& dcY = *s.dc[0];
    const HuffTable& acY = *s.ac[0];
    uint32_t mask = 0;
    for (uint32_t b = 0; b < 4; ++b)
        mask |= DecodeBlock(br, dcY, acY, predY, coef[b]) << b;
    mask |= DecodeBlock(br, *s.dc[1], *s.ac[1], predU, coef[4]) << 4;
    mask |= DecodeBlock(br, *s.dc[2], *s.ac[2], predV, coef[5]) << 5;
    s.br = br;
    s.pred[0] = predY;
    s.pred[1] = predU;
    s.pred[2] = predV;
    return mask;
}

inline void BuildHuffTable(HuffTable& t, const uint8_t* counts, const uint8_t* symbols, uint32_t total,
                           bool ac)
{
    uint16_t codes[256];
    uint8_t lengths[256];
    uint32_t k = 0;
    uint32_t code = 0;
    for (uint32_t len = 1; len <= 16; ++len) {
        t.delta[len] = static_cast<int32_t>(k) - static_cast<int32_t>(code);
        for (uint32_t i = 0; i < counts[len - 1] && k < total; ++i, ++k, ++code) {
            codes[k] = static_cast<uint16_t>(code);
            lengths[k] = static_cast<uint8_t>(len);
        }
        t.maxCode[len] = code << (16 - len);
        code <<= 1;
    }
    t.maxCode[0] = 0;
    t.maxCode[17] = 0xFFFFFFFFu;
    t.invalidLength = 16;
    std::memset(t.symbols, 0, sizeof(t.symbols));
    std::memcpy(t.symbols, symbols, total);

    std::memset(t.lut, 0, sizeof(t.lut));
    for (uint32_t i = 0; i < total; ++i) {
        const uint32_t len = lengths[i];
        if (len > 9)
            continue;
        const uint32_t first = static_cast<uint32_t>(codes[i]) << (9 - len);
        const uint32_t n = 1u << (9 - len);
        if (first + n > 512)
            continue; // over-subscribed table
        for (uint32_t j = 0; j < n; ++j)
            t.lut[first + j] = static_cast<uint16_t>((len << 8) | symbols[i]);
    }

    std::memset(t.fastAc, 0, sizeof(t.fastAc));
    if (!ac)
        return;
    for (uint32_t idx = 0; idx < 512; ++idx) {
        const uint32_t e = t.lut[idx];
        if (e == 0)
            continue;
        const uint32_t len = e >> 8;
        const uint32_t run = (e >> 4) & 15u;
        const uint32_t size = e & 15u;
        if (size == 0 || len + size > 9)
            continue;
        const uint32_t bits = (idx >> (9 - len - size)) & ((1u << size) - 1u);
        int32_t v = static_cast<int32_t>(bits);
        if (bits < (1u << (size - 1)))
            v += kExtendOffset[size];
        if (v < -128 || v > 127)
            continue;
        t.fastAc[idx] = static_cast<int16_t>(v * 256 + static_cast<int32_t>(run << 4) +
                                             static_cast<int32_t>(len + size));
    }
}

// A table SOS selects without a DHT: THPDec decodes it from its zeroed file
// info, where every lookup yields symbol 0 and consumes no bits.
inline void BuildEmptyHuffTable(HuffTable& t)
{
    std::memset(&t, 0, sizeof(t));
    t.maxCode[17] = 0xFFFFFFFFu;
    t.invalidLength = 0;
}

// ---- markers (__THPRead*) ---------------------------------------------------

inline int32_t ReadDqt(const uint8_t*& c, const uint8_t* end, Frame& f)
{
    if (end - c < 2)
        return kErrSyntax;
    uint32_t remaining = (LoadBE16(c) - 2u) & 0xFFFFu;
    c += 2;
    do {
        if (end - c < 65)
            return kErrSyntax;
        // THPDec indexes its tables with the whole Pq/Tq byte; 0..2 in practice.
        const uint32_t tq = *c++ & 3u;
        uint8_t natural[64];
        for (uint32_t i = 0; i < 64; ++i) {
            const uint32_t t = kZigzagT[i];
            natural[((t & 7u) << 3) | (t >> 3)] = c[i];
        }
        c += 64;
        for (uint32_t r = 0; r < 8; ++r) {
            for (uint32_t col = 0; col < 8; ++col) {
                const double scaled = kAanScale[col] * (static_cast<double>(natural[r * 8 + col]) * kAanScale[r]);
                f.quant[tq][col * 8 + r] = static_cast<float>(scaled) * 0.125f;
            }
        }
        f.quantDc[tq] = natural[0];
        remaining = (remaining - 65u) & 0xFFFFu;
    } while (remaining != 0);
    return kOk;
}

inline int32_t ReadSof(const uint8_t*& c, const uint8_t* end, Frame& f)
{
    if (end - c < 17)
        return kErrSyntax;
    c += 2;
    if (*c++ != 8)
        return kErrPrecision;
    f.height = static_cast<uint16_t>(LoadBE16(c));
    c += 2;
    f.width = static_cast<uint16_t>(LoadBE16(c));
    c += 2;
    if (*c++ != 3)
        return kErrComponents;
    for (uint32_t i = 0; i < 3; ++i) {
        ++c; // component id: THPDec ignores it
        const uint8_t sampling = *c++;
        if (sampling != (i == 0 ? 0x22 : 0x11))
            return kErrSampling;
        f.compQuant[i] = *c++;
    }
    return kOk;
}

inline int32_t ReadDht(const uint8_t*& c, const uint8_t* end, Frame& f)
{
    if (end - c < 2)
        return kErrSyntax;
    uint32_t remaining = (LoadBE16(c) - 2u) & 0xFFFFu;
    c += 2;
    do {
        if (end - c < 17)
            return kErrSyntax;
        const uint8_t tcth = *c++;
        const uint32_t index = ((static_cast<uint32_t>(tcth) << 1) & 30u) + (tcth >> 4);
        const uint8_t* counts = c;
        uint32_t total = 0;
        for (uint32_t i = 0; i < 16; ++i)
            total += counts[i];
        c += 16;
        if (total > 256 || end - c < static_cast<ptrdiff_t>(total))
            return kErrSyntax;
        // Indices past 3 land outside THPDec's table array; keep only the bit.
        if (index < 4)
            BuildHuffTable(f.huff[index], counts, c, total, (index & 1u) != 0);
        c += total;
        if (index < 8) // u8 mask; slw by 32+ gives 0
            f.validHuff = static_cast<uint8_t>(f.validHuff | (1u << index));
        remaining = (remaining - (total + 17u)) & 0xFFFFu;
    } while (remaining != 0);
    return kOk;
}

inline int32_t ReadSos(const uint8_t*& c, const uint8_t* end, Frame& f)
{
    if (end - c < 12)
        return kErrSyntax;
    c += 2;
    if (*c++ != 3)
        return kErrComponents;
    for (uint32_t i = 0; i < 3; ++i) {
        ++c; // component id
        const uint8_t sel = *c++;
        const uint32_t dc = sel >> 4;
        const uint32_t ac = sel & 15u;
        // THPDec's check, including its bit for the AC table (Tc + 1, not 2*Th + 1).
        if ((f.validHuff & (1u << dc)) == 0 || (f.validHuff & (1u << (ac + 1))) == 0)
            return kErrHuffman;
        // Tables 2*Th + Tc past the array would read THPDec's other state.
        if (2 * dc >= 4 || 2 * ac + 1 >= 4)
            return kErrHuffman;
        f.compDc[i] = static_cast<uint8_t>(2 * dc);
        f.compAc[i] = static_cast<uint8_t>(2 * ac + 1);
    }
    c += 3; // Ss, Se, Ah/Al
    f.mcusPerRow = static_cast<uint16_t>((static_cast<uint32_t>(f.width) + 15u) >> 4);
    for (uint32_t i = 0; i < 3; ++i) {
        if ((f.validHuff & (1u << f.compDc[i])) == 0)
            BuildEmptyHuffTable(f.huff[f.compDc[i]]);
        if ((f.validHuff & (1u << f.compAc[i])) == 0)
            BuildEmptyHuffTable(f.huff[f.compAc[i]]);
    }
    return kOk;
}

// ---- IDCT -------------------------------------------------------------------

#if THP_CORE_VMX
typedef __vector float VF;
typedef __vector signed int VI;
typedef __vector signed short VS;
typedef __vector unsigned char VB;

THP_INLINE VF Splat(float x) { return (VF){x, x, x, x}; }
THP_INLINE VF Add(VF a, VF b) { return vec_add(a, b); }
THP_INLINE VF Sub(VF a, VF b) { return vec_sub(a, b); }
THP_INLINE VF Madd(VF a, VF b, VF c) { return vec_madd(a, b, c); }
THP_INLINE VF Nmsub(VF a, VF b, VF c) { return vec_nmsub(a, b, c); } // c - a*b
THP_INLINE VS LoadS(const int16_t* p) { return vec_ld(0, p); }
THP_INLINE VF LoadF(const float* p) { return vec_ld(0, p); }
THP_INLINE VF HiToFloat(VS v) { return vec_ctf(vec_unpackh(v), 0); }
THP_INLINE VF LoToFloat(VS v) { return vec_ctf(vec_unpackl(v), 0); }
THP_INLINE VI Trunc(VF v) { return vec_cts(v, 0); }
THP_INLINE VS Pack(VI a, VI b) { return vec_packs(a, b); }
THP_INLINE VB PackU(VS a, VS b) { return vec_packsu(a, b); }
THP_INLINE VF MergeH(VF a, VF b) { return vec_mergeh(a, b); }
THP_INLINE VF MergeL(VF a, VF b) { return vec_mergel(a, b); }
THP_INLINE void StoreB(VB v, uint8_t* p) { vec_st(v, 0, p); }
THP_INLINE void ZeroS(int16_t* p) { vec_st(vec_splat_s16(0), 0, p); }
#else
// Lane-exact stand-ins for the VMX operations above (element 0 = lowest address).
struct VF { float f[4]; };
struct VI { int32_t i[4]; };
struct VS { int16_t s[8]; };
struct VB { uint8_t b[16]; };

THP_INLINE VF Splat(float x) { return VF{{x, x, x, x}}; }
THP_INLINE VF Add(VF a, VF b) { VF r; for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] + b.f[i]; return r; }
THP_INLINE VF Sub(VF a, VF b) { VF r; for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] - b.f[i]; return r; }
THP_INLINE VF Madd(VF a, VF b, VF c) { VF r; for (int i = 0; i < 4; ++i) r.f[i] = a.f[i] * b.f[i] + c.f[i]; return r; }
THP_INLINE VF Nmsub(VF a, VF b, VF c) { VF r; for (int i = 0; i < 4; ++i) r.f[i] = c.f[i] - a.f[i] * b.f[i]; return r; }
THP_INLINE VS LoadS(const int16_t* p) { VS r; std::memcpy(r.s, p, 16); return r; }
THP_INLINE VF LoadF(const float* p) { VF r; std::memcpy(r.f, p, 16); return r; }
THP_INLINE VF HiToFloat(VS v) { VF r; for (int i = 0; i < 4; ++i) r.f[i] = static_cast<float>(v.s[i]); return r; }
THP_INLINE VF LoToFloat(VS v) { VF r; for (int i = 0; i < 4; ++i) r.f[i] = static_cast<float>(v.s[4 + i]); return r; }
THP_INLINE VI Trunc(VF v)
{
    VI r;
    for (int i = 0; i < 4; ++i) {
        const float x = v.f[i];
        r.i[i] = x >= 2147483520.0f ? 2147483647 : x <= -2147483648.0f ? (-2147483647 - 1) : static_cast<int32_t>(x);
    }
    return r;
}
THP_INLINE VS Pack(VI a, VI b)
{
    VS r;
    for (int i = 0; i < 8; ++i) {
        const int32_t x = i < 4 ? a.i[i] : b.i[i - 4];
        r.s[i] = static_cast<int16_t>(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
    }
    return r;
}
THP_INLINE VB PackU(VS a, VS b)
{
    VB r;
    for (int i = 0; i < 16; ++i) {
        const int32_t x = i < 8 ? a.s[i] : b.s[i - 8];
        r.b[i] = static_cast<uint8_t>(x < 0 ? 0 : x > 255 ? 255 : x);
    }
    return r;
}
THP_INLINE VF MergeH(VF a, VF b) { return VF{{a.f[0], b.f[0], a.f[1], b.f[1]}}; }
THP_INLINE VF MergeL(VF a, VF b) { return VF{{a.f[2], b.f[2], a.f[3], b.f[3]}}; }
THP_INLINE void StoreB(VB v, uint8_t* p) { std::memcpy(p, v.b, 16); }
THP_INLINE void ZeroS(int16_t* p) { std::memset(p, 0, 16); }
#endif

struct IdctConst {
    VF k1414, k1847, k1082, km2613, zero;
};

// In-place 8-point AAN IDCT across eight vectors (jidctflt's flow graph with
// THPDec's constants), four independent transforms per vector.
THP_INLINE void Aan8(VF& i0, VF& i1, VF& i2, VF& i3, VF& i4, VF& i5, VF& i6, VF& i7, const IdctConst& k)
{
    const VF t10 = Add(i0, i4);
    const VF t11 = Sub(i0, i4);
    const VF t13 = Add(i2, i6);
    const VF t12n = Nmsub(Sub(i2, i6), k.k1414, t13); // -tmp12
    const VF e0 = Add(t10, t13);
    const VF e3 = Sub(t10, t13);
    const VF e1 = Sub(t11, t12n);
    const VF e2 = Add(t11, t12n);

    const VF z13 = Add(i5, i3);
    const VF z10 = Sub(i5, i3);
    const VF z11 = Add(i1, i7);
    const VF z12 = Sub(i1, i7);
    const VF o7 = Add(z11, z13);
    const VF o11 = Madd(Sub(z11, z13), k.k1414, k.zero);
    const VF z5 = Madd(Add(z10, z12), k.k1847, k.zero);
    const VF o10n = Nmsub(z12, k.k1082, z5); // -tmp10
    const VF o12 = Madd(z10, k.km2613, z5);
    const VF o6 = Sub(o12, o7);
    const VF o5 = Sub(o11, o6);
    const VF o4 = Sub(o5, o10n);

    i0 = Add(e0, o7);
    i7 = Sub(e0, o7);
    i1 = Add(e1, o6);
    i6 = Sub(e1, o6);
    i2 = Add(e2, o5);
    i5 = Sub(e2, o5);
    i4 = Add(e3, o4);
    i3 = Sub(e3, o4);
}

THP_INLINE void Transpose4(VF& a, VF& b, VF& c, VF& d)
{
    const VF t0 = MergeH(a, c);
    const VF t1 = MergeH(b, d);
    const VF t2 = MergeL(a, c);
    const VF t3 = MergeL(b, d);
    a = MergeH(t0, t1);
    b = MergeL(t0, t1);
    c = MergeH(t2, t3);
    d = MergeL(t2, t3);
}

THP_INLINE void StoreTile(VB rows01, VB rows23, uint8_t* dst, bool aligned)
{
    if (aligned) {
        StoreB(rows01, dst);
        StoreB(rows23, dst + 16);
    } else {
        alignas(16) uint8_t tmp[32];
        StoreB(rows01, tmp);
        StoreB(rows23, tmp + 16);
        std::memcpy(dst, tmp, 32);
    }
}

// coef: transposed coefficients (vector j = coefficient column j); zeroed on
// return. tileA/tileB receive pixel rows 0-3 and 4-7 (8 bytes per row).
THP_NOINLINE inline void IdctBlock(int16_t* coef, const float* q, uint8_t* tileA, uint8_t* tileB, bool aligned)
{
    IdctConst k;
    k.k1414 = Splat(1.4142135381698608f);
    k.k1847 = Splat(1.8477590084075928f);
    k.k1082 = Splat(1.0823922157287598f);
    k.km2613 = Splat(-2.613126039505005f);
    k.zero = Splat(0.0f);

    const VS c0 = LoadS(coef + 0), c1 = LoadS(coef + 8), c2 = LoadS(coef + 16), c3 = LoadS(coef + 24);
    const VS c4 = LoadS(coef + 32), c5 = LoadS(coef + 40), c6 = LoadS(coef + 48), c7 = LoadS(coef + 56);
    ZeroS(coef + 0);
    ZeroS(coef + 8);
    ZeroS(coef + 16);
    ZeroS(coef + 24);
    ZeroS(coef + 32);
    ZeroS(coef + 40);
    ZeroS(coef + 48);
    ZeroS(coef + 56);

    // Pass 1, horizontal: vector j holds coefficient column j, rows 0-3 (a)
    // and 4-7 (b); after it vector x holds Z[rows][x].
    VF a0 = Madd(HiToFloat(c0), LoadF(q + 0), k.zero), a1 = Madd(HiToFloat(c1), LoadF(q + 8), k.zero);
    VF a2 = Madd(HiToFloat(c2), LoadF(q + 16), k.zero), a3 = Madd(HiToFloat(c3), LoadF(q + 24), k.zero);
    VF a4 = Madd(HiToFloat(c4), LoadF(q + 32), k.zero), a5 = Madd(HiToFloat(c5), LoadF(q + 40), k.zero);
    VF a6 = Madd(HiToFloat(c6), LoadF(q + 48), k.zero), a7 = Madd(HiToFloat(c7), LoadF(q + 56), k.zero);
    Aan8(a0, a1, a2, a3, a4, a5, a6, a7, k);
    VF b0 = Madd(LoToFloat(c0), LoadF(q + 4), k.zero), b1 = Madd(LoToFloat(c1), LoadF(q + 12), k.zero);
    VF b2 = Madd(LoToFloat(c2), LoadF(q + 20), k.zero), b3 = Madd(LoToFloat(c3), LoadF(q + 28), k.zero);
    VF b4 = Madd(LoToFloat(c4), LoadF(q + 36), k.zero), b5 = Madd(LoToFloat(c5), LoadF(q + 44), k.zero);
    VF b6 = Madd(LoToFloat(c6), LoadF(q + 52), k.zero), b7 = Madd(LoToFloat(c7), LoadF(q + 60), k.zero);
    Aan8(b0, b1, b2, b3, b4, b5, b6, b7, k);

    // Transpose to rows: a0-a3 -> row v 0-3 x 0-3, a4-a7 -> rows 0-3 x 4-7,
    // b0-b3 -> rows 4-7 x 0-3, b4-b7 -> rows 4-7 x 4-7.
    Transpose4(a0, a1, a2, a3);
    Transpose4(a4, a5, a6, a7);
    Transpose4(b0, b1, b2, b3);
    Transpose4(b4, b5, b6, b7);

    // Pass 2, vertical, with the +128 level shift on the DC row.
    const VF bias = Splat(128.0f);
    a0 = Add(a0, bias);
    a4 = Add(a4, bias);
    Aan8(a0, a1, a2, a3, b0, b1, b2, b3, k); // columns 0-3
    Aan8(a4, a5, a6, a7, b4, b5, b6, b7, k); // columns 4-7

    const VS r0 = Pack(Trunc(a0), Trunc(a4)), r1 = Pack(Trunc(a1), Trunc(a5));
    const VS r2 = Pack(Trunc(a2), Trunc(a6)), r3 = Pack(Trunc(a3), Trunc(a7));
    const VS r4 = Pack(Trunc(b0), Trunc(b4)), r5 = Pack(Trunc(b1), Trunc(b5));
    const VS r6 = Pack(Trunc(b2), Trunc(b6)), r7 = Pack(Trunc(b3), Trunc(b7));
    StoreTile(PackU(r0, r1), PackU(r2, r3), tileA, aligned);
    StoreTile(PackU(r4, r5), PackU(r6, r7), tileB, aligned);
}

// DC-only block: the IDCT output is exactly trunc(dc * q / 8 + 128).
THP_INLINE void FillBlock(int32_t dc, int32_t quantDc, uint8_t* tileA, uint8_t* tileB)
{
    int32_t v = (dc * quantDc + 1024) >> 3;
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    uint64_t x = static_cast<uint32_t>(v);
    x |= x << 8;
    x |= x << 16;
    x |= x << 32;
    std::memcpy(tileA, &x, 8);
    std::memcpy(tileA + 8, &x, 8);
    std::memcpy(tileA + 16, &x, 8);
    std::memcpy(tileA + 24, &x, 8);
    std::memcpy(tileB, &x, 8);
    std::memcpy(tileB + 8, &x, 8);
    std::memcpy(tileB + 16, &x, 8);
    std::memcpy(tileB + 24, &x, 8);
}

THP_INLINE void EmitBlock(int16_t* coef, uint32_t hasAc, const float* q, int32_t quantDc, uint8_t* tileA,
                          uint8_t* tileB, bool aligned)
{
    if (hasAc) {
        IdctBlock(coef, q, tileA, tileB, aligned);
    } else {
        FillBlock(coef[0], quantDc, tileA, tileB);
        coef[0] = 0;
    }
}

} // namespace detail

// THPVideoDecode's marker loop. `end` bounds every read (the original has no
// bound; hitting it is reported as a syntax error). On kOk, f describes the
// frame and f.scan points at the entropy-coded data.
inline int32_t ParseHeaders(const uint8_t* src, const uint8_t* end, Frame& f)
{
    // THPDec zeroes its file info (DCZeroRange) before every frame.
    std::memset(f.quant, 0, sizeof(f.quant));
    std::memset(f.quantDc, 0, sizeof(f.quantDc));
    f.width = f.height = f.mcusPerRow = 0;
    f.restartInterval = 0;
    f.restart = false;
    f.validHuff = 0;
    std::memset(f.compQuant, 0, sizeof(f.compQuant));
    std::memset(f.compDc, 0, sizeof(f.compDc));
    std::memset(f.compAc, 0, sizeof(f.compAc));
    f.scan = nullptr;

    const uint8_t* c = src;
    for (;;) {
        if (end - c < 2)
            return kErrSyntax;
        if (*c++ != 0xFF)
            return kErrSyntax;
        while (c < end && *c == 0xFF)
            ++c;
        if (c >= end)
            return kErrSyntax;
        const uint8_t marker = *c++;
        int32_t status = kOk;
        if (marker <= 0xD7) {
            if (marker == 0xC4)
                status = detail::ReadDht(c, end, f);
            else if (marker == 0xC0)
                status = detail::ReadSof(c, end, f);
            else
                return kErrMarker;
        } else if (marker <= 0xDF) {
            if (marker == 0xDD) {
                if (end - c < 4)
                    return kErrSyntax;
                f.restart = true;
                f.restartInterval = static_cast<uint16_t>(detail::LoadBE16(c + 2));
                c += 4;
            } else if (marker == 0xDB) {
                status = detail::ReadDqt(c, end, f);
            } else if (marker == 0xDA) {
                status = detail::ReadSos(c, end, f);
                if (status == kOk) {
                    f.scan = c;
                    return kOk;
                }
            } else if (marker != 0xD8) {
                return kErrMarker;
            }
        } else if (marker <= 0xEF || marker == 0xFE) {
            if (end - c < 2 || end - c < static_cast<ptrdiff_t>(detail::LoadBE16(c)))
                return kErrSyntax;
            c += detail::LoadBE16(c);
        } else {
            return kErrMarker;
        }
        if (status != kOk)
            return status;
    }
}

// __THPDecompressYUV: decodes the scan of a frame ParseHeaders accepted into
// the three tiled planes (BandCount bands of LumaBandBytes/ChromaBandBytes).
// `end` bounds the bit reader. Returns false only for a frame wider than
// kMaxStagedWidth that also needs the staging path (nothing is written).
inline bool DecodeScan(const Frame& f, const uint8_t* end, Workspace& ws, uint8_t* outY, uint8_t* outU,
                       uint8_t* outV)
{
    using namespace detail;
    const uint32_t width = f.width;
    const uint32_t mcus = f.mcusPerRow;
    const uint32_t bands = BandCount(f);
    const uint32_t bandY = LumaBandBytes(f);
    const uint32_t bandC = ChromaBandBytes(f);
    const uint32_t tileRowY = width * 4u;          // THPDec's Gwid * 4
    const uint32_t tileRowC = (width >> 1) * 4u;
    const bool direct = (width & 15u) == 0 &&
                        ((reinterpret_cast<uintptr_t>(outY) | reinterpret_cast<uintptr_t>(outU) |
                          reinterpret_cast<uintptr_t>(outV)) & 15u) == 0;
    if (!direct && width > kMaxStagedWidth)
        return false;
    const bool aligned = direct || ((tileRowY | tileRowC) & 15u) == 0;

    const uint32_t qiY = f.compQuant[0] & 3u, qiU = f.compQuant[1] & 3u, qiV = f.compQuant[2] & 3u;
    const float* const qY = f.quant[qiY];
    const float* const qU = f.quant[qiU];
    const float* const qV = f.quant[qiV];
    const int32_t qdY = f.quantDc[qiY], qdU = f.quantDc[qiU], qdV = f.quantDc[qiV];

    std::memset(ws.coef, 0, sizeof(ws.coef));

    // __THPPrepBitStream: start at the word holding the first scan byte.
    ScanState s;
    const uintptr_t scanAddr = reinterpret_cast<uintptr_t>(f.scan);
    s.br.buf = 0;
    s.br.count = 0;
    s.br.p = reinterpret_cast<const uint8_t*>(scanAddr & ~static_cast<uintptr_t>(3));
    s.br.end = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(end) & ~static_cast<uintptr_t>(3));
    s.br.Refill();
    s.br.Drop(static_cast<uint32_t>(scanAddr & 3u) * 8u);
    for (uint32_t i = 0; i < 3; ++i) {
        s.pred[i] = 0;
        s.dc[i] = &f.huff[f.compDc[i]];
        s.ac[i] = &f.huff[f.compAc[i]];
    }
    uint32_t restartLeft = f.restartInterval;

    for (uint32_t band = 0; band < bands; ++band) {
        uint8_t* const dY = direct ? outY + band * bandY : ws.stageY;
        uint8_t* const dU = direct ? outU + band * bandC : ws.stageU;
        uint8_t* const dV = direct ? outV + band * bandC : ws.stageV;
        for (uint32_t mcu = 0; mcu < mcus; ++mcu) {
            // All six blocks are decoded before any IDCT, so the vector loads
            // never hit the scalar coefficient stores still in flight.
            const uint32_t acMask = DecodeMcu(s, ws.coef);

            // __THPInverseDCTNoYPos writes rows 0-7 at x, __THPInverseDCTY8 rows 8-15.
            uint8_t* const ty = dY + mcu * 64u;
            EmitBlock(ws.coef[0], acMask & 1u, qY, qdY, ty, ty + tileRowY, aligned);
            EmitBlock(ws.coef[1], acMask & 2u, qY, qdY, ty + 32, ty + tileRowY + 32, aligned);
            EmitBlock(ws.coef[2], acMask & 4u, qY, qdY, ty + 2 * tileRowY, ty + 3 * tileRowY, aligned);
            EmitBlock(ws.coef[3], acMask & 8u, qY, qdY, ty + 2 * tileRowY + 32, ty + 3 * tileRowY + 32, aligned);
            uint8_t* const tu = dU + mcu * 32u;
            uint8_t* const tv = dV + mcu * 32u;
            EmitBlock(ws.coef[4], acMask & 16u, qU, qdU, tu, tu + tileRowC, aligned);
            EmitBlock(ws.coef[5], acMask & 32u, qV, qdV, tv, tv + tileRowC, aligned);

            if (f.restart) {
                restartLeft = (restartLeft - 1u) & 0xFFFFu;
                if (restartLeft == 0) {
                    restartLeft = f.restartInterval;
                    s.br.AlignToByte();
                    s.pred[0] = s.pred[1] = s.pred[2] = 0;
                }
            }
        }
        if (!direct) {
            std::memcpy(outY + band * bandY, ws.stageY, bandY);
            std::memcpy(outU + band * bandC, ws.stageU, bandC);
            std::memcpy(outV + band * bandC, ws.stageV, bandC);
        }
    }
    return true;
}

} // namespace ThpCore
