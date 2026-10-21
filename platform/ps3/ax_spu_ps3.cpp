// The AX voice mix on an SPU: loads spu/ax_spu.c's program onto one SPU and passes it
// jobs. ax_mix.cpp decides what goes there and when; this only moves the jobs.
//
// /dev_hdd0/tmp/mkwii-nospuaudio.txt keeps the mix on the PPU.

#include "spu/ax_spu_job.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ppu-types.h>
#include <sys/spu.h>

extern "C" const char* mkw_app_dir(void);

namespace {

AxSpuJob g_job;
AxSpuResult g_result;
sysSpuImage g_image;
sys_spu_group_t g_group;
sys_spu_thread_t g_thread;
uint32_t g_seq;
int g_state;   // 0 not tried, 1 running, -1 off

inline uint64_t Timebase()
{
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

volatile uint32_t& ResultWord(const uint32_t& field)
{
    return const_cast<volatile uint32_t&>(field);
}

bool Start()
{
    if (FILE* f = std::fopen("/dev_hdd0/tmp/mkwii-nospuaudio.txt", "rb")) {
        std::fclose(f);
        std::printf("[audio] mkwii-nospuaudio.txt present: mixing on the PPU\n");
        return false;
    }
    static uint8_t elf[128 * 1024] __attribute__((aligned(128)));
    char path[300];
    snprintf(path, sizeof path, "%s/ax_spu.elf", mkw_app_dir());
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("[audio] no ax_spu.elf: mixing on the PPU\n");
        return false;
    }
    const size_t n = std::fread(elf, 1, sizeof elf, f);
    std::fclose(f);
    if (n == 0 || n == sizeof elf) {
        std::printf("[audio] ax_spu.elf unreadable (%u bytes): mixing on the PPU\n", static_cast<unsigned>(n));
        return false;
    }

    // Built by hand from the ELF, as spu_vtx.c does: sysSpuImageImport misreports segments.
    static sysSpuSegment segs[16];
    u32 entry = 0, nseg = 0;
    if (sysSpuElfGetInformation(elf, &entry, &nseg) != 0 || nseg == 0 || nseg > 16 ||
        sysSpuElfGetSegments(elf, segs, nseg) != 0) {
        std::printf("[audio] ax_spu.elf has no usable segments: mixing on the PPU\n");
        return false;
    }
    g_image.type = SPU_IMAGE_TYPE_USER;
    g_image.entryPoint = entry;
    g_image.segments = static_cast<u32>(reinterpret_cast<uintptr_t>(segs));
    g_image.segmentCount = nseg;

    sysSpuInitialize(6, 0);   // whichever of this and the vertex SPUs comes first; the other is refused, harmlessly
    static const char groupName[] = "axmix";
    static const char threadName[] = "axmix";
    sysSpuThreadGroupAttribute gattr;
    std::memset(&gattr, 0, sizeof gattr);
    gattr.nsize = sizeof groupName;
    gattr.name = groupName;
    s32 rc = sysSpuThreadGroupCreate(&g_group, 1, 100, &gattr);
    if (rc != 0) {
        std::printf("[audio] no SPU for the mix (group create %08x): mixing on the PPU\n", static_cast<unsigned>(rc));
        return false;
    }
    sysSpuThreadAttribute tattr;
    std::memset(&tattr, 0, sizeof tattr);
    tattr.nsize = sizeof threadName;
    tattr.name = threadName;
    tattr.option = SPU_THREAD_ATTR_NONE;
    sysSpuThreadArgument arg;
    std::memset(&arg, 0, sizeof arg);
    arg.arg0 = static_cast<u64>(reinterpret_cast<uintptr_t>(&g_job));
    arg.arg1 = static_cast<u64>(reinterpret_cast<uintptr_t>(&g_result));
    std::memset(&g_result, 0, sizeof g_result);
    __asm__ volatile("sync" ::: "memory");
    if ((rc = sysSpuThreadInitialize(&g_thread, g_group, 0, &g_image, &tattr, &arg)) != 0 ||
        (rc = sysSpuThreadGroupStart(g_group)) != 0) {
        std::printf("[audio] SPU mix thread failed to start (%08x): mixing on the PPU\n", static_cast<unsigned>(rc));
        return false;
    }

    // It says hello once it is waiting for jobs.
    const uint64_t t0 = Timebase();
    while (ResultWord(g_result.voices) != AX_SPU_HELLO) {
        if (Timebase() - t0 > 79800000ull) {
            std::printf("[audio] SPU mixer never answered: mixing on the PPU\n");
            sysSpuThreadGroupTerminate(g_group, 0);
            return false;
        }
    }
    std::printf("[audio] AX voices mixed on an SPU\n");
    return true;
}

} // namespace

// The job line to fill for the next mix, or nullptr when there is no SPU mixer.
extern "C" AxSpuJob* mkw_ax_spu_job(void)
{
    if (g_state == 0)
        g_state = Start() ? 1 : -1;
    return g_state > 0 ? &g_job : nullptr;
}

// Sends the job filled through mkw_ax_spu_job(). One at a time: wait before the next.
extern "C" void mkw_ax_spu_kick(void)
{
    g_job.seq = ++g_seq;
    g_job.result_ea = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_result));
    __asm__ volatile("sync" ::: "memory");
    sysSpuThreadWriteMb(g_thread, g_seq);
}

// Waits for the job last kicked. nullptr if the SPU stopped answering (then the mix
// stays on the PPU from here on).
extern "C" const AxSpuResult* mkw_ax_spu_wait(void)
{
    if (g_state <= 0)
        return nullptr;
    if (ResultWord(g_result.seq) != g_seq) {
        const uint64_t t0 = Timebase();
        uint32_t spins = 0;
        __asm__ volatile("or 1,1,1");   // low thread priority: the other hardware thread gets the core
        while (ResultWord(g_result.seq) != g_seq) {
            if ((++spins & 255u) == 0 && Timebase() - t0 > 79800ull * 250) {
                __asm__ volatile("or 2,2,2");
                std::printf("[audio] SPU mixer stopped answering (job %u): mixing on the PPU\n", g_seq);
                g_state = -1;
                return nullptr;
            }
        }
        __asm__ volatile("or 2,2,2");
    }
    __asm__ volatile("lwsync" ::: "memory");
    return &g_result;
}
