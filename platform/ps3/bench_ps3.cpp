// Microbenchmarks for the instruction patterns translated code leans on, run
// on the console from the control port (`bench`). Each result is PPE cycles
// per iteration, taken from the 79.8 MHz timebase (one tick is 40 cycles at
// 3.2 GHz), so they are only as clean as the other hardware thread lets them
// be: run with the guest idle for steady numbers.

#include <cstdint>
#include <cstdio>
#include <stdio.h>

namespace {

inline uint64_t Timebase()
{
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

constexpr uint32_t kIters = 2000000;

double CyclesPer(uint64_t t0, uint64_t t1, uint32_t iters)
{
    return static_cast<double>(t1 - t0) * 40.0 / static_cast<double>(iters);
}

// Stand-in for the translator's context: guest registers in memory.
struct Ctx {
    uint32_t gpr[32];
    uint32_t cr, xer, lr, ctr;
};

// The ctx ABI: the caller spills what the callee may read, the callee loads
// it, works, stores its results, and the caller reloads them.
__attribute__((noinline)) void CalleeCtx(Ctx* c)
{
    uint32_t r3 = c->gpr[3], r4 = c->gpr[4], r5 = c->gpr[5], r6 = c->gpr[6];
    uint32_t r1 = c->gpr[1], r31 = c->gpr[31];
    r3 = r3 + r4 * r5 + r6;
    r4 = r3 ^ r31;
    c->gpr[3] = r3;
    c->gpr[4] = r4;
    c->gpr[1] = r1;
}

// The same work with guest registers passed and returned in host registers.
__attribute__((noinline)) uint64_t CalleeRegs(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r31)
{
    r3 = r3 + r4 * r5 + r6;
    r4 = r3 ^ r31;
    return (static_cast<uint64_t>(r3) << 32) | r4;
}

double BenchLhs()
{
    volatile uint32_t slot = 0;
    uint32_t v = 1;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        slot = v;
        v = slot + 1;   // load right behind the store to the same word
    }
    return CyclesPer(t0, Timebase(), kIters);
}

double BenchNoLhs()
{
    volatile uint32_t slots[64] = {};
    uint32_t v = 1;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        slots[i & 31] = v;
        v = slots[((i + 16) & 31) + 32] + 1;   // load from a line not just stored
    }
    return CyclesPer(t0, Timebase(), kIters);
}

double BenchCallCtx()
{
    Ctx c{};
    uint32_t r3 = 1, r4 = 2, r5 = 3, r6 = 4, r1 = 0x80001000u, r31 = 7, r0 = 0, r7 = 0, r8 = 0, r9 = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        // What a translated call site does: spill the volatile set, call,
        // reload it.
        c.gpr[0] = r0; c.gpr[1] = r1; c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5;
        c.gpr[6] = r6; c.gpr[7] = r7; c.gpr[8] = r8; c.gpr[9] = r9; c.gpr[31] = r31;
        c.cr = i; c.xer = 0;
        CalleeCtx(&c);
        r0 = c.gpr[0]; r1 = c.gpr[1]; r3 = c.gpr[3]; r4 = c.gpr[4]; r5 = c.gpr[5];
        r6 = c.gpr[6]; r7 = c.gpr[7]; r8 = c.gpr[8]; r9 = c.gpr[9]; r31 = c.gpr[31];
        __asm__ volatile("" ::: "memory");
    }
    const uint64_t t1 = Timebase();
    if (r3 == 0x12345678u)
        std::printf("%u\n", r4);
    return CyclesPer(t0, t1, kIters);
}

double BenchCallRegs()
{
    uint32_t r3 = 1, r4 = 2, r5 = 3, r6 = 4, r31 = 7;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        const uint64_t r = CalleeRegs(r3, r4, r5, r6, r31);
        r3 = static_cast<uint32_t>(r >> 32);
        r4 = static_cast<uint32_t>(r);
        __asm__ volatile("" ::: "memory");
    }
    const uint64_t t1 = Timebase();
    if (r3 == 0x12345678u)
        std::printf("%u\n", r4);
    return CyclesPer(t0, t1, kIters);
}

double BenchMfcrMtcrf()
{
    uint32_t acc = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        uint32_t cr;
        __asm__ volatile("mfcr %0" : "=r"(cr));
        acc += cr;
        __asm__ volatile("mtcrf 0x38,%0" : : "r"(acc) : "cr2", "cr3", "cr4");
    }
    return CyclesPer(t0, Timebase(), kIters);
}

double BenchVarShift()
{
    uint32_t v = 0x12345678u, s = 3, acc = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        uint32_t r;
        __asm__ volatile("slw %0,%1,%2" : "=r"(r) : "r"(v), "r"(s));
        acc += r;
        s = (s + 1) & 31;
    }
    if (acc == 1)
        std::printf("x\n");
    return CyclesPer(t0, Timebase(), kIters);
}

double BenchImmShift()
{
    uint32_t v = 0x12345678u, acc = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        uint32_t r;
        __asm__ volatile("slwi %0,%1,3" : "=r"(r) : "r"(v + i));
        acc += r;
    }
    if (acc == 1)
        std::printf("x\n");
    return CyclesPer(t0, Timebase(), kIters);
}

double BenchLha(bool algebraic)
{
    static int16_t data[64];
    for (int i = 0; i < 64; ++i)
        data[i] = static_cast<int16_t>(i - 32);
    int32_t acc = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        int32_t r;
        const int16_t* p = &data[i & 63];
        if (algebraic)
            __asm__ volatile("lha %0,0(%1)" : "=r"(r) : "b"(p));
        else
            __asm__ volatile("lhz %0,0(%1)\n\textsh %0,%0" : "=r"(r) : "b"(p));
        acc += r;
    }
    if (acc == 1)
        std::printf("x\n");
    return CyclesPer(t0, Timebase(), kIters);
}

// A store followed at a distance of `gap` independent instructions by a load
// of the same word: how long the LHS window is.
double BenchLhsGap()
{
    volatile uint32_t slot = 0;
    uint32_t v = 1, a = 0, b = 0, c = 0, d = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        slot = v;
        a += i; b ^= a; c += b; d ^= c; a += d; b ^= a; c += b; d ^= c;
        a += i; b ^= a; c += b; d ^= c; a += d; b ^= a; c += b; d ^= c;
        a += i; b ^= a; c += b; d ^= c; a += d; b ^= a; c += b; d ^= c;
        v = slot + (d & 1);
    }
    if (v == 0x12345u)
        std::printf("x\n");
    return CyclesPer(t0, Timebase(), kIters);
}

// Reading the timebase, which every profiler transition does twice.
double BenchMftb()
{
    uint64_t acc = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i)
        acc += Timebase();
    if (acc == 1)
        std::printf("x\n");
    return CyclesPer(t0, Timebase(), kIters);
}

// int -> float: the PPE has no GPR-to-FPR move, so this goes through memory.
double BenchIntToFloat()
{
    float acc = 0.0f;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        volatile int32_t v = static_cast<int32_t>(i);
        acc += static_cast<float>(v);
    }
    if (acc == 1.0f)
        std::printf("x\n");
    return CyclesPer(t0, Timebase(), kIters);
}

// float -> int, the other direction through memory.
double BenchFloatToInt()
{
    int32_t acc = 0;
    float f = 1.5f;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i) {
        acc += static_cast<int32_t>(f);
        f += 1.0f;
    }
    if (acc == 1)
        std::printf("x\n");
    return CyclesPer(t0, Timebase(), kIters);
}

// An indirect call through a function pointer, as the GX sink makes per
// register write.
__attribute__((noinline)) void Sink(volatile uint32_t* p, uint32_t v)
{
    *p = v;
}
void (*volatile g_sink)(volatile uint32_t*, uint32_t) = Sink;

double BenchIndirectCall()
{
    volatile uint32_t slot = 0;
    const uint64_t t0 = Timebase();
    for (uint32_t i = 0; i < kIters; ++i)
        g_sink(&slot, i);
    return CyclesPer(t0, Timebase(), kIters);
}

} // namespace

extern "C" int mkw_bench(char* out, unsigned cap)
{
    const double lhs = BenchLhs();
    const double nolhs = BenchNoLhs();
    const double lhsGap = BenchLhsGap();
    const double callCtx = BenchCallCtx();
    const double callRegs = BenchCallRegs();
    const double cr = BenchMfcrMtcrf();
    const double vshift = BenchVarShift();
    const double ishift = BenchImmShift();
    const double lha = BenchLha(true);
    const double lhz = BenchLha(false);
    const double mftb = BenchMftb();
    const double i2f = BenchIntToFloat();
    const double f2i = BenchFloatToInt();
    const double icall = BenchIndirectCall();
    return snprintf(out, cap,
                         "cycles/iter: store+load same word %.1f, different line %.1f, same word after 24 ops %.1f\n"
                         "call via ctx %.1f, call via registers %.1f\n"
                         "mfcr+mtcrf %.1f, slw by register %.1f, slwi %.1f, lha %.1f, lhz+extsh %.1f\n"
                         "mftb %.1f, int->float %.1f, float->int %.1f, indirect call %.1f\n",
                         lhs, nolhs, lhsGap, callCtx, callRegs, cr, vshift, ishift, lha, lhz, mftb, i2f, f2i,
                         icall);
}
