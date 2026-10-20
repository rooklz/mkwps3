#pragma once
// The write-gather pipe, as translated code sees it.
//
// Upstream intercepts the whole GX library and replays it through aurora. On
// the PS3 the game's own GX code runs as translated, so every command it
// issues arrives here as a store to 0xCC008000 -- the real GX command stream,
// byte for byte. The stores land in a host ring that the GPU thread parses
// (gx_hw.cpp); the only exception is display-list recording, where the PI
// points the pipe at a guest buffer and the bytes go there in 32-byte bursts,
// as on hardware.
//
// Each store is one write at `cursor` and a compare against `limit`. Both
// point into memory with at least kPipeSlack bytes past `limit`, so a store
// never checks for room first; the slow path runs once the limit is crossed
// and settles whatever overran it. Only the game thread (every guest fiber
// runs on it) writes the pipe.

#include <cstdint>
#include <cstring>

namespace GxPipe {

inline constexpr uint32_t kPipeSlack = 256;

struct Cursor {
    uint8_t* cursor;
    uint8_t* limit;
};

extern Cursor g_pipe;

// Called once `cursor` reaches `limit`.
void Overflow();
// A burst too long for the slack, written in pieces.
void WriteLong(const uint8_t* data, uint32_t bytes);

#define MKW_GXPIPE_INLINE inline __attribute__((always_inline))

MKW_GXPIPE_INLINE void Advance(uint32_t bytes)
{
    g_pipe.cursor += bytes;
    if (__builtin_expect(g_pipe.cursor >= g_pipe.limit, 0))
        Overflow();
}

} // namespace GxPipe

// The names the translator emits. The host is big-endian, so a value's
// in-register byte order is already the stream's.
extern "C" {

MKW_GXPIPE_INLINE void GX_HLE_FIFO_Write8(uint8_t val)
{
    *GxPipe::g_pipe.cursor = val;
    GxPipe::Advance(1);
}

MKW_GXPIPE_INLINE void GX_HLE_FIFO_Write16(uint16_t val)
{
    std::memcpy(GxPipe::g_pipe.cursor, &val, 2);
    GxPipe::Advance(2);
}

MKW_GXPIPE_INLINE void GX_HLE_FIFO_Write32(uint32_t val)
{
    std::memcpy(GxPipe::g_pipe.cursor, &val, 4);
    GxPipe::Advance(4);
}

MKW_GXPIPE_INLINE void GX_HLE_FIFO_WriteFloat(float val)
{
    std::memcpy(GxPipe::g_pipe.cursor, &val, 4);
    GxPipe::Advance(4);
}

// A run of stores the translator gathered into one buffer (GXFlush's 32 bytes
// of padding is the common case; matrix loads run to ~100).
MKW_GXPIPE_INLINE void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes)
{
    if (__builtin_expect(sizeBytes > GxPipe::kPipeSlack, 0)) {
        GxPipe::WriteLong(data, sizeBytes);
        return;
    }
    std::memcpy(GxPipe::g_pipe.cursor, data, sizeBytes);
    GxPipe::Advance(sizeBytes);
}

} // extern "C"
