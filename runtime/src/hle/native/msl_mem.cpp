// memcpy and memset, native.
//
// MSL's memcpy copies a byte at a time (forwards, or backwards when the source
// is below the destination, so it is really memmove), and __fill_mem stores a
// word at a time; translated, every byte or word is a separate guest store. Here
// a copy or fill that lies wholly in RAM is the host's memmove or memset, which
// writes the same bytes. Anything else (MMIO, an unbacked address) goes through
// the translated function as before.

#define MKW_PIN_ARENA 1

#include "hle_stubs.h"
#include "native_difftest.h"

#include <cstdint>
#include <cstring>

extern "C" void mkw_ref_80005F34(CpuContext* ctx);   // memcpy
extern "C" void mkw_ref_80005F84(CpuContext* ctx);   // __fill_mem
extern "C" void mkw_ref_80006038(CpuContext* ctx);   // memset

namespace {

inline uint8_t* H(uint32_t address)
{
    return g_mkwGuestArena + (address & GuestFlat::kFoldMask);
}

// RAM on both sides: no MMIO, nothing unbacked.
inline bool InRam(uint32_t address, uint32_t bytes)
{
    return GuestFlat::RangeBacked(address, bytes);
}

// The translated function on the live context, for what the fast path doesn't
// cover. None of the three uses the stack.
template <void (*Ref)(CpuContext*)>
void Fallback(uint32_t r3, uint32_t r4, uint32_t r5)
{
    CpuContext* ctx = TryGetCpuContext();
    const uint32_t s3 = ctx->gpr[3], s4 = ctx->gpr[4], s5 = ctx->gpr[5], s6 = ctx->gpr[6], s7 = ctx->gpr[7];
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    Ref(ctx);
    ctx->gpr[3] = s3;
    ctx->gpr[4] = s4;
    ctx->gpr[5] = s5;
    ctx->gpr[6] = s6;
    ctx->gpr[7] = s7;
}

void Copy(uint32_t dst, uint32_t src, uint32_t n)
{
    if (n == 0)
        return;
    if (InRam(dst, n) && InRam(src, n))
        std::memmove(H(dst), H(src), n);
    else
        Fallback<mkw_ref_80005F34>(dst, src, n);
}

void Fill(uint32_t dst, uint32_t value, uint32_t n)
{
    if (n == 0)
        return;
    if (InRam(dst, n))
        std::memset(H(dst), static_cast<int>(value & 0xFFu), n);
    else
        Fallback<mkw_ref_80005F84>(dst, value, n);
}

NativeDiff::Check g_copyCheck("memcpy"), g_fillCheck("__fill_mem"), g_setCheck("memset");

} // namespace

void MSL_memcpy_HLE(uint32_t dst, uint32_t src, uint32_t n)
{
    if (!NativeDiff::Enabled()) {
        Copy(dst, src, n);
        return;
    }
    // The source is saved too: it may overlap the destination.
    const NativeDiff::Region regions[2] = {{dst, n}, {src, n}};
    g_copyCheck.Run(regions, 2, [&] { Copy(dst, src, n); }, [&](CpuContext& ctx) {
        ctx.gpr[3] = dst; ctx.gpr[4] = src; ctx.gpr[5] = n;
        mkw_ref_80005F34(&ctx);
    });
}

void MSL_fill_mem_HLE(uint32_t dst, uint32_t value, uint32_t n)
{
    if (!NativeDiff::Enabled()) {
        Fill(dst, value, n);
        return;
    }
    const NativeDiff::Region region = {dst, n};
    g_fillCheck.Run(&region, 1, [&] { Fill(dst, value, n); }, [&](CpuContext& ctx) {
        ctx.gpr[3] = dst; ctx.gpr[4] = value; ctx.gpr[5] = n;
        mkw_ref_80005F84(&ctx);
    });
}

// memset returns its destination, which it never changes: r3 already holds it.
void MSL_memset_HLE(uint32_t dst, uint32_t value, uint32_t n)
{
    if (!NativeDiff::Enabled()) {
        Fill(dst, value, n);
        return;
    }
    const NativeDiff::Region region = {dst, n};
    g_setCheck.Run(&region, 1, [&] { Fill(dst, value, n); }, [&](CpuContext& ctx) {
        ctx.gpr[3] = dst; ctx.gpr[4] = value; ctx.gpr[5] = n;
        mkw_ref_80006038(&ctx);
    });
}

PPC_NATIVE_OVERRIDE_VOID(80005F34, MSL_memcpy_HLE, (uint32_t dst, uint32_t src, uint32_t n), (dst, src, n));
PPC_NATIVE_OVERRIDE_VOID(80005F84, MSL_fill_mem_HLE, (uint32_t dst, uint32_t value, uint32_t n), (dst, value, n));
PPC_NATIVE_OVERRIDE_VOID(80006038, MSL_memset_HLE, (uint32_t dst, uint32_t value, uint32_t n), (dst, value, n));

