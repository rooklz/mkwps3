// __AXSyncPBs (801277A8), native, with the __AXServiceVPB and __AXDepopVoice it
// calls.
//
// Once per AX frame, inside the audio pump: walk every allocated voice by
// priority, estimate the DSP cycles it would cost, and copy the fields the game
// changed (the sync flags) from the voice's parameter block into the DSP's copy,
// or the DSP's progress back the other way. A voice that would overrun the cycle
// budget is dropped instead, and a voice with a pending depop adds its last
// samples to the depop accumulators. Translated, that was a few hundred calls a
// frame with the register file in between; here it is one native walk.
//
// Offsets and order follow the game's own version (read off the translation;
// ogws's AXVPB.c has the shape, but MKW's differs: no callback stack push in
// ServiceVPB, and the drop path is inlined).

#define MKW_PIN_ARENA 1

#include "RuntimeConfig.h"
#include "hle_stubs.h"
#include "native_difftest.h"

#include <cstdint>
#include <cstring>

extern "C" void DCInvalidateRange_801a1600(uint32_t addr, uint32_t length);
extern "C" void DCFlushRange_801a162c(uint32_t addr, uint32_t length);
extern "C" void mkw_ref_801277A8(CpuContext* ctx);   // __AXSyncPBs

namespace {

constexpr uint32_t kSda = RuntimeConfig::SDA1_BASE;
constexpr uint32_t kMaxVoices = kSda - 26212;
constexpr uint32_t kNumVoices = kSda - 26208;
constexpr uint32_t kRecDspCycles = kSda - 26204;
constexpr uint32_t kMaxDspCycles = kSda - 26200;
constexpr uint32_t kDspPbs = kSda - 26224;          // the DSP's parameter blocks, 320 bytes each
constexpr uint32_t kItds = kSda - 26220;            // 64 bytes each
constexpr uint32_t kCommandListCycles = kSda - 26388;
constexpr uint32_t kDroppedList = kSda - 26480;     // dropped voices, linked through +8
constexpr uint32_t kStackHeads = 0x802F40C0u;       // one list per priority, 0 (free) to 31
constexpr uint32_t kMixCycles = 0x8027E7C0u;        // per mixer-control mode, 32 entries
constexpr uint32_t kRmtMixCycles = 0x8027E840u;     // per remote mixer mode, 4 entries

inline uint8_t* H(uint32_t address) { return g_mkwGuestArena + (address & GuestFlat::kFoldMask); }
inline uint32_t R32(uint32_t a) { return *reinterpret_cast<const uint32_t*>(H(a)); }
inline uint32_t R16(uint32_t a) { return *reinterpret_cast<const uint16_t*>(H(a)); }
inline void W32(uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(H(a)) = v; }
inline void W16(uint32_t a, uint32_t v) { *reinterpret_cast<uint16_t*>(H(a)) = static_cast<uint16_t>(v); }
inline int32_t S16(uint32_t a) { return static_cast<int16_t>(R16(a)); }
inline uint32_t Rotl(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }

inline void Copy16(uint32_t dst, uint32_t src, uint32_t off) { W16(dst + off, R16(src + off)); }
inline void Copy32(uint32_t dst, uint32_t src, uint32_t off) { W32(dst + off, R32(src + off)); }

// The voice's own block lies in RAM; memmove covers the same bytes the game's
// memcpy writes.
inline void CopyBytes(uint32_t dst, uint32_t src, uint32_t n) { std::memmove(H(dst), H(src), n); }

inline uint32_t DspPb(uint32_t vpb) { return R32(kDspPbs) + R32(vpb + 24) * 320u; }

// __AXDepopVoice (801270E4): the block's last outputs into the depop sums.
void DepopVoice(uint32_t pb)
{
    static const int16_t kFrom[20] = {82, 84, 86, 88, 90, 92, 94, 96, 98, 100, 102, 104,
                                      250, 252, 254, 256, 258, 260, 262, 264};
    static const int16_t kSum[20] = {-26228, -26240, -26252, -26264, -26232, -26244, -26256,
                                     -26268, -26236, -26248, -26260, -26272, -26276, -26280,
                                     -26284, -26288, -26292, -26296, -26300, -26304};
    for (int i = 0; i < 20; ++i) {
        const uint32_t sum = kSda + kSum[i];
        W32(sum, R32(sum) + static_cast<uint32_t>(S16(pb + kFrom[i])));
    }
}

// __AXServiceVPB (80127258).
void ServiceVpb(uint32_t vpb)
{
    W32(kNumVoices, R32(kNumVoices) + 1);
    const uint32_t dst = DspPb(vpb);
    const uint32_t src = vpb + 40;
    const uint32_t sync = R32(vpb + 28);

    if (sync == 0) {
        Copy16(src, dst, 16);    // state
        Copy16(src, dst, 106);   // current volume
        Copy16(src, dst, 122);   // current address
        Copy16(src, dst, 124);
        return;
    }
    if (sync & 0x80000000u) {
        CopyBytes(dst, src, 320);
        return;
    }
    if (sync & 0x1u) {
        Copy16(dst, src, 8);
        Copy16(dst, src, 10);
    }
    if (sync & 0x2u)
        Copy32(dst, src, 12);
    if (sync & 0x4u)
        Copy16(dst, src, 16);
    else
        Copy16(src, dst, 16);
    if (sync & 0x8u)
        Copy16(dst, src, 18);
    if (sync & 0x10u)
        CopyBytes(dst + 20, src + 20, 48);
    if (sync & 0x40u) {
        Copy16(dst, src, 78);
        Copy16(dst, src, 80);
    } else if (sync & 0x20u) {
        for (uint32_t off = 68; off <= 80; off += 2)
            Copy16(dst, src, off);
        const uint32_t itd = R32(vpb + 36);
        for (uint32_t off = 0; off < 64; off += 4)
            W32(itd + off, 0);
    }
    if (sync & 0x80u)
        CopyBytes(dst + 82, src + 82, 24);
    if (sync & 0x200u) {
        Copy16(src, dst, 106);
        W16(dst + 108, R16(src + 108));
    } else if (sync & 0x100u) {
        Copy16(dst, src, 106);
        W16(dst + 108, R16(src + 108));
    }
    if (sync & 0x7800u) {
        if (sync & 0x800u)
            Copy16(dst, src, 110);
        if (sync & 0x1000u)
            Copy32(dst, src, 114);
        if (sync & 0x2000u)
            Copy32(dst, src, 118);
        if (sync & 0x4000u)
            Copy32(dst, src, 122);
        else
            Copy32(src, dst, 122);
    } else if (sync & 0x400u) {
        Copy32(dst, src, 110);
        Copy32(dst, src, 114);
        Copy32(dst, src, 118);
        Copy32(dst, src, 122);
    } else {
        Copy16(src, dst, 122);
        Copy16(src, dst, 124);
    }
    if (sync & 0x8000u) {
        for (uint32_t off = 126; off <= 162; off += 4)
            Copy32(dst, src, off);
    }
    if (sync & 0x20000u) {
        Copy16(dst, src, 166);
        Copy16(dst, src, 168);
    } else if (sync & 0x10000u) {
        for (uint32_t off = 166; off <= 178; off += 2)
            Copy16(dst, src, off);
    }
    if (sync & 0x40000u) {
        Copy16(dst, src, 180);
        Copy16(dst, src, 182);
        Copy16(dst, src, 184);
    }
    if (sync & 0x100000u) {
        Copy16(dst, src, 190);
        Copy16(dst, src, 192);
    } else if (sync & 0x80000u) {
        for (uint32_t off = 186; off <= 192; off += 2)
            Copy16(dst, src, off);
    }
    if (sync & 0x400000u) {
        for (uint32_t off = 204; off <= 212; off += 2)
            Copy16(dst, src, off);
    } else if (sync & 0x200000u) {
        for (uint32_t off = 194; off <= 212; off += 2)
            Copy16(dst, src, off);
    }
    if (sync & 0x800000u)
        Copy16(dst, src, 214);
    if (sync & 0x1000000u)
        Copy16(dst, src, 216);
    if (sync & 0x2000000u)
        CopyBytes(dst + 218, src + 218, 32);
    if (sync & 0x4000000u)
        CopyBytes(dst + 250, src + 250, 16);
    if (sync & 0x8000000u)
        CopyBytes(dst + 266, src + 266, 10);
    if (sync & 0x20000000u) {
        Copy16(dst, src, 280);
        Copy16(dst, src, 282);
    } else if (sync & 0x40000000u) {
        for (uint32_t off = 286; off <= 294; off += 2)
            Copy16(dst, src, off);
    } else if (sync & 0x10000000u) {
        for (uint32_t off = 276; off <= 294; off += 2)
            Copy16(dst, src, off);
    }
}

// The DSP cycles a running voice adds (all of its share, sample-rate conversion
// and mixes included).
uint32_t RunningCycles(uint32_t vpb)
{
    uint32_t c = 387;
    if (R16(vpb + 226))
        c += 309;   // low-pass
    if (R16(vpb + 234))
        c += 1024;  // biquad
    if (R16(vpb + 108) == 1)
        c += 27;    // ITD
    const uint32_t select = R16(vpb + 48);
    const uint32_t ratio = (R16(vpb + 206) << 16) | R16(vpb + 208);
    const uint32_t scaled = (((ratio << 9) + 0x10000u - 0x8000u) >> 16) & 0xFFFFu;
    c += select == 0 ? scaled + 1561 : select == 1 ? scaled + 1466 : 605;
    const uint32_t mixer = R32(vpb + 52);
    c += R32(kMixCycles + (Rotl(mixer, 13) & 124)) + R32(kMixCycles + (Rotl(mixer, 2) & 124)) +
         R32(kMixCycles + (Rotl(mixer, 8) & 124)) + R32(kMixCycles + (Rotl(mixer, 18) & 124));
    if (R16(vpb + 254) == 1) {   // remote speaker
        c += 613;
        const uint32_t filter = R16(vpb + 316);
        if (filter == 1)
            c += 118;
        else if (filter == 2)
            c += 834;
        const uint32_t rmt = R16(vpb + 256);
        static const unsigned kRot[8] = {30, 2, 20, 22, 24, 26, 28, 0};
        for (unsigned r : kRot)
            c += R32(kRmtMixCycles + ((r ? Rotl(rmt, r) : rmt) & 12u));
    }
    return c;
}

void SyncPbs(uint32_t baseCycles)
{
    W32(kNumVoices, 0);
    const uint32_t voices = R32(kMaxVoices);
    DCInvalidateRange_801a1600(R32(kDspPbs), voices * 320u);
    DCInvalidateRange_801a1600(R32(kItds), voices << 6);

    uint32_t cycles = R32(kCommandListCycles) + voices * 600u + baseCycles + 32u;
    for (uint32_t prio = 31; prio != 0; --prio) {
        for (uint32_t vpb = R32(kStackHeads + prio * 4u); vpb != 0; vpb = R32(vpb)) {
            if (R16(vpb + 108) == 1)
                cycles += 129;
            if (R32(vpb + 32) != 0)
                DepopVoice(DspPb(vpb));
            if (R16(vpb + 56) == 1) {
                cycles += RunningCycles(vpb);
                if (R32(kMaxDspCycles) > cycles) {
                    ServiceVpb(vpb);
                } else {
                    // Over budget: drop the voice and queue it for its callback.
                    const uint32_t pb = DspPb(vpb);
                    if (R16(pb + 16) == 1)
                        DepopVoice(pb);
                    W16(vpb + 56, 0);
                    W16(pb + 16, 0);
                    W32(vpb + 8, R32(kDroppedList));
                    W32(kDroppedList, vpb);
                }
            } else {
                ServiceVpb(vpb);
            }
            W32(vpb + 28, 0);
            W32(vpb + 32, 0);
        }
    }
    W32(kRecDspCycles, cycles);

    for (uint32_t vpb = R32(kStackHeads); vpb != 0; vpb = R32(vpb)) {
        if (R32(vpb + 32) != 0)
            DepopVoice(DspPb(vpb));
        W32(vpb + 32, 0);
        W16(DspPb(vpb) + 16, 0);
    }

    DCFlushRange_801a162c(R32(kDspPbs), voices * 320u);
    DCFlushRange_801a162c(R32(kItds), voices << 6);
}

NativeDiff::Check g_syncCheck("__AXSyncPBs");

} // namespace

void AX_SyncPBs_HLE(uint32_t baseCycles)
{
    if (!NativeDiff::Enabled()) {
        SyncPbs(baseCycles);
        return;
    }
    // Everything it writes: the DSP blocks, ITD buffers and voices (one span
    // from 802F82C0), and AX's small-data variables.
    const NativeDiff::Region regions[2] = {{0x802F82C0u, 0x80309AC0u - 0x802F82C0u},
                                           {kSda - 26480, 26480 - 26200 + 4}};
    g_syncCheck.Run(regions, 2, [&] { SyncPbs(baseCycles); }, [&](CpuContext& ctx) {
        ctx.gpr[3] = baseCycles;
        mkw_ref_801277A8(&ctx);
    });
}

PPC_NATIVE_OVERRIDE_VOID(801277A8, AX_SyncPBs_HLE, (uint32_t baseCycles), (baseCycles));
