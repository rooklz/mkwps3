// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright 2018 Dolphin Emulator Project
//
// Portions of this file are derived from the Dolphin Emulator
// (https://github.com/dolphin-emu/dolphin):
//
//   * Source/Core/Common/FloatUtils.cpp - the Gekko/Broadway fres and frsqrte
//     estimate tables and the interpolation routines that read them.
//
// The remainder is this project's PS3 port of WiiCompiled's float layer.

#pragma once
// Floating-point and paired-single PowerPC semantics for the Cell PPE.
//
// The PPE is a PowerPC: its FPU computes the same IEEE double and single
// operations Gekko does, with the same fused multiply-add and the same
// FPSCR[NI] flushing (ppc_isa_fpenv.h). So where upstream emulates an
// instruction on SSE, this file mostly emits the instruction itself:
//   - every paired-single lane is a double holding a single-precision value
//     (FprVal in ppc_isa_context.h); ps_add/ps_mul/ps_madd are two fadds /
//     fmuls / fmadds, ps_merge is register moves, ps_sel is fsel;
//   - nothing crosses between the FPU and the integer unit on a hot path. On
//     the PPE that crossing is a store and a reload of the same address -- a
//     ~40-cycle load-hit-store -- so the bit-level helpers below are kept for
//     cold paths and rare operand forms only.
// Gekko quirks the PPE does not share (the 25-bit rounding of fmuls' C
// operand, the fres/frsqrte estimate tables) stay in software, behind a
// test that lets the common case through on the FPU alone.

#include "ppc_isa_config.h"
#include "ppc_isa_context.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// ---- bit casts (cold: each is an FPR<->GPR move through memory) ----------

inline uint64_t PpcBitCastToU64Inline(double value)
{
    uint64_t integral = 0;
    std::memcpy(&integral, &value, sizeof(integral));
    return integral;
}

inline uint32_t PPC_FprLowWordInline(double value)
{
    return static_cast<uint32_t>(PpcBitCastToU64Inline(value));
}

inline double PpcBitCastToDoubleInline(uint64_t value)
{
    double result = 0.0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline uint32_t PpcBitCastToU32Inline(float value)
{
    uint32_t integral = 0;
    std::memcpy(&integral, &value, sizeof(integral));
    return integral;
}

inline float PpcBitCastToFloatInline(uint32_t value)
{
    float result = 0.0f;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

// The state-free ABI moves FPR values as 64-bit patterns. An FPR crossing it
// is a scalar (compiled code does not return paired values in registers), so
// the pattern is ps0's double bits and both lanes take it back.
inline uint64_t PpcFprBits(const PPC_FPR& fpr)
{
    return PpcBitCastToU64Inline(fpr.d.ps0);
}

inline void PpcSetFprBits(PPC_FPR& fpr, uint64_t bits)
{
    fpr.d = FprVal(PpcBitCastToDoubleInline(bits));
}

// ---- lanes --------------------------------------------------------------

inline FprVal PpcGetPairedFprInline(const PPC_FPR& fpr)
{
    return fpr.d;
}

inline void PpcSetPairedFprInline(PPC_FPR& fpr, FprVal packed)
{
    fpr.d = packed;
}

// A lane already holds a single-precision value; narrowing it is exact.
inline float PpcGetPs0Inline(FprVal value)
{
    return static_cast<float>(value.ps0);
}

inline float PpcGetPs1Inline(FprVal value)
{
    return static_cast<float>(value.ps1);
}

inline FprVal PpcPackPairedInline(float ps0, float ps1)
{
    return FprVal(static_cast<double>(ps0), static_cast<double>(ps1));
}

inline bool MkwHostNiActiveInline() noexcept
{
    return g_mkwHostNiActive;
}

// frsp. With FPSCR[NI] mirrored onto the host the PPE flushes a denormal
// single result to zero exactly as Gekko does, so the conversion is the
// instruction and nothing else.
inline float PpcForceSingleValueInline(double value)
{
    return static_cast<float>(value);
}

inline float PpcFlushSingleForNiInline(float value)
{
    if (!MkwHostNiActiveInline())
        return value;
    if (std::fabs(value) < std::numeric_limits<float>::min())
        return __builtin_copysign(0.0f, value);
    return value;
}

inline FprVal PpcFlushPairedForNiInline(FprVal value)
{
    return value;
}

// Is `value` exactly representable as a single? frsp + fcmpu, no GPR.
inline bool PpcIsSingleInline(double value)
{
    return static_cast<double>(static_cast<float>(value)) == value;
}

// Gekko rounds the C operand of fmuls/fmadds to a 25-bit significand. A value
// that already fits a single (24 bits) is unchanged by that, which is nearly
// every operand game code produces; only a true double takes the bit path.
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PpcForce25BitSlow(double value)
{
    constexpr uint64_t kDoubleExpMask = 0x7FF0000000000000ULL;
    constexpr uint64_t kDoubleFracMask = 0x000FFFFFFFFFFFFFULL;
    constexpr int kDoubleFracWidth = 52;

    uint64_t integral = PpcBitCastToU64Inline(value);
    const uint64_t exponent = integral & kDoubleExpMask;
    const uint64_t fraction = integral & kDoubleFracMask;

    if (exponent == 0 && fraction != 0) {
        int64_t keepMask = static_cast<int64_t>(0xFFFFFFFFF8000000LL);
        uint64_t round = 0x8000000ULL;
        uint32_t leadingZeros = 0;
        uint64_t normalizedFraction = fraction;
        while ((normalizedFraction & (1ULL << 63)) == 0) {
            normalizedFraction <<= 1;
            ++leadingZeros;
        }
        const uint32_t shift = leadingZeros - (63 - kDoubleFracWidth);
        keepMask >>= shift;
        round >>= shift;
        integral = (integral & static_cast<uint64_t>(keepMask)) + (integral & round);
    } else {
        integral = (integral & 0xFFFFFFFFF8000000ULL) + (integral & 0x8000000ULL);
    }
    return PpcBitCastToDoubleInline(integral);
}

inline double PpcForce25BitInline(double value)
{
    if (PpcIsSingleInline(value))
        return value;
    return PpcForce25BitSlow(value);
}

// ---- fres / frsqrte: Gekko's estimate tables ----------------------------

struct PpcEstimateEntryInline {
    int32_t base;
    int32_t decrement;
};

inline constexpr std::array<PpcEstimateEntryInline, 32> kPpcFresEstimateInline = {{
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
    {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106},
}};

inline double PpcApproximateReciprocalInline(double value)
{
    constexpr uint64_t kSign = 0x8000000000000000ULL;
    constexpr uint64_t kExponent = 0x7FF0000000000000ULL;
    constexpr uint64_t kFraction = 0x000FFFFFFFFFFFFFULL;
    constexpr uint64_t kQuietBit = 0x0008000000000000ULL;
    const uint64_t input = PpcBitCastToU64Inline(value);
    const uint64_t mantissa = input & kFraction;
    const uint64_t sign = input & kSign;
    uint64_t exponent = input & kExponent;

    if (mantissa == 0 && exponent == 0)
        return PpcBitCastToDoubleInline(sign | kExponent);
    if (exponent == kExponent) {
        if (mantissa == 0)
            return PpcBitCastToDoubleInline(sign);
        return PpcBitCastToDoubleInline(input | kQuietBit);
    }
    if (exponent < (uint64_t{895} << 52))
        return __builtin_copysign(static_cast<double>(std::numeric_limits<float>::max()), value);
    if (exponent >= (uint64_t{1149} << 52))
        return __builtin_copysign(0.0, value);

    exponent = (uint64_t{0x7FD} << 52) - exponent;
    const int index = static_cast<int>(mantissa >> 37);
    const auto& entry = kPpcFresEstimateInline[static_cast<size_t>(index / 1024)];
    const int64_t estimate = static_cast<int64_t>(entry.base) -
        (static_cast<int64_t>(entry.decrement) * (index % 1024) + 1) / 2;
    return PpcBitCastToDoubleInline(sign | exponent | (static_cast<uint64_t>(estimate) << 29));
}

inline constexpr std::array<PpcEstimateEntryInline, 32> kPpcFrsqrteEstimateInline = {{
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
}};

inline double PpcApproximateReciprocalSquareRootInline(double value)
{
    constexpr uint64_t kSign = 0x8000000000000000ULL;
    constexpr uint64_t kExponent = 0x7FF0000000000000ULL;
    constexpr uint64_t kFraction = 0x000FFFFFFFFFFFFFULL;
    constexpr uint64_t kQuietBit = 0x0008000000000000ULL;
    constexpr uint64_t kCanonicalQuietNaN = kExponent | kQuietBit;

    const uint64_t input = PpcBitCastToU64Inline(value);
    uint64_t mantissa = input & kFraction;
    const uint64_t sign = input & kSign;
    int64_t exponent = static_cast<int64_t>(input & kExponent);

    if (mantissa == 0 && exponent == 0)
        return PpcBitCastToDoubleInline(sign | kExponent);
    if (static_cast<uint64_t>(exponent) == kExponent) {
        if (mantissa == 0)
            return sign ? PpcBitCastToDoubleInline(kCanonicalQuietNaN) : 0.0;
        return PpcBitCastToDoubleInline(input | kQuietBit);
    }
    if (sign != 0)
        return PpcBitCastToDoubleInline(kCanonicalQuietNaN);
    if (exponent == 0) {
        do {
            exponent -= int64_t{1} << 52;
            mantissa <<= 1;
        } while ((mantissa & (uint64_t{1} << 52)) == 0);
        mantissa &= kFraction;
        exponent += int64_t{1} << 52;
    }

    const int64_t exponentLsb = exponent & (int64_t{1} << 52);
    exponent = (((int64_t{0x3FF} << 52) - ((exponent - (int64_t{0x3FE} << 52)) / 2)) &
                static_cast<int64_t>(kExponent));
    const int index = static_cast<int>((static_cast<uint64_t>(exponentLsb) | mantissa) >> 37);
    const auto& entry = kPpcFrsqrteEstimateInline[static_cast<size_t>(index / 2048)];
    const int64_t estimate = static_cast<int64_t>(entry.base) +
                             static_cast<int64_t>(entry.decrement) * (index % 2048);
    return PpcBitCastToDoubleInline(sign | static_cast<uint64_t>(exponent) |
                                    (static_cast<uint64_t>(estimate) << 26));
}

// ---- fctiwz -------------------------------------------------------------
// The integer goes in the low word of the FPR; generated code reads it back
// with PPC_FprLowWordInline or stores it with stfd/stfiwx.
inline int32_t PpcClampIntegerWordInline(double value)
{
    if (std::isnan(value))
        return static_cast<int32_t>(0x80000000u);
    if (value >= 2147483647.0)
        return 2147483647;
    if (value <= -2147483648.0)
        return static_cast<int32_t>(0x80000000u);
    return static_cast<int32_t>(value);
}

inline double PpcPackIntegerWordInline(int32_t value)
{
    return PpcBitCastToDoubleInline(static_cast<uint64_t>(static_cast<uint32_t>(value)));
}

// ---- scalar single and fused families ------------------------------------

inline float PpcFmadd32(float a, float c, float b)
{
    return __builtin_fmaf(a, c, b);   // fmadds: one rounding, as Gekko
}

// Gekko's single fused ops keep A and B at full precision, round C to 25
// bits and round only the result. A double FMA reproduces that except for an
// exact result one double ULP from a single tie; recover those cases.
template <bool Subtract>
inline double PpcAccurateSingleMaddIntermediateInline(double a, double c, double b)
{
    const double roundedC = PpcForce25BitInline(c);
    const double signedB = Subtract ? -b : b;
    double result = __builtin_fma(a, roundedC, signedB);
    const uint64_t resultBits = PpcBitCastToU64Inline(result);
    constexpr uint64_t kDiscardedMask = 0x000000001FFFFFFFULL;
    constexpr uint64_t kEvenTie = 0x0000000010000000ULL;
    if ((resultBits & kDiscardedMask) == kEvenTie) {
        const double aPrime = signedB - result;
        const double bPrime = result + aPrime;
        const double deltaA = __builtin_fma(a, roundedC, aPrime);
        const double deltaB = signedB - bPrime;
        const double error = deltaA + deltaB;
        if (error != 0.0)
            result = PpcBitCastToDoubleInline(
                (error > 0.0) == (result > 0.0) ? resultBits + 1 : resultBits - 1);
    }
    return result;
}

// All four operands single-representable: the PPE's own fmadds rounds once to
// single exactly as the double-FMA-then-round path does, so use it.
inline bool PpcAllSingle3(double a, double c, double b)
{
    return PpcIsSingleInline(a) & PpcIsSingleInline(c) & PpcIsSingleInline(b);
}

inline double PpcFmulsInline(double a, double c)
{
    return static_cast<double>(PpcForceSingleValueInline(a * PpcForce25BitInline(c)));
}

inline double PpcFmulsNoNiInline(double a, double c)
{
    return static_cast<double>(static_cast<float>(a * PpcForce25BitInline(c)));
}

inline double PpcFmaddInline(double a, double c, double b) { return __builtin_fma(a, c, b); }
inline double PpcFmsubInline(double a, double c, double b) { return __builtin_fma(a, c, -b); }

inline double PpcFnmaddInline(double a, double c, double b)
{
    const double result = __builtin_fma(a, c, b);
    return std::isnan(result) ? result : -result;
}

inline double PpcFnmsubInline(double a, double c, double b)
{
    const double result = __builtin_fma(a, c, -b);
    return std::isnan(result) ? result : -result;
}

inline double PpcFmaddsInline(double a, double c, double b)
{
    if (PpcAllSingle3(a, c, b))
        return static_cast<double>(PpcFmadd32(static_cast<float>(a), static_cast<float>(c),
                                              static_cast<float>(b)));
    return static_cast<double>(PpcForceSingleValueInline(
        PpcAccurateSingleMaddIntermediateInline<false>(a, c, b)));
}

inline double PpcFmsubsInline(double a, double c, double b)
{
    if (PpcAllSingle3(a, c, b))
        return static_cast<double>(PpcFmadd32(static_cast<float>(a), static_cast<float>(c),
                                              -static_cast<float>(b)));
    return static_cast<double>(PpcForceSingleValueInline(
        PpcAccurateSingleMaddIntermediateInline<true>(a, c, b)));
}

inline double PpcFnmaddsInline(double a, double c, double b)
{
    const float result = static_cast<float>(PpcFmaddsInline(a, c, b));
    return static_cast<double>(std::isnan(result) ? result : -result);
}

inline double PpcFnmsubsInline(double a, double c, double b)
{
    const float result = static_cast<float>(PpcFmsubsInline(a, c, b));
    return static_cast<double>(std::isnan(result) ? result : -result);
}

// ---- paired singles -----------------------------------------------------
// Each lane is computed in single precision with one rounding, as Gekko and
// upstream's SSE lanes do. A lane is a single held in a double, so the float
// casts below are exact and GCC emits fmuls/fadds/fmadds directly.

#define MKW_PS_LANES(expr0, expr1) FprVal(static_cast<double>(expr0), static_cast<double>(expr1))
#define MKW_F(x) static_cast<float>(x)

inline FprVal PPC_PsMulInline(FprVal a, FprVal b)
{
    return MKW_PS_LANES(MKW_F(a.ps0) * MKW_F(b.ps0), MKW_F(a.ps1) * MKW_F(b.ps1));
}
inline FprVal PPC_PsMulNoNiInline(FprVal a, FprVal b) { return PPC_PsMulInline(a, b); }

inline FprVal PPC_PsAddInline(FprVal a, FprVal b)
{
    return MKW_PS_LANES(MKW_F(a.ps0) + MKW_F(b.ps0), MKW_F(a.ps1) + MKW_F(b.ps1));
}
inline FprVal PPC_PsAddNoNiInline(FprVal a, FprVal b) { return PPC_PsAddInline(a, b); }

inline FprVal PPC_PsSubInline(FprVal a, FprVal b)
{
    return MKW_PS_LANES(MKW_F(a.ps0) - MKW_F(b.ps0), MKW_F(a.ps1) - MKW_F(b.ps1));
}
inline FprVal PPC_PsSubNoNiInline(FprVal a, FprVal b) { return PPC_PsSubInline(a, b); }

inline FprVal PPC_PsDivInline(FprVal a, FprVal b)
{
    return MKW_PS_LANES(MKW_F(a.ps0) / MKW_F(b.ps0), MKW_F(a.ps1) / MKW_F(b.ps1));
}

inline FprVal PPC_PsMaddInline(FprVal a, FprVal c, FprVal b)
{
    return MKW_PS_LANES(PpcFmadd32(MKW_F(a.ps0), MKW_F(c.ps0), MKW_F(b.ps0)),
                        PpcFmadd32(MKW_F(a.ps1), MKW_F(c.ps1), MKW_F(b.ps1)));
}
inline FprVal PPC_PsMaddNoNiInline(FprVal a, FprVal c, FprVal b) { return PPC_PsMaddInline(a, c, b); }

inline FprVal PPC_PsMsubInline(FprVal a, FprVal c, FprVal b)
{
    return MKW_PS_LANES(PpcFmadd32(MKW_F(a.ps0), MKW_F(c.ps0), -MKW_F(b.ps0)),
                        PpcFmadd32(MKW_F(a.ps1), MKW_F(c.ps1), -MKW_F(b.ps1)));
}
inline FprVal PPC_PsMsubNoNiInline(FprVal a, FprVal c, FprVal b) { return PPC_PsMsubInline(a, c, b); }

inline FprVal PPC_PsMadds0Inline(FprVal a, FprVal c, FprVal b)
{
    return MKW_PS_LANES(PpcFmadd32(MKW_F(a.ps0), MKW_F(c.ps0), MKW_F(b.ps0)),
                        PpcFmadd32(MKW_F(a.ps1), MKW_F(c.ps0), MKW_F(b.ps1)));
}

inline FprVal PPC_PsMadds1Inline(FprVal a, FprVal c, FprVal b)
{
    return MKW_PS_LANES(PpcFmadd32(MKW_F(a.ps0), MKW_F(c.ps1), MKW_F(b.ps0)),
                        PpcFmadd32(MKW_F(a.ps1), MKW_F(c.ps1), MKW_F(b.ps1)));
}

inline float PpcNegateUnlessNan(float v) { return std::isnan(v) ? v : -v; }

inline FprVal PPC_PsNmaddInline(FprVal a, FprVal c, FprVal b)
{
    const FprVal r = PPC_PsMaddInline(a, c, b);
    return MKW_PS_LANES(PpcNegateUnlessNan(MKW_F(r.ps0)), PpcNegateUnlessNan(MKW_F(r.ps1)));
}

inline FprVal PPC_PsNmsubInline(FprVal a, FprVal c, FprVal b)
{
    const FprVal r = PPC_PsMsubInline(a, c, b);
    return MKW_PS_LANES(PpcNegateUnlessNan(MKW_F(r.ps0)), PpcNegateUnlessNan(MKW_F(r.ps1)));
}
inline FprVal PPC_PsNmsubNoNiInline(FprVal a, FprVal c, FprVal b) { return PPC_PsNmsubInline(a, c, b); }

inline FprVal PPC_PsMuls0Inline(FprVal a, FprVal c)
{
    return MKW_PS_LANES(MKW_F(a.ps0) * MKW_F(c.ps0), MKW_F(a.ps1) * MKW_F(c.ps0));
}

inline FprVal PPC_PsMuls1Inline(FprVal a, FprVal c)
{
    return MKW_PS_LANES(MKW_F(a.ps0) * MKW_F(c.ps1), MKW_F(a.ps1) * MKW_F(c.ps1));
}

inline PPC_FPR PpcMakePairedResultInline(float ps0, float ps1)
{
    PPC_FPR result;
    result.d = PpcPackPairedInline(ps0, ps1);
    return result;
}

// Not an architectural operation (Gekko has no scalar-to-paired move): the
// representation change a scalar result needs before paired code reads it.
inline FprVal PPC_PsFromScalarInline(double value)
{
    const float single = static_cast<float>(value);
    return PpcPackPairedInline(single, single);
}
inline FprVal PPC_PsFromScalarNoNiInline(double value) { return PPC_PsFromScalarInline(value); }

inline double PPC_PsToScalarInline(FprVal value)
{
    return value.ps0;
}

// ps_merge: result.ps0 from frA, result.ps1 from frB -- register moves.
inline FprVal PPC_PsMerge00Inline(FprVal a, FprVal b) { return FprVal(a.ps0, b.ps0); }
inline FprVal PPC_PsMerge01Inline(FprVal a, FprVal b) { return FprVal(a.ps0, b.ps1); }
inline FprVal PPC_PsMerge10Inline(FprVal a, FprVal b) { return FprVal(a.ps1, b.ps0); }
inline FprVal PPC_PsMerge11Inline(FprVal a, FprVal b) { return FprVal(a.ps1, b.ps1); }

// "frA >= -0.0 ? frC : frB" per lane; an unordered control selects frB. This
// is fsel's own definition.
inline FprVal PPC_PsSelInline(FprVal lhs, FprVal control, FprVal rhs)
{
    return FprVal(control.ps0 >= -0.0 ? lhs.ps0 : rhs.ps0,
                  control.ps1 >= -0.0 ? lhs.ps1 : rhs.ps1);
}

inline FprVal PPC_PsNegInline(FprVal value) { return FprVal(-value.ps0, -value.ps1); }
inline FprVal PPC_PsAbsInline(FprVal value) { return FprVal(std::fabs(value.ps0), std::fabs(value.ps1)); }

inline FprVal PPC_PsSum0Inline(FprVal a, FprVal b, FprVal c)
{
    return MKW_PS_LANES(PpcForceSingleValueInline(a.ps0 + b.ps1),
                        PpcFlushSingleForNiInline(MKW_F(c.ps1)));
}

inline FprVal PPC_PsSum1Inline(FprVal a, FprVal b, FprVal c)
{
    return MKW_PS_LANES(PpcFlushSingleForNiInline(MKW_F(c.ps0)),
                        PpcForceSingleValueInline(a.ps0 + b.ps1));
}

#undef MKW_F

// ---- single <-> double bit conversions (cold paths) ----------------------

inline uint32_t PpcConvertToSingleFTZInline(uint64_t value)
{
    const uint32_t exp = static_cast<uint32_t>((value >> 52) & 0x7FFu);
    if (exp > 896u || (value & 0x7FFFFFFFFFFFFFFFULL) == 0)
        return static_cast<uint32_t>(((value >> 32) & 0xC0000000ULL) | ((value >> 29) & 0x3FFFFFFFULL));
    return static_cast<uint32_t>((value >> 32) & 0x80000000ULL);
}

inline uint64_t PpcConvertToDoubleBitsInline(uint32_t value)
{
    uint64_t x = value;
    uint64_t exp = (x >> 23) & 0xFFu;
    uint64_t frac = x & 0x007FFFFFu;

    if (exp > 0 && exp < 255) {
        const uint64_t y = !(exp >> 7);
        const uint64_t z = (y << 61) | (y << 60) | (y << 59);
        return ((x & 0xC0000000ULL) << 32) | z | ((x & 0x3FFFFFFFULL) << 29);
    }
    if (exp == 0 && frac != 0) {
        exp = 1023 - 126;
        do {
            frac <<= 1;
            --exp;
        } while ((frac & 0x00800000u) == 0);
        return ((x & 0x80000000ULL) << 32) | (exp << 52) | ((frac & 0x007FFFFFULL) << 29);
    }
    const uint64_t y = exp >> 7;
    const uint64_t z = (y << 61) | (y << 60) | (y << 59);
    return ((x & 0xC0000000ULL) << 32) | z | ((x & 0x3FFFFFFFULL) << 29);
}

inline FprVal PpcPackPairedBitsInline(uint32_t ps0, uint32_t ps1)
{
    return PpcPackPairedInline(PpcBitCastToFloatInline(ps0), PpcBitCastToFloatInline(ps1));
}

// ---- out-of-line helpers (ppc_helpers.cpp / fpu_helpers.cpp) ------------

extern "C" void PPC_Mtfsf(uint32_t fieldMask, double source);
extern "C" void PPC_Mtfsfi(uint32_t field, uint32_t value);
extern "C" void PPC_Mtfsb0(uint32_t bit);
extern "C" void PPC_Mtfsb1(uint32_t bit);
extern "C" double PPC_Mffs();

extern "C" inline FprVal PPC_PsAdd(FprVal a, FprVal b) { return PPC_PsAddInline(a, b); }
extern "C" inline FprVal PPC_PsSub(FprVal a, FprVal b) { return PPC_PsSubInline(a, b); }
extern "C" inline FprVal PPC_PsDiv(FprVal a, FprVal b) { return PPC_PsDivInline(a, b); }
extern "C" inline FprVal PPC_PsNeg(FprVal v) { return PPC_PsNegInline(v); }
extern "C" inline FprVal PPC_PsMul(FprVal a, FprVal b) { return PPC_PsMulInline(a, b); }
extern "C" inline FprVal PPC_PsMsub(FprVal a, FprVal c, FprVal b) { return PPC_PsMsubInline(a, c, b); }
extern "C" inline FprVal PPC_PsMadd(FprVal a, FprVal c, FprVal b) { return PPC_PsMaddInline(a, c, b); }
extern "C" inline FprVal PPC_PsNmsub(FprVal a, FprVal c, FprVal b) { return PPC_PsNmsubInline(a, c, b); }
extern "C" inline FprVal PPC_PsMadds0(FprVal a, FprVal c, FprVal b) { return PPC_PsMadds0Inline(a, c, b); }
extern "C" inline FprVal PPC_PsMadds1(FprVal a, FprVal c, FprVal b) { return PPC_PsMadds1Inline(a, c, b); }
extern "C" inline FprVal PPC_PsNmadd(FprVal a, FprVal c, FprVal b) { return PPC_PsNmaddInline(a, c, b); }
extern "C" inline FprVal PPC_PsSel(FprVal l, FprVal c, FprVal r) { return PPC_PsSelInline(l, c, r); }

extern "C" inline FprVal PPC_PsRes(FprVal value)
{
    return PpcPackPairedInline(
        static_cast<float>(PpcApproximateReciprocalInline(value.ps0)),
        static_cast<float>(PpcApproximateReciprocalInline(value.ps1)));
}

extern "C" inline FprVal PPC_PsRsqrte(FprVal value)
{
    return PpcPackPairedInline(
        static_cast<float>(PpcApproximateReciprocalSquareRootInline(value.ps0)),
        static_cast<float>(PpcApproximateReciprocalSquareRootInline(value.ps1)));
}

extern "C" inline FprVal PPC_PsFromScalar(double v) { return PPC_PsFromScalarInline(v); }
extern "C" inline double PPC_PsToScalar(FprVal v) { return PPC_PsToScalarInline(v); }
extern "C" inline FprVal PPC_PsMerge00(FprVal a, FprVal b) { return PPC_PsMerge00Inline(a, b); }
extern "C" inline FprVal PPC_PsMerge01(FprVal a, FprVal b) { return PPC_PsMerge01Inline(a, b); }
extern "C" inline FprVal PPC_PsMerge10(FprVal a, FprVal b) { return PPC_PsMerge10Inline(a, b); }
extern "C" inline FprVal PPC_PsMerge11(FprVal a, FprVal b) { return PPC_PsMerge11Inline(a, b); }
extern "C" inline double PPC_Fsqrt(double value) { return std::sqrt(value); }

// fres replicates its single result into both lanes.
extern "C" inline FprVal PPC_Fres(FprVal value)
{
    const float result = static_cast<float>(PpcApproximateReciprocalInline(value.ps0));
    return PpcPackPairedInline(result, result);
}

extern "C" inline double PPC_Frsqrte(double value)
{
    return PpcApproximateReciprocalSquareRootInline(value);
}

extern "C" inline double PPC_Fsel(double control, double negative, double positive)
{
    return (control >= -0.0) ? positive : negative;
}

extern "C" inline double PPC_Fctiwz(double value)
{
    return PpcPackIntegerWordInline(PpcClampIntegerWordInline(value));
}

extern "C" inline double PPC_Fmadd(double a, double c, double b) { return PpcFmaddInline(a, c, b); }
extern "C" inline double PPC_Fmsub(double a, double c, double b) { return PpcFmsubInline(a, c, b); }
extern "C" inline double PPC_Fnmadd(double a, double c, double b) { return PpcFnmaddInline(a, c, b); }
extern "C" inline double PPC_Fnmsub(double a, double c, double b) { return PpcFnmsubInline(a, c, b); }
extern "C" inline double PPC_Fadds(double a, double b) { return static_cast<double>(PpcForceSingleValueInline(a + b)); }
extern "C" inline double PPC_Fsubs(double a, double b) { return static_cast<double>(PpcForceSingleValueInline(a - b)); }
extern "C" inline double PPC_Fmuls(double a, double b) { return PpcFmulsInline(a, b); }
extern "C" inline double PPC_Fdivs(double a, double b) { return static_cast<double>(PpcForceSingleValueInline(a / b)); }
extern "C" inline double PPC_Fmadds(double a, double c, double b) { return PpcFmaddsInline(a, c, b); }
extern "C" inline double PPC_Fmsubs(double a, double c, double b) { return PpcFmsubsInline(a, c, b); }
extern "C" inline double PPC_Fnmadds(double a, double c, double b) { return PpcFnmaddsInline(a, c, b); }
extern "C" inline double PPC_Fnmsubs(double a, double c, double b) { return PpcFnmsubsInline(a, c, b); }

extern "C" double PPC_Fctiw(double value);
extern "C" void PPC_Stfiwx(uint32_t addr, double fprValue);
extern "C" inline FprVal PPC_PsSum0(FprVal a, FprVal b, FprVal c) { return PPC_PsSum0Inline(a, b, c); }
extern "C" inline FprVal PPC_PsSum1(FprVal a, FprVal b, FprVal c) { return PPC_PsSum1Inline(a, b, c); }
extern "C" inline FprVal PPC_PsMuls0(FprVal a, FprVal c) { return PPC_PsMuls0Inline(a, c); }
extern "C" inline FprVal PPC_PsMuls1(FprVal a, FprVal c) { return PPC_PsMuls1Inline(a, c); }
extern "C" inline FprVal PPC_PsAbs(FprVal v) { return PPC_PsAbsInline(v); }
extern "C" inline FprVal PPC_PsNabs(FprVal v) { return FprVal(-std::fabs(v.ps0), -std::fabs(v.ps1)); }
extern "C" inline FprVal PPC_PsMr(FprVal v) { return v; }

extern "C" void PPC_Fcmp(uint32_t crField, double a, double b);
extern "C" void PPC_PsCmpo0(uint32_t crField, FprVal a, FprVal b);
extern "C" void PPC_PsCmpu0(uint32_t crField, FprVal a, FprVal b);
extern "C" void PPC_PsCmpo1(uint32_t crField, FprVal a, FprVal b);
extern "C" void PPC_PsCmpu1(uint32_t crField, FprVal a, FprVal b);
