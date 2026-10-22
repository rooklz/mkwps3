// Windows-style fibers on the PS3 (see win_fiber.h).
//
// ABI (this toolchain): 64-bit ELF with ELFv1 function descriptors; the
// callee-saved set is r1, r2, LR, CR, r14-r31, f14-f31, v20-v31 and VRSAVE,
// which is exactly what fiber_ps3.S swaps. r14 is the pinned guest-arena base
// in translated code; a new fiber starts with it set.

#include "win_fiber.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

extern "C" uint8_t* MkwGuestArenaBase();

extern "C" {
struct FiberCtx {
    uint64_t sp;
    uint64_t toc;
    uint64_t lr;
    uint64_t cr;
    uint64_t gpr[18];
    uint64_t fpr[18];
    uint64_t vrsave;
    uint64_t pad;
    uint8_t vr[12][16];
} __attribute__((aligned(16)));

void fiber_swap(FiberCtx* from, FiberCtx* to);
void fiber_boot(void);
}

namespace {

struct PsFiber {
    FiberCtx ctx;
    void* stack = nullptr;
    size_t stackSize = 0;
    LPFIBER_START_ROUTINE start = nullptr;
    void* parameter = nullptr;
};

PsFiber* g_current = nullptr;

// Translated code nests one host frame per guest call, and PPC64 frames are
// large; 512 KiB per guest thread leaves deep call chains room.
constexpr size_t kMinFiberStack = 512 * 1024;

void FiberEntry(void* p)
{
    PsFiber* f = static_cast<PsFiber*>(p);
    f->start(f->parameter);
    // A fiber procedure must switch away for good rather than return.
    std::printf("[fiber] fiber procedure returned; stopping\n");
    std::abort();
}

void Prime(PsFiber* f)
{
    std::memset(&f->ctx, 0, sizeof f->ctx);
    uintptr_t top = (reinterpret_cast<uintptr_t>(f->stack) + f->stackSize - 256) & ~static_cast<uintptr_t>(15);
    *reinterpret_cast<uint64_t*>(top) = 0;   // null back chain
    f->ctx.sp = top;
    // ELFv1: a function's C address is its descriptor {entry, TOC, env}.
    const uint64_t* boot = reinterpret_cast<const uint64_t*>(reinterpret_cast<const void*>(&fiber_boot));
    f->ctx.toc = boot[1];
    f->ctx.lr = boot[0];
    f->ctx.gpr[0] = reinterpret_cast<uint64_t>(MkwGuestArenaBase());                 // r14
    f->ctx.gpr[1] = reinterpret_cast<uint64_t>(reinterpret_cast<const void*>(&FiberEntry));  // r15
    f->ctx.gpr[2] = reinterpret_cast<uint64_t>(f);                                     // r16
    __asm__ volatile("mfspr %0, 256" : "=r"(f->ctx.vrsave));
}

} // namespace

void* ConvertThreadToFiber(void* parameter)
{
    if (g_current)
        return g_current;
    PsFiber* f = new PsFiber;
    f->parameter = parameter;   // its context is filled by the first switch away
    g_current = f;
    return f;
}

void* CreateFiber(size_t stackSize, LPFIBER_START_ROUTINE start, void* parameter)
{
    PsFiber* f = new PsFiber;
    f->stackSize = stackSize < kMinFiberStack ? kMinFiberStack : stackSize;
    f->stack = memalign(128, f->stackSize);
    if (!f->stack) {
        delete f;
        return nullptr;
    }
    f->start = start;
    f->parameter = parameter;
    Prime(f);
    return f;
}

extern "C" void mkw_hprof_fiber_switch(void* fiber);

void SwitchToFiber(void* fiber)
{
    PsFiber* to = static_cast<PsFiber*>(fiber);
    PsFiber* from = g_current;
    if (!to || to == from)
        return;
    g_current = to;
    mkw_hprof_fiber_switch(to);
    fiber_swap(&from->ctx, &to->ctx);
}

void DeleteFiber(void* fiber)
{
    PsFiber* f = static_cast<PsFiber*>(fiber);
    if (!f || f == g_current)
        return;
    std::free(f->stack);
    delete f;
}

void* GetCurrentFiber()
{
    return g_current;
}

bool ConvertFiberToThread()
{
    // The thread's own fiber record is released; the thread keeps running.
    if (g_current && !g_current->stack) {
        delete g_current;
        g_current = nullptr;
    }
    return true;
}

void* GetFiberData()
{
    return g_current ? g_current->parameter : nullptr;
}
