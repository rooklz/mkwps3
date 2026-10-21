#pragma once
// The AX voice path: one PB's frame of samples decoded, resampled, filtered and mixed into
// the buses, and the PB layouts it is read and written through. Used by AXWii (ax_mix.cpp)
// and, as the reference the SPU mixer is held to, by tests/audio/ax_spu_test.cpp.

#include "ax_internal.h"
#include "ax_mix_kernels.h"
#include "isa/big_endian.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace AxDspHle {

inline AXMixControl ConvertMixerControl(uint32_t mixerControl) {
    uint32_t ret = 0;
    if (mixerControl & 0x00000001) ret |= MIX_MAIN_L;
    if (mixerControl & 0x00000002) ret |= MIX_MAIN_R;
    if (mixerControl & 0x00000004) ret |= MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP;
    if (mixerControl & 0x00000008) ret |= MIX_MAIN_S;
    if (mixerControl & 0x00000010) ret |= MIX_MAIN_S | MIX_MAIN_S_RAMP;
    if (mixerControl & 0x00010000) ret |= MIX_AUXA_L;
    if (mixerControl & 0x00020000) ret |= MIX_AUXA_R;
    if (mixerControl & 0x00040000) ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (mixerControl & 0x00080000) ret |= MIX_AUXA_S;
    if (mixerControl & 0x00100000) ret |= MIX_AUXA_S | MIX_AUXA_S_RAMP;
    if (mixerControl & 0x00200000) ret |= MIX_AUXB_L;
    if (mixerControl & 0x00400000) ret |= MIX_AUXB_R;
    if (mixerControl & 0x00800000) ret |= MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (mixerControl & 0x01000000) ret |= MIX_AUXB_S;
    if (mixerControl & 0x02000000) ret |= MIX_AUXB_S | MIX_AUXB_S_RAMP;
    if (mixerControl & 0x04000000) ret |= MIX_AUXC_L;
    if (mixerControl & 0x08000000) ret |= MIX_AUXC_R;
    if (mixerControl & 0x10000000) ret |= MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP;
    if (mixerControl & 0x20000000) ret |= MIX_AUXC_S;
    if (mixerControl & 0x40000000) ret |= MIX_AUXC_S | MIX_AUXC_S_RAMP;
    return static_cast<AXMixControl>(ret);
}

// Both directions take their offsets in the same order - guest side first,
// then host side - so a mirrored read/write pair reads the same way round.
inline void CopyGuestToStruct(uint32_t addr, size_t guestOffset, char* dst, size_t structOffset,
                              size_t byteCount) {
    if (const uint8_t* guest =
            MixResolveRange(addr + static_cast<uint32_t>(guestOffset), byteCount)) {
        for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
            uint16_t value = 0;
            std::memcpy(&value, guest + i, sizeof(value));
            *reinterpret_cast<uint16_t*>(dst + structOffset + i) = MemoryInline::ByteSwap16(value);
        }
        return;
    }
    for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
        *reinterpret_cast<uint16_t*>(dst + structOffset + i) =
            MixRead16(addr + static_cast<uint32_t>(guestOffset + i));
    }
}

inline void CopyStructToGuest(uint32_t addr, size_t guestOffset, const char* src, size_t structOffset,
                              size_t byteCount) {
    if (uint8_t* guest =
            MixResolveRange(addr + static_cast<uint32_t>(guestOffset), byteCount)) {
        for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
            BigEndian::Write16(
                guest + i, *reinterpret_cast<const uint16_t*>(src + structOffset + i));
        }
        return;
    }
    for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
        MixWrite16(addr + static_cast<uint32_t>(guestOffset + i),
                   *reinterpret_cast<const uint16_t*>(src + structOffset + i));
    }
}

// One run of halfwords that is present in both the guest parameter block and
// AXPBWii, possibly at different offsets because the guest layout omits a
// field this struct carries.
struct PBSegment {
    size_t guestOffset;
    size_t structOffset;
    size_t byteCount;
};

// The segmentation is a property of the ucode's PB layout, not of the
// direction of travel, so reads and writes share this one table. Returns the
// number of segments written into `out`.
inline size_t PBSegmentsForLayout(PBLayout layout, std::array<PBSegment, 3>& out) {
    constexpr size_t pbBytes = sizeof(AXPBWii);
    constexpr size_t updatesBegin = offsetof(AXPBWii, updates);
    constexpr size_t updatesEnd = offsetof(AXPBWii, updates) + sizeof(PBUpdatesWii);
    constexpr size_t gapBegin = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
    constexpr size_t gapEnd = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);

    switch (layout) {
    case PBLayout::SkipBiquadGapOnly:
        out[0] = {0, 0, gapBegin};
        out[1] = {gapBegin, gapEnd, pbBytes - gapEnd};
        return 2;
    case PBLayout::SkipUpdatesAndBiquadGap:
        out[0] = {0, 0, updatesBegin};
        out[1] = {updatesBegin, updatesEnd, gapBegin - updatesEnd};
        out[2] = {gapBegin, gapEnd, pbBytes - gapEnd};
        return 3;
    case PBLayout::SkipUpdatesOnly:
    case PBLayout::Unknown:
    default:
        // Unknown means detection has not settled yet. ReadPB deliberately
        // falls back to the SkipUpdatesOnly shape so it has something to
        // score candidates against; WritePBWithLayout refuses instead,
        // because guessing a segmentation on the way back out would corrupt
        // the guest's parameter block.
        out[0] = {0, 0, updatesBegin};
        out[1] = {updatesBegin, updatesEnd, pbBytes - updatesEnd};
        return 2;
    }
}

inline void ReadPBWithLayout(uint32_t addr, AXPBWii& pb, PBLayout layout) {
    std::memset(&pb, 0, sizeof(pb));
    char* dst = reinterpret_cast<char*>(&pb);
    std::array<PBSegment, 3> segments{};
    const size_t count = PBSegmentsForLayout(layout, segments);
    for (size_t i = 0; i < count; ++i) {
        CopyGuestToStruct(addr, segments[i].guestOffset, dst, segments[i].structOffset,
                          segments[i].byteCount);
    }
}

inline void WritePBWithLayout(uint32_t addr, const AXPBWii& pb, PBLayout layout) {
    if (layout == PBLayout::Unknown) {
        // See PBSegmentsForLayout: no segmentation is known to be correct,
        // so nothing is written back. WritePB refuses earlier for the same
        // reason; this is the guard that makes it safe on its own.
        return;
    }
    const char* src = reinterpret_cast<const char*>(&pb);
    std::array<PBSegment, 3> segments{};
    const size_t count = PBSegmentsForLayout(layout, segments);
    for (size_t i = 0; i < count; ++i) {
        CopyStructToGuest(addr, segments[i].guestOffset, src, segments[i].structOffset,
                          segments[i].byteCount);
    }
}

// Mirrors GetInputSamples' coeffs += pb.coef_select * 0x200, but bounds-checked since
// coef_select is a guest halfword; a corrupt PB degrades to linear interpolation instead
// of an out-of-bounds read.
inline const int16_t* VoiceCoefficients(const int16_t* base, uint16_t coefSelect) {
    if (base == nullptr) {
        return nullptr;
    }
    const uint32_t offset = static_cast<uint32_t>(coefSelect) * 0x200u;
    if (offset + 0x200u > kResamplingCoefficientCount) {
        return nullptr;
    }
    return base + offset;
}

// `coeffs` is the DROM base; the bank is selected here, per voice, exactly
// where GetInputSamples selects it.
inline void Resample(Accelerator& accel, AXPBWii& pb, int16_t* out, uint32_t count,
                     const int16_t* coeffs, bool coeffsFit32) {
    // The resampler consumes a number of source samples fixed by the ratio and the starting
    // fraction (one per output sample when it does not interpolate). Decode exactly those in
    // one run, then resample from the buffer: the same samples, read the same way.
    const uint32_t ratio = Hilo(pb.src.ratio_hi, pb.src.ratio_lo);
    const uint64_t needed = (pb.src_type == 0 || pb.src_type == 1)
        ? (static_cast<uint64_t>(pb.src.cur_addr_frac) + static_cast<uint64_t>(count) * ratio) >> 16
        : count;
    constexpr uint32_t kMaxDecoded = 1024;
    if (needed <= kMaxDecoded) {
        // The four history samples, then the new ones: the four taps for an output
        // sample are the four in a row ending at the newest sample read so far.
        std::array<int16_t, kMaxDecoded + 4> line;
        std::memcpy(line.data(), pb.src.last_samples, sizeof(int16_t) * 4);
        accel.ReadSamples(line.data() + 4, static_cast<uint32_t>(needed));
        pb.src.cur_addr_frac = static_cast<uint16_t>(ResampleLine(
            line.data(), out, count, pb.src.last_samples,
            pb.src.cur_addr_frac, ratio, pb.src_type, VoiceCoefficients(coeffs, pb.coef_select),
            coeffsFit32));
        return;
    }
    pb.src.cur_addr_frac = static_cast<uint16_t>(ResampleAudio(
        [&accel](uint32_t) {
            return accel.ReadSample();
        },
        out, count, pb.src.last_samples, pb.src.cur_addr_frac,
        Hilo(pb.src.ratio_hi, pb.src.ratio_lo), pb.src_type,
        VoiceCoefficients(coeffs, pb.coef_select)));
}

inline void MixAdd(int* out, const int16_t* input, uint32_t count, VolumeData& vd, int16_t& dpop, bool ramp) {
    // int16 x uint16 always fits in int32, so the 64-bit clamp the generic
    // helper performs is unnecessary here.
    const uint16_t delta = ramp ? vd.volume_delta : 0;
    if (vd.volume == 0 && delta == 0) {
        // Adds nothing; the kernel would leave only the last (zero) sample as the de-pop.
        if (count != 0)
            dpop = 0;
        return;
    }
    vd.volume = AxMixKernels::MixAddRamp(out, input, count, vd.volume, delta, dpop);
}

inline void LowPassFilter(int16_t* samples, uint32_t count, PBLowPassFilter& filter) {
    for (uint32_t i = 0; i < count; ++i) {
        filter.yn1 = samples[i] = ClampS16((filter.a0 * static_cast<int32_t>(samples[i]) +
                                            filter.b0 * static_cast<int32_t>(filter.yn1)) >> 15);
    }
}

inline void BiquadFilter(int16_t* samples, uint32_t count, PBBiquadFilter& filter) {
    for (uint32_t i = 0; i < count; ++i) {
        const int16_t xn0 = samples[i];
        int64_t value = 0;
        value += filter.b0 * static_cast<int32_t>(xn0);
        value += filter.b1 * static_cast<int32_t>(filter.xn1);
        value += filter.b2 * static_cast<int32_t>(filter.xn2);
        value += filter.a1 * static_cast<int32_t>(filter.yn1);
        value += filter.a2 * static_cast<int32_t>(filter.yn2);
        value <<= 2;
        value += (value & 0x10000) ? 0x8000 : 0x7fff;
        const int16_t yn0 = ClampS16(value >> 16);
        filter.xn2 = filter.xn1;
        filter.yn2 = filter.yn1;
        filter.xn1 = xn0;
        filter.yn1 = yn0;
        samples[i] = yn0;
    }
}

// One voice's frame into the buses; false (and nothing done) when it isn't running.
inline bool ProcessVoice(AXPBWii& pb, const AXBuffers& buffers, uint32_t count, AXMixControl control,
                         const int16_t* coeffs, bool newFilter, bool coeffsFit32) {
    if (pb.running != 1) {
        return false;
    }
    Accelerator accel;
    accel.Setup(&pb);
    std::array<int16_t, kAxSamplesPerFrame> samples{};
    Resample(accel, pb, samples.data(), count, coeffs, coeffsFit32);
    accel.Commit();
    // Same wrapping 16-bit ramp as MixAdd, minus the accumulate. The two's
    // complement wrap of the int16 envelope is bit-identical to the uint16
    // wrap the kernel performs, so the value stored back is unchanged.
    pb.vol_env.cur_volume = static_cast<int16_t>(AxMixKernels::ScaleRamp(
        samples.data(), count, static_cast<uint16_t>(pb.vol_env.cur_volume),
        static_cast<uint16_t>(pb.vol_env.cur_volume_delta)));
    if (pb.lpf.on) {
        LowPassFilter(samples.data(), count, pb.lpf);
    }
    if (newFilter && pb.biquad.on) {
        BiquadFilter(samples.data(), count, pb.biquad);
    }

    auto mix = [&](uint32_t bit, uint32_t rampBit, int* out, VolumeData& vd, int16_t& dpop) {
        if ((control & bit) == 0) {
            return;
        }
        MixAdd(out, samples.data(), count, vd, dpop, (control & rampBit) != 0);
    };
    mix(MIX_MAIN_L, MIX_MAIN_L_RAMP, buffers[kBusMainL], pb.mixer.main_left, pb.dpop.main_left);
    mix(MIX_MAIN_R, MIX_MAIN_R_RAMP, buffers[kBusMainR], pb.mixer.main_right, pb.dpop.main_right);
    mix(MIX_MAIN_S, MIX_MAIN_S_RAMP, buffers[kBusMainS], pb.mixer.main_surround, pb.dpop.main_surround);
    mix(MIX_AUXA_L, MIX_AUXA_L_RAMP, buffers[kBusAuxAL], pb.mixer.auxA_left, pb.dpop.auxA_left);
    mix(MIX_AUXA_R, MIX_AUXA_R_RAMP, buffers[kBusAuxAR], pb.mixer.auxA_right, pb.dpop.auxA_right);
    mix(MIX_AUXA_S, MIX_AUXA_S_RAMP, buffers[kBusAuxAS], pb.mixer.auxA_surround, pb.dpop.auxA_surround);
    mix(MIX_AUXB_L, MIX_AUXB_L_RAMP, buffers[kBusAuxBL], pb.mixer.auxB_left, pb.dpop.auxB_left);
    mix(MIX_AUXB_R, MIX_AUXB_R_RAMP, buffers[kBusAuxBR], pb.mixer.auxB_right, pb.dpop.auxB_right);
    mix(MIX_AUXB_S, MIX_AUXB_S_RAMP, buffers[kBusAuxBS], pb.mixer.auxB_surround, pb.dpop.auxB_surround);
    mix(MIX_AUXC_L, MIX_AUXC_L_RAMP, buffers[kBusAuxCL], pb.mixer.auxC_left, pb.dpop.auxC_left);
    mix(MIX_AUXC_R, MIX_AUXC_R_RAMP, buffers[kBusAuxCR], pb.mixer.auxC_right, pb.dpop.auxC_right);
    mix(MIX_AUXC_S, MIX_AUXC_S_RAMP, buffers[kBusAuxCS], pb.mixer.auxC_surround, pb.dpop.auxC_surround);

    if (pb.remote) {
        if (newFilter && pb.remote_iir.on) {
            if (pb.remote_iir.on == 2) {
                BiquadFilter(samples.data(), count, pb.remote_iir.biquad);
            } else {
                LowPassFilter(samples.data(), count, pb.remote_iir.lpf);
            }
        }

        const uint32_t wmCount = count == kAxSamplesPerFrame ? 18u : 6u;
        std::array<int16_t, 18> wmSamples{};
        pb.remote_src.cur_addr_frac = static_cast<uint16_t>(ResampleAudio(
            [&samples, count](uint32_t index) {
                return samples[std::min<uint32_t>(index, count - 1)];
            },
            wmSamples.data(), wmCount, pb.remote_src.last_samples, pb.remote_src.cur_addr_frac,
            0x00055555u, 0, coeffs));

        auto mixRemote = [&](uint32_t channel, int* out, VolumeData& vd, int16_t& dpop) {
            const uint16_t mode = static_cast<uint16_t>((pb.remote_mixer_control >> (channel * 2)) & 3u);
            if (mode == 0) {
                return;
            }
            MixAdd(out, wmSamples.data(), wmCount, vd, dpop, (mode & 2u) != 0);
        };
        mixRemote(0, buffers[kBusWm0Main], pb.remote_mixer.main0, pb.remote_dpop.main0);
        mixRemote(1, buffers[kBusWm0Aux], pb.remote_mixer.aux0, pb.remote_dpop.aux0);
        mixRemote(2, buffers[kBusWm1Main], pb.remote_mixer.main1, pb.remote_dpop.main1);
        mixRemote(3, buffers[kBusWm1Aux], pb.remote_mixer.aux1, pb.remote_dpop.aux1);
        mixRemote(4, buffers[kBusWm2Main], pb.remote_mixer.main2, pb.remote_dpop.main2);
        mixRemote(5, buffers[kBusWm2Aux], pb.remote_mixer.aux2, pb.remote_dpop.aux2);
        mixRemote(6, buffers[kBusWm3Main], pb.remote_mixer.main3, pb.remote_dpop.main3);
        mixRemote(7, buffers[kBusWm3Aux], pb.remote_mixer.aux3, pb.remote_dpop.aux3);
    }
    return true;
}

} // namespace AxDspHle
