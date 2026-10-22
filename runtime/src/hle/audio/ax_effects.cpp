#include "abi_bridge.h"
#include "gx_hw.h"
#include "isa/big_endian.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>


extern "C" void func_8012B830(CpuContext* ctx);

#if defined(__clang__)
// PowerPC uses discrete fmuls/fadds; a fused multiply-add would change sample rounding.
#pragma clang fp contract(off)
#endif

namespace {
namespace ReverbStd {

constexpr uint32_t kSamplesPerFrame = 96;
constexpr uint32_t kChannels = 3;

// .sdata2 constants the guest function loads through r2.
constexpr uint32_t kOneConstantAddr = 0x80388588u;   // 1.0f
constexpr uint32_t kScaleConstantAddr = 0x8038858Cu; // 0.6f send pre-scale

// AXFX_REVERBSTD_EXP field offsets (byte offsets into the struct in r4).
constexpr uint32_t kFieldPreDelayCoef = 0x18;
constexpr uint32_t kFieldEarlyLength = 0x2C;
constexpr uint32_t kFieldComb1Coef = 0x64;
constexpr uint32_t kFieldComb2Coef = 0x68;
constexpr uint32_t kFieldAllpassCoef = 0x9C;
constexpr uint32_t kFieldLastAllpass = 0xA0; // + channel * 4
constexpr uint32_t kFieldDamping = 0xAC;
constexpr uint32_t kFieldFlags = 0xB0;
constexpr uint32_t kFieldDryPreScale = 0xD0;
constexpr uint32_t kFieldWetPreScale = 0xD4;
constexpr uint32_t kFieldAuxInputBuffers = 0xD8;
constexpr uint32_t kFieldAuxOutputBuffers = 0xDC;
constexpr uint32_t kFieldMainOutGain = 0xE0;
constexpr uint32_t kFieldAuxOutGain = 0xE4;
constexpr uint32_t kStateStructBytes = 0xE8;

enum RingId : uint32_t {
    kRingPreDelay = 0,
    kRingEarly,
    kRingComb1,
    kRingComb2,
    kRingAllpass1,
    kRingAllpass2,
    kRingCount,
};

struct RingLayout {
    uint32_t bufferField;   // Channel 0 buffer pointer.
    uint32_t channelStride; // Byte stride between channel buffer pointers.
    uint32_t indexField;
    uint32_t lengthField;
};

constexpr RingLayout kRingLayout[kRingCount] = {
    {0x00, 4, 0x0C, 0x10}, // Pre-delay comb.
    {0x1C, 4, 0x28, 0x2C}, // Early reflection tap (optional).
    {0x34, 8, 0x4C, 0x54}, // Comb 1 (per-channel pointers interleave with comb 2).
    {0x38, 8, 0x50, 0x58}, // Comb 2.
    {0x6C, 8, 0x84, 0x8C}, // Allpass 1 (interleaves with allpass 2).
    {0x70, 8, 0x88, 0x90}, // Allpass 2.
};

struct Frame {
    uint8_t* ring[kRingCount][kChannels]{};
    uint32_t ringIndex[kRingCount]{};
    uint32_t ringLength[kRingCount]{};
    uint8_t* main[kChannels]{};
    const uint8_t* auxIn[kChannels]{};
    uint8_t* auxOut[kChannels]{};
    float lastAllpass[kChannels]{};
    float preDelayCoef = 0.0f;
    float comb1Coef = 0.0f;
    float comb2Coef = 0.0f;
    float allpassCoef = 0.0f;
    float damping = 0.0f;
    float oneMinusDamping = 0.0f;
    float dryScale = 0.0f;
    float wetScale = 0.0f;
    float mainGain = 0.0f;
    float auxGain = 0.0f;
    bool hasEarly = false;
    bool hasAuxIn = false;
    bool hasAuxOut = false;
};

inline float LoadFloat(const uint8_t* host) {
    return BigEndian::ReadFloat32(host);
}

inline void StoreFloat(uint8_t* host, float value) {
    BigEndian::WriteFloat32(host, value);
}

inline int32_t LoadS32(const uint8_t* host) {
    return static_cast<int32_t>(BigEndian::Read32(host));
}

inline void StoreS32(uint8_t* host, int32_t value) {
    BigEndian::Write32(host, static_cast<uint32_t>(value));
}

// Guest-thread-only range resolver. Deliberately NOT the mix's MixResolveRange: this
// callback must materialize deferred GX reads through the page table, which the
// worker-safe resolver refuses to do by design.
uint8_t* ResolveGuestThreadRange(uint32_t addr, size_t bytes) {
    if (addr == 0 || bytes == 0) {
        return nullptr;
    }
    if (uint8_t* fast = MemoryInline::GetPointerFast(addr, bytes)) {
        return fast;
    }
    // A ring buffer may straddle the inline page granularity; the region lookup
    // still returns one contiguous host mapping for the whole range.
    try {
        return Memory::GetPointer(addr, bytes);
    } catch (const Memory::AccessViolation&) {
        return nullptr;
    }
}

// Collects everything the render loop needs. Returns false when the layout is
// not one this port can serve bit-exactly, in which case the caller must run the
// translated function instead.
bool BuildFrame(uint32_t buffersAddr, uint32_t stateAddr, Frame& frame) {
    if (buffersAddr == 0 || stateAddr == 0) {
        return false;
    }
    if (!Memory::Contains(stateAddr, kStateStructBytes)) {
        return false;
    }

    const uint32_t auxInputBuffers = Memory::Read32(stateAddr + kFieldAuxInputBuffers);
    const uint32_t auxOutputBuffers = Memory::Read32(stateAddr + kFieldAuxOutputBuffers);
    frame.hasAuxIn = auxInputBuffers != 0;
    frame.hasAuxOut = auxOutputBuffers != 0;

    constexpr size_t kFrameBytes = kSamplesPerFrame * sizeof(int32_t);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.main[channel] =
            ResolveGuestThreadRange(Memory::Read32(buffersAddr + channel * 4), kFrameBytes);
        if (!frame.main[channel]) {
            return false;
        }
        if (frame.hasAuxIn) {
            frame.auxIn[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxInputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxIn[channel]) {
                return false;
            }
        }
        if (frame.hasAuxOut) {
            frame.auxOut[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxOutputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxOut[channel]) {
                return false;
            }
        }
    }

    frame.hasEarly = Memory::Read32(stateAddr + kFieldEarlyLength) != 0;
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        const RingLayout& layout = kRingLayout[ring];
        frame.ringLength[ring] = Memory::Read32(stateAddr + layout.lengthField);
        frame.ringIndex[ring] = Memory::Read32(stateAddr + layout.indexField);
        if (ring == kRingEarly && !frame.hasEarly) {
            continue;
        }
        // The guest maintains index < length; anything else means the struct is
        // not initialized the way this port assumes.
        if (frame.ringLength[ring] == 0 || frame.ringIndex[ring] >= frame.ringLength[ring]) {
            return false;
        }
        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            frame.ring[ring][channel] = ResolveGuestThreadRange(
                Memory::Read32(stateAddr + layout.bufferField + channel * layout.channelStride),
                static_cast<size_t>(frame.ringLength[ring]) * sizeof(float));
            if (!frame.ring[ring][channel]) {
                return false;
            }
        }
    }

    const float sendScale = Memory::ReadFloat32(kScaleConstantAddr);
    const float one = Memory::ReadFloat32(kOneConstantAddr);
    frame.damping = Memory::ReadFloat32(stateAddr + kFieldDamping);
    frame.oneMinusDamping = one - frame.damping;
    frame.dryScale = sendScale * Memory::ReadFloat32(stateAddr + kFieldDryPreScale);
    frame.wetScale = sendScale * Memory::ReadFloat32(stateAddr + kFieldWetPreScale);
    frame.preDelayCoef = Memory::ReadFloat32(stateAddr + kFieldPreDelayCoef);
    frame.comb1Coef = Memory::ReadFloat32(stateAddr + kFieldComb1Coef);
    frame.comb2Coef = Memory::ReadFloat32(stateAddr + kFieldComb2Coef);
    frame.allpassCoef = Memory::ReadFloat32(stateAddr + kFieldAllpassCoef);
    frame.mainGain = Memory::ReadFloat32(stateAddr + kFieldMainOutGain);
    frame.auxGain = Memory::ReadFloat32(stateAddr + kFieldAuxOutGain);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.lastAllpass[channel] = Memory::ReadFloat32(stateAddr + kFieldLastAllpass + channel * 4);
    }
    return true;
}

// A frame of int32 samples to floats. The PPE's integer and float units only
// meet through memory, and a value stored and loaded straight back stalls for
// tens of cycles, so every store goes out before the first load comes back.
inline void SamplesToFloat(const int64_t* raw, float* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
#if defined(__PPU__)
        double bits;
        std::memcpy(&bits, &raw[i], sizeof(bits));
        double value;
        __asm__("fcfid %0,%1" : "=f"(value) : "f"(bits));
        out[i] = static_cast<float>(value);
#else
        out[i] = static_cast<float>(static_cast<int32_t>(raw[i]));
#endif
    }
}

// The fctiwz results' low words, read once they have all been stored.
inline void StoreWords(uint8_t* dst, const double* converted, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t word;
        std::memcpy(&word, reinterpret_cast<const uint8_t*>(&converted[i]) + 4, sizeof(word));
        StoreS32(dst + i * 4u, static_cast<int32_t>(word));
    }
}

// The guest's fctiwz: round toward zero, saturating out-of-range values and
// NaN, with the word in the double's low half.
inline double ConvertToIntegerWordBits(float value) {
    return PPC_Fctiwz(static_cast<double>(value));
}

// Channel by channel: the channels share nothing but the ring indices, so the
// samples go through in the guest's order per channel, and each channel's
// conversions batch up around its filter loop.
void Render(uint32_t stateAddr, Frame& frame) {
    uint32_t index[kRingCount];
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        int64_t raw[kSamplesPerFrame];
        float input[kSamplesPerFrame];
        double mainBits[kSamplesPerFrame];
        double auxBits[kSamplesPerFrame];

        uint8_t* const mainBuffer = frame.main[channel];
        for (uint32_t sample = 0; sample < kSamplesPerFrame; ++sample) {
            int32_t rawInput = LoadS32(mainBuffer + sample * 4u);
            if (frame.hasAuxIn) {
                rawInput = static_cast<int32_t>(
                    static_cast<uint32_t>(rawInput) +
                    static_cast<uint32_t>(LoadS32(frame.auxIn[channel] + sample * 4u)));
            }
            raw[sample] = rawInput;
        }
        SamplesToFloat(raw, input, kSamplesPerFrame);

        for (uint32_t ring = 0; ring < kRingCount; ++ring) {
            index[ring] = frame.ringIndex[ring];
        }
        uint8_t* const preDelayRing = frame.ring[kRingPreDelay][channel];
        uint8_t* const earlyRing = frame.ring[kRingEarly][channel];
        uint8_t* const comb1Ring = frame.ring[kRingComb1][channel];
        uint8_t* const comb2Ring = frame.ring[kRingComb2][channel];
        uint8_t* const allpass1Ring = frame.ring[kRingAllpass1][channel];
        uint8_t* const allpass2Ring = frame.ring[kRingAllpass2][channel];
        float lastAllpass = frame.lastAllpass[channel];

        for (uint32_t sample = 0; sample < kSamplesPerFrame; ++sample) {
            const float in = input[sample];

            // Pre-delay comb: the tap that leaves the buffer also feeds the dry
            // (early) send. Every product is its own statement so the host
            // compiler cannot fuse a multiply into the following add.
            uint8_t* const preDelaySlot = preDelayRing + index[kRingPreDelay] * 4u;
            const float preDelayTap = LoadFloat(preDelaySlot);
            const float preDelayFeedback = preDelayTap * frame.preDelayCoef;
            StoreFloat(preDelaySlot, in + preDelayFeedback);

            float excite = in;
            if (frame.hasEarly) {
                uint8_t* const earlySlot = earlyRing + index[kRingEarly] * 4u;
                excite = LoadFloat(earlySlot);
                StoreFloat(earlySlot, in);
            }

            const float dry = preDelayTap * frame.dryScale;

            uint8_t* const comb1Slot = comb1Ring + index[kRingComb1] * 4u;
            const float comb1Tap = LoadFloat(comb1Slot);
            const float comb1Feedback = comb1Tap * frame.comb1Coef;
            StoreFloat(comb1Slot, excite + comb1Feedback);

            uint8_t* const comb2Slot = comb2Ring + index[kRingComb2] * 4u;
            const float comb2Tap = LoadFloat(comb2Slot);
            const float comb2Feedback = comb2Tap * frame.comb2Coef;
            const float combSum = comb1Tap + comb2Tap;
            StoreFloat(comb2Slot, excite + comb2Feedback);

            uint8_t* const allpass1Slot = allpass1Ring + index[kRingAllpass1] * 4u;
            const float allpass1Tap = LoadFloat(allpass1Slot);
            const float allpass1Feedback = allpass1Tap * frame.allpassCoef;
            const float allpass1Store = combSum + allpass1Feedback;
            StoreFloat(allpass1Slot, allpass1Store);
            const float allpass1Feedforward = allpass1Store * frame.allpassCoef;
            const float allpass1Out = allpass1Tap - allpass1Feedforward;

            const float dampedNew = frame.oneMinusDamping * allpass1Out;
            const float dampedOld = frame.damping * lastAllpass;
            const float damped = dampedNew + dampedOld;
            lastAllpass = damped;

            uint8_t* const allpass2Slot = allpass2Ring + index[kRingAllpass2] * 4u;
            const float allpass2Tap = LoadFloat(allpass2Slot);
            const float allpass2Feedback = allpass2Tap * frame.allpassCoef;
            const float allpass2Store = damped + allpass2Feedback;
            StoreFloat(allpass2Slot, allpass2Store);
            const float allpass2Feedforward = allpass2Store * frame.allpassCoef;
            const float allpass2Out = allpass2Tap - allpass2Feedforward;

            const float wet = allpass2Out * frame.wetScale;
            const float mixed = dry + wet;
            const float mainSample = mixed * frame.mainGain;
            mainBits[sample] = ConvertToIntegerWordBits(mainSample);
            if (frame.hasAuxOut) {
                const float auxSample = mixed * frame.auxGain;
                auxBits[sample] = ConvertToIntegerWordBits(auxSample);
            }

            for (uint32_t ring = 0; ring < kRingCount; ++ring) {
                if (ring == kRingEarly && !frame.hasEarly) {
                    continue;
                }
                const uint32_t next = index[ring] + 1u;
                index[ring] = next < frame.ringLength[ring] ? next : 0u;
            }
        }
        frame.lastAllpass[channel] = lastAllpass;

        StoreWords(mainBuffer, mainBits, kSamplesPerFrame);
        if (frame.hasAuxOut) {
            StoreWords(frame.auxOut[channel], auxBits, kSamplesPerFrame);
        }
    }

    // The guest writes these back every sample; nothing can observe the
    // intermediate values, so one store per field at the end is equivalent.
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        if (ring == kRingEarly && !frame.hasEarly) {
            continue;
        }
        Memory::Write32(stateAddr + kRingLayout[ring].indexField, index[ring]);
    }
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        Memory::WriteFloat32(stateAddr + kFieldLastAllpass + channel * 4,
                             static_cast<double>(frame.lastAllpass[channel]));
    }
}

} // namespace ReverbStd
} // namespace

// Time base ticks spent in the reverb, the frames it rendered natively and the
// ones it left to the translated guest code, for the stats line.
extern "C" {
uint64_t g_reverbTicks = 0;
uint32_t g_reverbCalls = 0;
uint32_t g_reverbFallbacks = 0;
}

namespace {
struct ReverbTimer {
    const uint64_t start = GxHw::HostTicks();
    ~ReverbTimer() { g_reverbTicks += GxHw::HostTicks() - start; }
};
} // namespace

extern "C" void AXFXReverbStdExpCallback_8012b830(CpuContext* ctx) {
    if (!ctx) {
        return;
    }
    ReverbTimer timer;

    const uint32_t buffersAddr = ctx->gpr[3];
    const uint32_t stateAddr = ctx->gpr[4];

    uint32_t flags = 0;
    try {
        flags = Memory::Read32(stateAddr + ReverbStd::kFieldFlags);
    } catch (const Memory::AccessViolation&) {
        ++g_reverbFallbacks;
        func_8012B830(ctx);
        return;
    }
    if (flags != 0) {
        // Reset request: the guest clears the "in progress" bit and skips the
        // frame entirely.
        Memory::Write32(stateAddr + ReverbStd::kFieldFlags, flags & ~2u);
        return;
    }

    ReverbStd::Frame frame;
    bool built = false;
    try {
        built = ReverbStd::BuildFrame(buffersAddr, stateAddr, frame);
    } catch (const Memory::AccessViolation&) {
        built = false;
    }
    if (!built) {
        ++g_reverbFallbacks;
        func_8012B830(ctx);
        return;
    }

    ++g_reverbCalls;
    ReverbStd::Render(stateAddr, frame);
}

REGISTER_NATIVE_FUNCTION_AS(0x8012B830, AXFXReverbStdExpCallback_8012b830,
                            "AXFXReverbStdExpCallback_8012b830");
