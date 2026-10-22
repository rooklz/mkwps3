// nw4r::snd::detail::Voice::CalcMixParam (800AB760) and the Util pan and
// volume curves it uses, native.
//
// The sound thread runs this for every channel and output of every voice, every
// audio frame: it turns a voice's volumes, sends and pans into the sixteen
// 16-bit mix levels of an AX parameter block. Translated, each call saved and
// restored sixteen FPRs, emulated the condition register for some forty float
// compares, went through the context for each of its eight curve lookups, and
// kept a full-context fallback for its jump table. Here it is the same
// arithmetic, operation for operation in single precision through the helpers
// the translated code uses, on MKW's own layout (the 2008 nw4r, read off the
// translated function; ogws has the 2006 source), with the game's constants
// and curve tables read where the game keeps them.
//
// /dev_hdd0/tmp/mkwii-snd-difftest.txt also runs the translated function on the
// same input (on a private context and stack) and logs any output that differs.

#include "RuntimeConfig.h"
#include "guest_flat_memory.h"
#include "hle_stubs.h"
#include "isa/ppc_isa_float.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kSda2 = RuntimeConfig::SDA2_BASE;
constexpr uint32_t kPanTables = 0x80274B70u;     // float* per pan curve: sqrt, sin/cos, linear
constexpr uint32_t kVolumeTable = 0x80249798u;   // dB -> ratio, 0.1 dB steps from -90.4
constexpr uint32_t kAxManager = 0x802D5730u;     // AxManager::GetInstance(); output mode first

inline const uint8_t* G(uint32_t address)
{
    return MkwGuestArenaBase() + (address & GuestFlat::kFoldMask);
}

inline double F(uint32_t address) { return *reinterpret_cast<const float*>(G(address)); }
inline uint32_t W(uint32_t address) { return *reinterpret_cast<const uint32_t*>(G(address)); }
inline double K(int offset) { return F(kSda2 + static_cast<uint32_t>(offset)); }

inline void Store16(uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint16_t*>(MkwGuestArenaBase() + (address & GuestFlat::kFoldMask)) =
        static_cast<uint16_t>(value);
}

inline int32_t Fctiwz(double value) { return static_cast<int32_t>(PPC_FprLowWordInline(PPC_Fctiwz(value))); }

struct PanInfo {
    uint32_t curve;
    bool centerZero;
    bool zeroClamp;
};

// Util::CalcPanRatio (800ACE00).
double CalcPanRatio(double pan, const PanInfo& info)
{
    double p;
    if (pan > K(-27968))
        p = K(-27968);                                   // 1
    else if (pan < K(-27944))
        p = K(-27944);                                   // -1
    else
        p = pan;
    const double x = PpcFmulsInline(PpcFaddsInline(K(-27968), p), K(-27960));
    const uint32_t table = W(kPanTables + (info.curve << 2));
    const int32_t index = Fctiwz(PpcFaddsInline(K(-27960), PpcFmulsInline(K(-27940), x)));
    double ratio = F(table + (static_cast<uint32_t>(index) << 2));
    if (info.centerZero)
        ratio = PpcFdivsInline(ratio, F(table + 512));
    const double high = info.zeroClamp ? K(-27968) : K(-27964);   // 1 or 2
    if (ratio > high)
        return high;
    if (ratio < K(-27936))
        return K(-27936);                                // 0
    return ratio;
}

// Util::CalcSurroundPanRatio (800ACEE0).
double CalcSurroundPanRatio(double surroundPan, const PanInfo& info)
{
    double s;
    if (surroundPan > K(-27964))
        s = K(-27964);                                   // 2
    else if (surroundPan < K(-27936))
        s = K(-27936);                                   // 0
    else
        s = surroundPan;
    const double x = PpcFmulsInline(s, K(-27960));
    const uint32_t table = W(kPanTables + (info.curve << 2));
    const int32_t index = Fctiwz(PpcFaddsInline(K(-27960), PpcFmulsInline(K(-27940), x)));
    const double ratio = F(table + (static_cast<uint32_t>(index) << 2));
    if (ratio > K(-27964))
        return K(-27964);
    if (ratio < K(-27936))
        return K(-27936);
    return ratio;
}

// Util::CalcVolumeRatio (800ACDA0).
double CalcVolumeRatio(double db)
{
    double d;
    if (db > K(-27956))
        d = K(-27956);                                   // 6 dB
    else if (db < K(-27952))
        d = K(-27952);                                   // -90.4 dB
    else
        d = db;
    const int32_t index = Fctiwz(PpcFmulsInline(K(-27948), d));
    return F(kVolumeTable + (static_cast<uint32_t>(index + 904) << 2));
}

// Voice::TransformDpl2Pan (800AB5C0).
void TransformDpl2Pan(double* outPan, double* outSurroundPan, double pan, double surroundPan)
{
    const double absPan = __builtin_fabs(pan);
    const double s = PpcFsubsInline(surroundPan, K(-28024));   // span - 1
    double resultPan, resultSpan;
    if (absPan <= __builtin_fabs(s)) {
        if (s <= K(-28020)) {
            resultPan = pan;
            resultSpan = PpcFaddsInline(K(-28016), PpcFmulsInline(K(-28012), s));
        } else {
            resultPan = PpcFmulsInline(K(-28008), pan);
            resultSpan = PpcFaddsInline(K(-28016), PpcFmulsInline(K(-28004), s));
        }
    } else if (pan >= K(-28020)) {
        if (s <= K(-28020)) {
            const double ratio = PpcFdivsInline(-s, pan);
            resultPan = PpcFmulsInline(PpcFaddsInline(K(-28000), PpcFmulsInline(K(-27996), ratio)), absPan);
            resultSpan = PpcFaddsInline(K(-28016), PpcFaddsInline(PpcFmulsInline(K(-27992), s),
                                                                  PpcFmulsInline(K(-28012), pan)));
        } else {
            const double ratio = PpcFdivsInline(-s, pan);
            resultPan = PpcFmulsInline(PpcFaddsInline(K(-28000), PpcFmulsInline(K(-27988), ratio)), absPan);
            resultSpan = PpcFaddsInline(K(-28016), PpcFmulsInline(K(-28004), pan));
        }
    } else {
        if (s <= K(-28020)) {
            const double ratio = PpcFdivsInline(-s, pan);
            resultPan = PpcFmulsInline(PpcFsubsInline(PpcFmulsInline(K(-27996), ratio), K(-28000)), absPan);
            resultSpan = PpcFaddsInline(K(-28016), PpcFsubsInline(PpcFmulsInline(K(-27992), s),
                                                                  PpcFmulsInline(K(-28004), pan)));
        } else {
            const double ratio = PpcFdivsInline(-s, pan);
            resultPan = PpcFmulsInline(PpcFsubsInline(PpcFmulsInline(K(-27988), ratio), K(-28000)), absPan);
            resultSpan = PpcFaddsInline(K(-28016), PpcFmulsInline(K(-28004), -pan));
        }
    }
    *outPan = static_cast<float>(resultPan);
    *outSurroundPan = PpcFaddsInline(static_cast<float>(resultSpan), K(-28024));
}

// The u16 mix level of a ratio (CalcMixVolume, inlined in the game).
uint32_t MixVolume(double v)
{
    if (v <= K(-28020))
        return 0;
    uint32_t u = PPC_CvtFp2UnsignedInline(PpcFmulsInline(K(-27976), v));
    if (u >= 65535u)
        u = 65535u;
    return u & 0xFFFFu;
}

void CalcMixParam(uint32_t self, int32_t channel, int32_t voice, uint32_t mix, uint32_t rmtMix)
{
    const uint32_t flags = W(self + 180);
    const uint32_t out = self + static_cast<uint32_t>(voice) * 24u;   // mVoiceOutParam[voice] - 36

    double mainVolume = K(-28020), mainSend = K(-28020), fxA = K(-28020), fxB = K(-28020), fxC = K(-28020);
    if (flags & 1u) {
        mainVolume = F(self + 184);
        mainSend = F(self + 188);
        const double sum = PpcFaddsInline(F(self + 192), F(out + 60));
        if (sum > K(-28024))
            fxA = K(-28024);
        else if (sum < K(-28020))
            fxA = K(-28020);
        else
            fxA = sum;
        fxB = F(self + 196);
        fxC = F(self + 200);
    }
    const double main = PpcFmulsInline(mainVolume, mainSend);
    const double fx_a = PpcFmulsInline(mainVolume, fxA);
    const double fx_b = PpcFmulsInline(mainVolume, fxB);
    const double fx_c = PpcFmulsInline(mainVolume, fxC);

    double remote[4];
    for (int i = 0; i < 4; ++i) {
        double volume = K(-28020), send = K(-28020);
        if (flags & (2u << i)) {
            volume = F(self + 204 + 4u * i);
            send = F(self + 220 + 4u * i);
        }
        remote[i] = PpcFmulsInline(volume, send);
    }

    PanInfo info{0, false, false};
    const uint32_t curve = W(self + 272);
    if (curve <= 8) {
        info.curve = curve / 3;
        info.centerZero = curve % 3 != 0;
        info.zeroClamp = curve % 3 == 2;
    }

    const int32_t channels = static_cast<int32_t>(W(self + 140));
    double left = K(-28020), right = K(-28020), front, rear;
    if (channels > 1 && static_cast<int32_t>(W(self + 268)) == 1) {
        const double pan = PpcFaddsInline(F(self + 168), F(out + 52));
        const double surroundPan = PpcFaddsInline(F(self + 172), F(out + 56));
        if (channel == 0)
            left = CalcPanRatio(pan, info);
        else if (channel == 1)
            right = CalcPanRatio(-pan, info);
        front = CalcSurroundPanRatio(surroundPan, info);
        rear = CalcSurroundPanRatio(PpcFsubsInline(K(-27992), surroundPan), info);
    } else {
        double voicePan = K(-28020);
        if (channels == 2) {
            if (channel == 0)
                voicePan = K(-27984);                    // -1
            if (channel == 1)
                voicePan = K(-28024);                    // 1
        }
        double pan, surroundPan;
        const double basePan = PpcFaddsInline(PpcFaddsInline(F(self + 168), voicePan), F(out + 52));
        const double baseSurroundPan = PpcFaddsInline(F(self + 172), F(out + 56));
        if (static_cast<int32_t>(W(kAxManager)) == 2)
            TransformDpl2Pan(&pan, &surroundPan, basePan, baseSurroundPan);
        else {
            pan = static_cast<float>(basePan);
            surroundPan = static_cast<float>(baseSurroundPan);
        }
        left = CalcPanRatio(pan, info);
        right = CalcPanRatio(-pan, info);
        front = CalcSurroundPanRatio(surroundPan, info);
        rear = CalcSurroundPanRatio(PpcFsubsInline(K(-27992), surroundPan), info);
    }

    const double surround = CalcVolumeRatio(K(-27980));   // -3 dB
    const double lrMixed = PpcFmulsInline(K(-28008), PpcFaddsInline(left, right));

    // vL vR vS, vAuxAL vAuxAR vAuxAS, vAuxBL vAuxBR vAuxBS, vAuxCL vAuxCR vAuxCS.
    double v[12];
    const double zero = K(-28020);
    switch (static_cast<int32_t>(W(kAxManager))) {
    case 0: {   // stereo
        v[0] = PpcFmulsInline(main, left);  v[1] = PpcFmulsInline(main, right);  v[2] = zero;
        v[3] = PpcFmulsInline(fx_a, left);  v[4] = PpcFmulsInline(fx_a, right);  v[5] = zero;
        v[6] = PpcFmulsInline(fx_b, left);  v[7] = PpcFmulsInline(fx_b, right);  v[8] = zero;
        v[9] = PpcFmulsInline(fx_c, left);  v[10] = PpcFmulsInline(fx_c, right); v[11] = zero;
        break;
    }
    case 3: {   // mono
        v[0] = v[1] = PpcFmulsInline(main, lrMixed);  v[2] = zero;
        v[3] = v[4] = PpcFmulsInline(fx_a, lrMixed);  v[5] = zero;
        v[6] = v[7] = PpcFmulsInline(fx_b, lrMixed);  v[8] = zero;
        v[9] = v[10] = PpcFmulsInline(fx_c, lrMixed); v[11] = zero;
        break;
    }
    case 1: {   // surround
        const double fl = PpcFmulsInline(left, front), fr = PpcFmulsInline(right, front);
        const double rs = PpcFmulsInline(surround, rear);
        v[0] = PpcFmulsInline(main, fl); v[1] = PpcFmulsInline(main, fr); v[2] = PpcFmulsInline(main, rs);
        v[3] = PpcFmulsInline(fx_a, fl); v[4] = PpcFmulsInline(fx_a, fr); v[5] = PpcFmulsInline(fx_a, rs);
        v[6] = PpcFmulsInline(fx_b, fl); v[7] = PpcFmulsInline(fx_b, fr); v[8] = PpcFmulsInline(fx_b, rs);
        v[9] = PpcFmulsInline(fx_c, fl); v[10] = PpcFmulsInline(fx_c, fr); v[11] = PpcFmulsInline(fx_c, rs);
        break;
    }
    case 2: {   // DPL2: the S and C slots carry the rear pair
        const double fl = PpcFmulsInline(left, front), fr = PpcFmulsInline(right, front);
        const double rl = PpcFmulsInline(left, rear), rr = PpcFmulsInline(right, rear);
        v[0] = PpcFmulsInline(main, fl); v[1] = PpcFmulsInline(main, fr); v[2] = PpcFmulsInline(main, rl);
        v[3] = PpcFmulsInline(fx_a, fl); v[4] = PpcFmulsInline(fx_a, fr); v[5] = PpcFmulsInline(fx_a, rl);
        v[6] = PpcFmulsInline(fx_b, fl); v[7] = PpcFmulsInline(fx_b, fr); v[8] = PpcFmulsInline(fx_b, rl);
        v[9] = PpcFmulsInline(main, rr); v[10] = PpcFmulsInline(fx_a, rr); v[11] = PpcFmulsInline(fx_b, rr);
        break;
    }
    default:    // no other output mode exists
        for (double& level : v)
            level = zero;
        break;
    }

    for (int i = 0; i < 12; ++i)
        Store16(mix + 2u * i, MixVolume(v[i]));
    for (int i = 0; i < 4; ++i) {
        Store16(rmtMix + 4u * i, MixVolume(PpcFmulsInline(lrMixed, remote[i])));
        Store16(rmtMix + 4u * i + 2, 0);
    }
}

} // namespace

// --- the difftest ------------------------------------------------------------------

#if defined(MKW_SND_DIFFTEST)
extern "C" void mkw_ref_800AB760(CpuContext* ctx);
#endif

namespace {

bool DifftestEnabled()
{
#if defined(MKW_SND_DIFFTEST)
    static int on = -1;
    if (on < 0) {
        FILE* f = std::fopen("/dev_hdd0/tmp/mkwii-snd-difftest.txt", "rb");
        on = f != nullptr;
        if (f)
            std::fclose(f);
        if (on)
            std::printf("[snd] difftest on: Voice::CalcMixParam against its translation\n");
    }
    return on != 0;
#else
    return false;
#endif
}

#if defined(MKW_SND_DIFFTEST)
// The translated function on a context of its own: a scratch stack at fold
// 3E000000 (guest BE000000), the SDA bases, the GQRs OSInitFastCast sets.
void Reference(uint32_t self, int32_t channel, int32_t voice, uint32_t mix, uint32_t rmtMix)
{
    static bool backed = false;
    if (!backed) {
        backed = GuestFlat::BackShadow(0x3E000000u, 1u << 20, "snd-difftest");
        if (!backed)
            return;
    }
    static const uint32_t kGqr[8] = {0, 0, 0x00040004u, 0x00050005u, 0x00060006u, 0x00070007u, 0, 0};
    CpuContext ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = 0xBE0FFFC0u;
    ctx.gpr[2] = RuntimeConfig::SDA2_BASE;
    ctx.gpr[13] = RuntimeConfig::SDA1_BASE;
    ctx.gpr[3] = self;
    ctx.gpr[4] = static_cast<uint32_t>(channel);
    ctx.gpr[5] = static_cast<uint32_t>(voice);
    ctx.gpr[6] = mix;
    ctx.gpr[7] = rmtMix;
    std::memcpy(ctx.gqr, kGqr, sizeof kGqr);
    if (const CpuContext* live = TryGetCpuContext())
        ctx.fpscr = live->fpscr;
    mkw_ref_800AB760(&ctx);
}

void CompareWithReference(uint32_t self, int32_t channel, int32_t voice, uint32_t mix, uint32_t rmtMix)
{
    static unsigned calls = 0, mismatches = 0;
    uint8_t native[40], reference[40];
    std::memcpy(native, G(mix), 24);
    std::memcpy(native + 24, G(rmtMix), 16);
    Reference(self, channel, voice, mix, rmtMix);
    std::memcpy(reference, G(mix), 24);
    std::memcpy(reference + 24, G(rmtMix), 16);
    ++calls;
    if (std::memcmp(native, reference, sizeof native) != 0) {
        if (mismatches++ < 16) {
            std::printf("[snd] CalcMixParam differs (call %u, voice %08x ch %d out %d, mode %u curve %u):",
                        calls, self, channel, voice, W(kAxManager), W(self + 272));
            for (int i = 0; i < 20; ++i) {
                const unsigned n = (native[2 * i] << 8) | native[2 * i + 1];
                const unsigned r = (reference[2 * i] << 8) | reference[2 * i + 1];
                if (n != r)
                    std::printf(" [%d] %04x/%04x", 2 * i, n, r);
            }
            std::printf("\n");
        }
    }
    if ((calls & 0xFFFFu) == 0)
        std::printf("[snd] difftest: %u calls, %u differed\n", calls, mismatches);
    // The reference's output stays: identical when they agree.
}
#endif

} // namespace

void Voice_CalcMixParam_HLE(uint32_t self, uint32_t channel, uint32_t voice, uint32_t mix, uint32_t rmtMix)
{
    CalcMixParam(self, static_cast<int32_t>(channel), static_cast<int32_t>(voice), mix, rmtMix);
#if defined(MKW_SND_DIFFTEST)
    if (DifftestEnabled())
        CompareWithReference(self, static_cast<int32_t>(channel), static_cast<int32_t>(voice), mix, rmtMix);
#else
    (void)DifftestEnabled;
#endif
}

PPC_NATIVE_OVERRIDE_VOID(800AB760, Voice_CalcMixParam_HLE,
                         (uint32_t self, uint32_t channel, uint32_t voice, uint32_t mix, uint32_t rmtMix),
                         (self, channel, voice, mix, rmtMix));
