// The difftest for native ports of game functions: with the build defining
// MKW_NATIVE_DIFFTEST and /dev_hdd0/tmp/mkwii-native-difftest.txt on the
// console, every call runs the native port, keeps what it wrote, puts the
// memory back, runs the translated function on the same input (on a private
// context and stack) and logs any byte that differs. The translated result is
// the one left in memory, so a wrong port can't steer the game away from the
// run it's being compared with.
#pragma once

#include "RuntimeConfig.h"
#include "guest_flat_memory.h"
#include "isa/ppc_isa_context.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace NativeDiff {

#if defined(MKW_NATIVE_DIFFTEST)
inline bool Enabled()
{
    static int on = -1;
    if (on < 0) {
        FILE* f = std::fopen("/dev_hdd0/tmp/mkwii-native-difftest.txt", "rb");
        on = f != nullptr;
        if (f)
            std::fclose(f);
        if (on)
            std::printf("[native] difftest on: native ports against their translations\n");
    }
    return on != 0;
}
#else
constexpr bool Enabled() { return false; }
#endif

inline uint8_t* Host(uint32_t address)
{
    return MkwGuestArenaBase() + (address & GuestFlat::kFoldMask);
}

// A context for running a translated reference: a scratch stack at fold
// 3E200000 (guest BE200000), the SDA bases, and the live GQRs and FPSCR.
inline bool MakeContext(CpuContext& ctx)
{
    static int backed = -1;
    if (backed < 0)
        backed = GuestFlat::BackShadow(0x3E200000u, 1u << 20, "native-difftest") ? 1 : 0;
    if (!backed)
        return false;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = 0xBE2FFFC0u;
    ctx.gpr[2] = RuntimeConfig::SDA2_BASE;
    ctx.gpr[13] = RuntimeConfig::SDA1_BASE;
    if (const CpuContext* live = TryGetCpuContext()) {
        std::memcpy(ctx.gqr, live->gqr, sizeof ctx.gqr);
        ctx.fpscr = live->fpscr;
    }
    return true;
}

// Guest memory a call may write: saved before the native port runs, so the
// reference starts from the same bytes.
struct Region {
    uint32_t address;
    uint32_t bytes;
};

// RAM, or the difftest's own page (scalar results are compared through it).
inline bool Backed(const Region& r)
{
    const uint32_t fold = r.address & GuestFlat::kFoldMask;
    if (fold >= 0x3E200000u && fold + r.bytes <= 0x3E300000u)
        return true;
    return GuestFlat::RangeBacked(r.address, r.bytes);
}

class Check {
public:
    explicit Check(const char* name) : name_(name) {}

    // Runs native(), then reference(ctx) from the same memory, comparing the
    // regions. Regions may overlap each other; each is at most kMaxBytes.
    template <typename Native, typename Reference>
    void Run(const Region* regions, int count, Native native, Reference reference)
    {
        // Up to 80 KB a region (AX's voice blocks are 70 KB). Allocated on first
        // use (one set per kind of check), so a build without the difftest carries none of it.
        static constexpr uint32_t kMaxBytes = 80 * 1024;
        static uint8_t* buffers = nullptr;
        if (!buffers)
            buffers = new uint8_t[8 * kMaxBytes];
        uint8_t (*before)[kMaxBytes] = reinterpret_cast<uint8_t (*)[kMaxBytes]>(buffers);
        uint8_t (*after)[kMaxBytes] = reinterpret_cast<uint8_t (*)[kMaxBytes]>(buffers + 4 * kMaxBytes);
        if (count > 4)
            count = 4;
        for (int i = 0; i < count; ++i) {
            if (regions[i].bytes > kMaxBytes || !Backed(regions[i])) {
                native();
                ++skipped_;
                return;
            }
            std::memcpy(before[i], Host(regions[i].address), regions[i].bytes);
        }
        native();
        for (int i = 0; i < count; ++i)
            std::memcpy(after[i], Host(regions[i].address), regions[i].bytes);
        for (int i = count - 1; i >= 0; --i)
            std::memcpy(Host(regions[i].address), before[i], regions[i].bytes);
        CpuContext ctx;
        if (!MakeContext(ctx))
            return;
        reference(ctx);
        ++calls_;
        bool same = true;
        for (int i = 0; i < count && same; ++i)
            same = std::memcmp(after[i], Host(regions[i].address), regions[i].bytes) == 0;
        if (!same && mismatches_++ < 16) {
            std::printf("[native] %s differs (call %u):", name_, calls_);
            for (int i = 0; i < count; ++i) {
                const uint8_t* ref = Host(regions[i].address);
                int shown = 0;
                for (uint32_t b = 0; b < regions[i].bytes && shown < 6; ++b) {
                    if (after[i][b] != ref[b]) {
                        std::printf(" %08x:%02x/%02x", regions[i].address + b, after[i][b], ref[b]);
                        ++shown;
                    }
                }
            }
            std::printf("\n");
        }
        if ((calls_ & 0xFFFFu) == 0)
            std::printf("[native] difftest %s: %u calls, %u differed, %u skipped\n", name_, calls_, mismatches_, skipped_);
    }

private:
    const char* name_;
    unsigned calls_ = 0, mismatches_ = 0, skipped_ = 0;
};

} // namespace NativeDiff
