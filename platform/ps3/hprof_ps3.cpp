// Function-level sampling of native code, for a measurement build.
//
// Built with MKW_HPROF=1 (Makefile.ps3), the renderer and the GX FIFO parser
// are compiled with -finstrument-functions, so every function entry and exit
// in them calls the two hooks below. Instrumenting the whole runtime slows
// start-up past the point of being usable. A thread that has attached keeps a shadow stack of the
// functions it is in; the control port's `hprof <gpu|game> <secs>` samples the
// top of that stack from its own thread and writes what it saw to
// /dev_hdd0/tmp/hprof.bin for tools/ps3-hprof.sh to name. Translated game code
// is not instrumented; the guest profiler (pcs) covers that.
//
// In a normal build nothing calls the hooks and attaching does nothing.

#include <cstdint>
#include <cstdio>
#include <unistd.h>

namespace {

constexpr unsigned kDepth = 128;

struct Stack {
    void* volatile fn[kDepth];
    volatile unsigned depth;
};

Stack g_stacks[2];                  // 0: GPU thread, 1: game thread
__thread Stack* t_stack = nullptr;

} // namespace

extern "C" {

void __cyg_profile_func_enter(void* fn, void* site) __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void* fn, void* site) __attribute__((no_instrument_function));
void mkw_hprof_attach(int which) __attribute__((no_instrument_function));
int mkw_hprof_sample(int which, unsigned ms, const char* path) __attribute__((no_instrument_function));

void __cyg_profile_func_enter(void* fn, void*)
{
    Stack* s = t_stack;
    if (!s)
        return;
    const unsigned d = s->depth;
    if (d < kDepth)
        s->fn[d] = fn;
    s->depth = d + 1;
}

void __cyg_profile_func_exit(void*, void*)
{
    Stack* s = t_stack;
    if (s && s->depth)
        s->depth = s->depth - 1;
}

// Called on the thread to be profiled.
void mkw_hprof_attach(int which)
{
    if (which == 0 || which == 1)
        t_stack = &g_stacks[which];
}

// Samples thread `which` every ~100 us for `ms` and writes pairs of 32-bit
// words: the innermost function and its caller (0 when outside any
// instrumented function). Returns the number of samples.
int mkw_hprof_sample(int which, unsigned ms, const char* path)
{
    if (which != 0 && which != 1)
        return -1;
    static uint32_t buf[2 * 200000];
    const Stack& s = g_stacks[which];
    unsigned n = 0;
    const unsigned want = ms * 10u;
    for (unsigned i = 0; i < want && n + 2 <= sizeof buf / sizeof buf[0]; ++i) {
        usleep(97);
        const unsigned d = s.depth;
        uint32_t top = 0, caller = 0;
        if (d > 0 && d <= kDepth) {
            top = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.fn[d - 1]));
            if (d > 1)
                caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.fn[d - 2]));
        }
        buf[n++] = top;
        buf[n++] = caller;
    }
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return -1;
    std::fwrite(buf, sizeof buf[0], n, f);
    std::fclose(f);
    return static_cast<int>(n / 2);
}

} // extern "C"
