#pragma once

// Audio output. Upstream streams to SDL; on the PS3 this is a libaudio port
// (platform/ps3/audio_backend_ps3.cpp): 48 kHz float stereo, fed from a ring
// the AX/AI HLE fills at the game's rate and a PPU thread drains, resampling.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class AudioBackend {
public:
    static AudioBackend& Instance();

    bool Init(uint32_t sampleRate, uint32_t channels);
    void Shutdown();

    bool PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes);
    bool PushSamplesLE16(const int16_t* samples, size_t sampleCount);

    void SetMasterVolume(float volume);
    void SetMuted(bool muted);

private:
    AudioBackend() = default;
    ~AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;
};
