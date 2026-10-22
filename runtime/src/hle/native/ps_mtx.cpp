// PSMTXConcat and PSMTXMultVec, native.
//
// Both are the SDK's paired-single routines: the same lane operations in the
// same order through the helpers the translated code uses, so the results are
// the same bits. What goes is the translation's overhead around them: the f14,
// f15 and f31 saves, the stack frame, and the guest register file. The game
// calls Concat thousands of times a frame.

#define MKW_PIN_ARENA 1

#include "hle_stubs.h"
#include "isa/ppc_isa_float.h"
#include "native_difftest.h"

#include <cstdint>

extern "C" void mkw_ref_80199D64(CpuContext* ctx);   // PSMTXConcat
extern "C" void mkw_ref_8019A91C(CpuContext* ctx);   // PSMTXMultVec

namespace {

constexpr uint32_t kZeroOne = 0x80385A08u;   // {0.0f, 1.0f}: the last column's implicit row

inline const float* F(uint32_t address)
{
    return reinterpret_cast<const float*>(g_mkwGuestArena + (address & GuestFlat::kFoldMask));
}

inline float* W(uint32_t address)
{
    return reinterpret_cast<float*>(g_mkwGuestArena + (address & GuestFlat::kFoldMask));
}

// psq_l with GQR0 (two floats), and with W=1 (one float, 1.0 in ps1).
inline FprVal Pair(const float* p) { return FprVal(p[0], p[1]); }
inline FprVal Single(const float* p) { return FprVal(p[0], 1.0); }

inline void StorePair(float* p, FprVal v)
{
    p[0] = static_cast<float>(v.ps0);
    p[1] = static_cast<float>(v.ps1);
}

void Concat(uint32_t a, uint32_t b, uint32_t ab)
{
    const float* pa = F(a);
    const float* pb = F(b);
    const FprVal a0 = Pair(pa), a1 = Pair(pa + 2), a2 = Pair(pa + 4), a3 = Pair(pa + 6);
    const FprVal a4 = Pair(pa + 8), a5 = Pair(pa + 10);
    const FprVal b0 = Pair(pb), b1 = Pair(pb + 2), b2 = Pair(pb + 4), b3 = Pair(pb + 6);
    const FprVal b4 = Pair(pb + 8), b5 = Pair(pb + 10);
    const FprVal unit = Pair(F(kZeroOne));
    float* out = W(ab);

    // Every input is loaded before the first store, as in the original: ab may
    // be a or b.
    FprVal r00 = PPC_PsMuls0Inline(b0, a0);
    FprVal r01 = PPC_PsMuls0Inline(b1, a0);
    FprVal r10 = PPC_PsMuls0Inline(b0, a2);
    FprVal r11 = PPC_PsMuls0Inline(b1, a2);
    r00 = PPC_PsMadds1Inline(b2, a0, r00);
    r10 = PPC_PsMadds1Inline(b2, a2, r10);
    r01 = PPC_PsMadds1Inline(b3, a0, r01);
    r11 = PPC_PsMadds1Inline(b3, a2, r11);
    r00 = PPC_PsMadds0Inline(b4, a1, r00);
    r01 = PPC_PsMadds0Inline(b5, a1, r01);
    r10 = PPC_PsMadds0Inline(b4, a3, r10);
    r11 = PPC_PsMadds0Inline(b5, a3, r11);
    r01 = PPC_PsMadds1Inline(unit, a1, r01);
    r11 = PPC_PsMadds1Inline(unit, a3, r11);
    FprVal r20 = PPC_PsMuls0Inline(b0, a4);
    FprVal r21 = PPC_PsMuls0Inline(b1, a4);
    r20 = PPC_PsMadds1Inline(b2, a4, r20);
    r21 = PPC_PsMadds1Inline(b3, a4, r21);
    r20 = PPC_PsMadds0Inline(b4, a5, r20);
    r21 = PPC_PsMadds0Inline(b5, a5, r21);
    r21 = PPC_PsMadds1Inline(unit, a5, r21);
    StorePair(out, r00);
    StorePair(out + 2, r01);
    StorePair(out + 4, r10);
    StorePair(out + 6, r11);
    StorePair(out + 8, r20);
    StorePair(out + 10, r21);
}

void MultVec(uint32_t m, uint32_t src, uint32_t dst)
{
    const float* pm = F(m);
    const float* ps = F(src);
    float* out = W(dst);
    const FprVal xy = Pair(ps);
    const FprVal z1 = Single(ps + 2);
    // Loads and stores interleave as in the original (each row's store comes
    // after the next row's loads), so even a dst inside m reads the same.
    const FprVal t0 = PPC_PsMaddInline(Pair(pm + 2), z1, PPC_PsMulInline(Pair(pm), xy));
    const FprVal m4 = Pair(pm + 4), m6 = Pair(pm + 6);
    out[0] = static_cast<float>(PPC_PsSum0Inline(t0, t0, FprVal(0.0)).ps0);
    const FprVal t1 = PPC_PsMaddInline(m6, z1, PPC_PsMulInline(m4, xy));
    const FprVal m8 = Pair(pm + 8), m10 = Pair(pm + 10);
    out[1] = static_cast<float>(PPC_PsSum0Inline(t1, t1, FprVal(0.0)).ps0);
    const FprVal t2 = PPC_PsMaddInline(m10, z1, PPC_PsMulInline(m8, xy));
    out[2] = static_cast<float>(PPC_PsSum0Inline(t2, t2, FprVal(0.0)).ps0);
}

NativeDiff::Check g_concatCheck("PSMTXConcat"), g_multVecCheck("PSMTXMultVec");

} // namespace

void PSMTXConcat_HLE(uint32_t a, uint32_t b, uint32_t ab)
{
    if (!NativeDiff::Enabled()) {
        Concat(a, b, ab);
        return;
    }
    const NativeDiff::Region region = {ab, 48};
    g_concatCheck.Run(&region, 1, [&] { Concat(a, b, ab); }, [&](CpuContext& ctx) {
        ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = ab;
        mkw_ref_80199D64(&ctx);
    });
}

void PSMTXMultVec_HLE(uint32_t m, uint32_t src, uint32_t dst)
{
    if (!NativeDiff::Enabled()) {
        MultVec(m, src, dst);
        return;
    }
    const NativeDiff::Region region = {dst, 12};
    g_multVecCheck.Run(&region, 1, [&] { MultVec(m, src, dst); }, [&](CpuContext& ctx) {
        ctx.gpr[3] = m; ctx.gpr[4] = src; ctx.gpr[5] = dst;
        mkw_ref_8019A91C(&ctx);
    });
}

PPC_NATIVE_OVERRIDE_VOID(80199D64, PSMTXConcat_HLE, (uint32_t a, uint32_t b, uint32_t ab), (a, b, ab));
PPC_NATIVE_OVERRIDE_VOID(8019A91C, PSMTXMultVec_HLE, (uint32_t m, uint32_t src, uint32_t dst), (m, src, dst));
