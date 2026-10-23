// nw4r::snd::detail::VoiceManager::NotifyVoiceUpdate (800AC9F0), native, with
// the Voice::ResetDelta, AxVoice::ResetDelta and AxVoiceParamBlock::SetVoiceMix
// it runs for every channel of every playing voice.
//
// The sound thread's AX callback calls it once per AX frame, inside the audio
// pump: each voice's volume ramp is folded into its current volume and its
// mixer levels are re-sent with their deltas zeroed, so the DSP starts the next
// frame without a ramp. Translated, that was four levels of calls per AX voice
// with an interrupt lock around each level; here it is one walk, taking the
// same locks in the same order.
//
// Layout is MKW's 2008 nw4r, read off the translation; ogws (snd_VoiceManager,
// snd_Voice, snd_AxVoice) has the 2006 source of the same functions.

#define MKW_PIN_ARENA 1

#include "hle_stubs.h"
#include "../native/native_difftest.h"

#include <cstdint>

extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);
extern "C" void mkw_ref_8008A6F0(CpuContext* ctx);   // AxVoice::ResetDelta

namespace {

inline uint8_t* H(uint32_t address) { return g_mkwGuestArena + (address & GuestFlat::kFoldMask); }
inline uint32_t R32(uint32_t a) { return *reinterpret_cast<const uint32_t*>(H(a)); }
inline uint32_t R16(uint32_t a) { return *reinterpret_cast<const uint16_t*>(H(a)); }
inline void W32(uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(H(a)) = v; }
inline void W16(uint32_t a, uint32_t v) { *reinterpret_cast<uint16_t*>(H(a)) = static_cast<uint16_t>(v); }

// AxVoiceParamBlock::SetVoiceMix (8008BFE0) with immediate = true: the 24
// levels into the voice's mix, and the mixer-control bits that say which buses
// are live (and ramping).
void SetVoiceMixImmediate(uint32_t block, const uint16_t (&mix)[24])
{
    static const uint32_t kBits[24] = {
        0x1, 0x5, 0x2, 0x6, 0x10000, 0x50000, 0x20000, 0x60000,
        0x200000, 0xA00000, 0x400000, 0xC00000, 0x4000000, 0x14000000, 0x8000000, 0x18000000,
        0x8, 0x18, 0x80000, 0x180000, 0x1000000, 0x3000000, 0x20000000, 0x60000000,
    };
    const int32_t level = OS__DisableInterrupts_801a65ac();
    const uint32_t vpb = R32(block);
    if (vpb == 0) {
        OS__RestoreInterrupts_801a65d4(level);
        return;
    }
    uint32_t control = 0;
    for (int i = 0; i < 24; ++i) {
        W16(vpb + 60 + 2u * i, mix[i]);
        if (mix[i] != 0)
            control |= kBits[i];
    }
    W32(R32(block) + 52, control);
    const uint32_t sync = R32(block);
    W32(sync + 28, R32(sync + 28) | 18u);   // mixer control and mix changed
    OS__RestoreInterrupts_801a65d4(level);
}

// AxVoice::ResetDelta (8008A6F0).
void AxVoiceResetDelta(uint32_t ax)
{
    const int32_t outer = OS__DisableInterrupts_801a65ac();
    if (R32(ax) == 0) {
        OS__RestoreInterrupts_801a65d4(outer);
        return;
    }
    {
        // AxVoiceParamBlock::UpdateDelta: the ramp so far into the current volume.
        const int32_t inner = OS__DisableInterrupts_801a65ac();
        if (const uint32_t vpb = R32(ax)) {
            const int32_t delta = static_cast<int16_t>(R16(vpb + 148));
            W16(ax + 8, R16(ax + 8) + static_cast<uint32_t>(delta * 96));
            W16(R32(ax) + 146, R16(ax + 8));
            W16(R32(ax) + 148, 0);
            const uint32_t v = R32(ax);
            W32(v + 28, R32(v + 28) | 0x100u);
        }
        OS__RestoreInterrupts_801a65d4(inner);
    }
    // The previous levels, each with a zero delta, in AXPBMIX order.
    static const uint8_t kFrom[12] = {30, 32, 36, 38, 42, 44, 48, 50, 34, 40, 46, 52};
    uint16_t mix[24];
    for (int i = 0; i < 12; ++i) {
        mix[2 * i] = static_cast<uint16_t>(R16(ax + kFrom[i]));
        mix[2 * i + 1] = 0;
    }
    SetVoiceMixImmediate(ax, mix);
    OS__RestoreInterrupts_801a65d4(outer);
}

NativeDiff::Check g_resetCheck("AxVoice::ResetDelta");

// Checked per AX voice: the walk above it only iterates.
void AxVoiceResetDeltaChecked(uint32_t ax)
{
    if (!NativeDiff::Enabled()) {
        AxVoiceResetDelta(ax);
        return;
    }
    const uint32_t vpb = R32(ax);
    const NativeDiff::Region regions[2] = {{ax, 64}, {vpb ? vpb : ax, vpb ? 360u : 64u}};
    g_resetCheck.Run(regions, 2, [&] { AxVoiceResetDelta(ax); }, [&](CpuContext& ctx) {
        ctx.gpr[3] = ax;
        mkw_ref_8008A6F0(&ctx);
    });
}

// Voice::ResetDelta (800AB3F0): every channel of every output.
void VoiceResetDelta(uint32_t voice)
{
    for (int32_t out = 0; out < static_cast<int32_t>(R32(voice + 144)); ++out) {
        uint32_t slot = voice + 4u * out + 12;
        for (int32_t ch = 0; ch < static_cast<int32_t>(R32(voice + 140)); ++ch, slot += 16) {
            if (const uint32_t ax = R32(slot))
                AxVoiceResetDeltaChecked(ax);
        }
    }
}

} // namespace

void VoiceManager_NotifyVoiceUpdate_HLE(uint32_t self)
{
    const int32_t level = OS__DisableInterrupts_801a65ac();
    const uint32_t list = self + 8;   // the voices by priority, linked at +276
    for (uint32_t node = R32(list); node != list;) {
        const uint32_t voice = node - 276;
        node = R32(node);
        VoiceResetDelta(voice);
    }
    OS__RestoreInterrupts_801a65d4(level);
}

PPC_NATIVE_OVERRIDE_VOID(800AC9F0, VoiceManager_NotifyVoiceUpdate_HLE, (uint32_t self), (self));
