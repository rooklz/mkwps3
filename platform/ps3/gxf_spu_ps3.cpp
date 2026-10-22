// The GX front end on an SPU: loads spu/gxf_spu.c's program onto one SPU and points it
// at the control block gx_hw.cpp owns. gx_hw.cpp decides whether to use it.

#include "../../runtime/src/hle/gxhw/gxf_shared.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ppu-types.h>
#include <sys/spu.h>

extern "C" const char* mkw_app_dir(void);

namespace {

sysSpuImage g_image;
sys_spu_group_t g_group;
sys_spu_thread_t g_thread;

inline uint64_t Timebase()
{
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

} // namespace

// Starts the parser on `ctl`, whose setup line is filled in. False leaves the parse on the PPU.
extern "C" bool mkw_gxf_spu_start(GxfControl* ctl)
{
    static uint8_t elf[128 * 1024] __attribute__((aligned(128)));
    char path[300];
    snprintf(path, sizeof path, "%s/gxf_spu.elf", mkw_app_dir());
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("[gxf] no gxf_spu.elf: parsing on the PPU\n");
        return false;
    }
    const size_t n = std::fread(elf, 1, sizeof elf, f);
    std::fclose(f);
    if (n == 0 || n == sizeof elf) {
        std::printf("[gxf] gxf_spu.elf unreadable (%u bytes): parsing on the PPU\n", static_cast<unsigned>(n));
        return false;
    }

    // Built by hand from the ELF, as spu_vtx.c does: sysSpuImageImport misreports segments.
    static sysSpuSegment segs[16];
    u32 entry = 0, nseg = 0;
    if (sysSpuElfGetInformation(elf, &entry, &nseg) != 0 || nseg == 0 || nseg > 16 ||
        sysSpuElfGetSegments(elf, segs, nseg) != 0) {
        std::printf("[gxf] gxf_spu.elf has no usable segments: parsing on the PPU\n");
        return false;
    }
    g_image.type = SPU_IMAGE_TYPE_USER;
    g_image.entryPoint = entry;
    g_image.segments = static_cast<u32>(reinterpret_cast<uintptr_t>(segs));
    g_image.segmentCount = nseg;

    sysSpuInitialize(6, 0);   // whichever SPU user comes first; the others are refused, harmlessly
    static const char groupName[] = "gxfront";
    static const char threadName[] = "gxfront";
    sysSpuThreadGroupAttribute gattr;
    std::memset(&gattr, 0, sizeof gattr);
    gattr.nsize = sizeof groupName;
    gattr.name = groupName;
    s32 rc = sysSpuThreadGroupCreate(&g_group, 1, 100, &gattr);
    if (rc != 0) {
        std::printf("[gxf] no SPU for the parser (group create %08x): parsing on the PPU\n", static_cast<unsigned>(rc));
        return false;
    }
    sysSpuThreadAttribute tattr;
    std::memset(&tattr, 0, sizeof tattr);
    tattr.nsize = sizeof threadName;
    tattr.name = threadName;
    tattr.option = SPU_THREAD_ATTR_NONE;
    sysSpuThreadArgument arg;
    std::memset(&arg, 0, sizeof arg);
    arg.arg0 = static_cast<u64>(reinterpret_cast<uintptr_t>(ctl));
    __asm__ volatile("sync" ::: "memory");
    if ((rc = sysSpuThreadInitialize(&g_thread, g_group, 0, &g_image, &tattr, &arg)) != 0 ||
        (rc = sysSpuThreadGroupStart(g_group)) != 0) {
        std::printf("[gxf] SPU parser failed to start (%08x): parsing on the PPU\n", static_cast<unsigned>(rc));
        return false;
    }

    const uint64_t t0 = Timebase();
    while (ctl->hello != GXF_HELLO) {
        if (Timebase() - t0 > 79800000ull) {
            std::printf("[gxf] SPU parser never answered: parsing on the PPU\n");
            sysSpuThreadGroupTerminate(g_group, 0);
            return false;
        }
    }
    std::printf("[gxf] GX commands parsed on an SPU\n");
    return true;
}
