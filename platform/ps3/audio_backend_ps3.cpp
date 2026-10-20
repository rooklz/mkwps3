// AudioBackend on libaudio (see audio_backend.h).
//
// The HLE pushes interleaved 16-bit stereo at the game's rate (32 kHz for
// MKWii). A ring holds it as float; an audio PPU thread keeps the port's
// blocks filled one short of full -- writing the block being played is the
// classic tearing buzz -- resampling to the port's fixed 48 kHz.

#include "audio_backend.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ppu-types.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/systime.h>

namespace {

constexpr uint32_t kPortRate = 48000u;
constexpr uint32_t kRingFrames = 1u << 14;   // ~0.5 s at 32 kHz

struct State {
    std::mutex lock;
    float ring[kRingFrames * 2];
    uint32_t head = 0;      // frames written
    uint32_t tail = 0;      // frames consumed
    uint32_t sourceRate = 32000u;
    double phase = 0.0;     // fractional source position between tail and tail+1
    float gain = 1.0f;
    bool muted = false;
    bool ready = false;
    u32 port = 0;
    audioPortConfig config{};
    u64 writeBlock = 0;
    sys_ppu_thread_t thread = 0;
    std::atomic<bool> run{false};
};

State& S()
{
    static State s;
    return s;
}

uint32_t Queued(const State& s) { return s.head - s.tail; }

void PushFrame(State& s, float l, float r)
{
    if (Queued(s) >= kRingFrames - 1)
        return;   // full: drop the newest rather than tear what is queued
    const uint32_t i = (s.head & (kRingFrames - 1)) * 2;
    s.ring[i] = l;
    s.ring[i + 1] = r;
    ++s.head;
}

// One port block: AUDIO_BLOCK_SAMPLES stereo frames at 48 kHz.
void RenderBlock(State& s, float* dst)
{
    std::lock_guard<std::mutex> g(s.lock);
    const double step = static_cast<double>(s.sourceRate) / kPortRate;
    const float gain = s.muted ? 0.0f : s.gain;
    for (int f = 0; f < AUDIO_BLOCK_SAMPLES; ++f) {
        if (Queued(s) < 2) {
            dst[f * 2] = dst[f * 2 + 1] = 0.0f;
            continue;
        }
        const uint32_t a = (s.tail & (kRingFrames - 1)) * 2;
        const uint32_t b = ((s.tail + 1) & (kRingFrames - 1)) * 2;
        const float t = static_cast<float>(s.phase);
        dst[f * 2] = (s.ring[a] + (s.ring[b] - s.ring[a]) * t) * gain;
        dst[f * 2 + 1] = (s.ring[a + 1] + (s.ring[b + 1] - s.ring[a + 1]) * t) * gain;
        s.phase += step;
        while (s.phase >= 1.0 && Queued(s) >= 2) {
            s.phase -= 1.0;
            ++s.tail;
        }
    }
}

void AudioThread(void*)
{
    State& s = S();
    const u64 nblocks = s.config.numBlocks;
    while (s.run.load()) {
        u64 readBlock = *reinterpret_cast<volatile u64*>(static_cast<uintptr_t>(s.config.readIndex));
        if (readBlock >= nblocks)
            readBlock = 0;
        for (;;) {
            const u64 ahead = (s.writeBlock + nblocks - readBlock) % nblocks;
            if (ahead >= nblocks - 1)
                break;
            float* dst = reinterpret_cast<float*>(static_cast<uintptr_t>(s.config.audioDataStart) +
                                                  s.writeBlock * s.config.channelCount *
                                                      AUDIO_BLOCK_SAMPLES * sizeof(float));
            RenderBlock(s, dst);
            s.writeBlock = (s.writeBlock + 1) % nblocks;
        }
        sysUsleep(2000);
    }
    sysThreadExit(0);
}

bool MutedByFlag()
{
    FILE* f = std::fopen("/dev_hdd0/tmp/mkwii-mute.txt", "r");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

} // namespace

AudioBackend& AudioBackend::Instance()
{
    static AudioBackend backend;
    return backend;
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels)
{
    State& s = S();
    {
        std::lock_guard<std::mutex> g(s.lock);
        if (sampleRate)
            s.sourceRate = sampleRate;
        if (s.ready)
            return true;
    }
    (void)channels;
    if (audioInit() != 0) {
        std::printf("[audio] audioInit failed; running silent\n");
        return false;
    }
    audioPortParam params;
    std::memset(&params, 0, sizeof params);
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.level = 1.0f;
    if (audioPortOpen(&params, &s.port) != 0 || audioGetPortConfig(s.port, &s.config) != 0 ||
        audioPortStart(s.port) != 0) {
        std::printf("[audio] port open failed; running silent\n");
        audioQuit();
        return false;
    }
    s.muted = MutedByFlag();
    s.ready = true;
    s.run.store(true);
    char name[] = "mkw_audio";
    sysThreadCreate(&s.thread, AudioThread, nullptr, 900, 64 * 1024, THREAD_JOINABLE, name);
    std::printf("[audio] port %u open at 48 kHz (source %u Hz)%s\n", static_cast<unsigned>(s.port),
                s.sourceRate, s.muted ? ", muted" : "");
    return true;
}

void AudioBackend::Shutdown()
{
    State& s = S();
    if (!s.ready)
        return;
    s.run.store(false);
    u64 rv = 0;
    sysThreadJoin(s.thread, &rv);
    audioPortStop(s.port);
    audioPortClose(s.port);
    audioQuit();
    s.ready = false;
}

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes)
{
    State& s = S();
    if (!s.ready)
        return false;
    std::lock_guard<std::mutex> g(s.lock);
    // Big-endian interleaved stereo, which is also host order here.
    const int16_t* p = reinterpret_cast<const int16_t*>(data);
    const size_t frames = bytes / 4;
    for (size_t i = 0; i < frames; ++i)
        PushFrame(s, p[i * 2] * (1.0f / 32768.0f), p[i * 2 + 1] * (1.0f / 32768.0f));
    return true;
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount)
{
    // Despite the name, upstream passes host-order samples.
    State& s = S();
    if (!s.ready)
        return false;
    std::lock_guard<std::mutex> g(s.lock);
    const size_t frames = sampleCount / 2;
    for (size_t i = 0; i < frames; ++i)
        PushFrame(s, samples[i * 2] * (1.0f / 32768.0f), samples[i * 2 + 1] * (1.0f / 32768.0f));
    return true;
}

void AudioBackend::SetMasterVolume(float volume)
{
    State& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    s.gain = volume;
}

void AudioBackend::SetMuted(bool muted)
{
    State& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    s.muted = muted || MutedByFlag();
}
