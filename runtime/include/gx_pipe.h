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

// Called once `cursor` reaches `limit`; returns the cursor it leaves.
uint8_t* Overflow();
// A burst too long for the slack, written in pieces.
void WriteLong(const uint8_t* data, uint32_t bytes);

#define MKW_GXPIPE_INLINE inline __attribute__((always_inline))

// The cursor is stored again after the slow path with the value Overflow
// returns, so GCC knows what the next store's cursor is on both paths and
// keeps it in a register; otherwise it reloaded it after the join, straight
// behind the store.
MKW_GXPIPE_INLINE void Advance(uint32_t bytes)
{
    uint8_t* cursor = g_pipe.cursor + bytes;
    g_pipe.cursor = cursor;
    if (__builtin_expect(cursor >= g_pipe.limit, 0))
        cursor = Overflow();
    g_pipe.cursor = cursor;
}

} // namespace GxPipe

// On the PPU each store is an asm of its own that names only the address.
// Written in C, a store through `cursor` may alias g_pipe itself (the build
// has -fno-strict-aliasing), so GCC reloaded cursor and limit after every one:
// a load straight behind the previous write's store of the cursor, which the
// PPE pays for as a load-hit-store. Nothing on the game thread reads the bytes
// back other than through a call (Overflow, WriteLong, the GX functions that
// publish the ring or end a display list), and a volatile asm stays on its
// side of a call.
//
// A float goes in with stfs when the cursor is word-aligned; memcpy took it
// through a GPR and the stack, a second load-hit-store. Only then: the stream
// is byte-granular (an XF load's floats follow a 1-byte opcode and a 4-byte
// header), and a floating-point store to an address that isn't word-aligned
// takes an alignment interrupt on the PPE, which the game never gets out of.
#if defined(__PPU__)
#define MKW_GXPIPE_STORE(insn, kind, val) \
    __asm__ volatile(insn " %0,0(%1)" : : kind(val), "b"(GxPipe::g_pipe.cursor))
#endif

// The names the translator emits. The host is big-endian, so a value's
// in-register byte order is already the stream's.
extern "C" {

MKW_GXPIPE_INLINE void GX_HLE_FIFO_Write8(uint8_t val)
{
#if defined(__PPU__)
    MKW_GXPIPE_STORE("stb", "r", val);
#else
    *GxPipe::g_pipe.cursor = val;
#endif
    GxPipe::Advance(1);
}

MKW_GXPIPE_INLINE void GX_HLE_FIFO_Write16(uint16_t val)
{
#if defined(__PPU__)
    MKW_GXPIPE_STORE("sth", "r", val);
#else
    std::memcpy(GxPipe::g_pipe.cursor, &val, 2);
#endif
    GxPipe::Advance(2);
}

MKW_GXPIPE_INLINE void GX_HLE_FIFO_Write32(uint32_t val)
{
#if defined(__PPU__)
    MKW_GXPIPE_STORE("stw", "r", val);
#else
    std::memcpy(GxPipe::g_pipe.cursor, &val, 4);
#endif
    GxPipe::Advance(4);
}

MKW_GXPIPE_INLINE void GX_HLE_FIFO_WriteFloat(float val)
{
#if defined(__PPU__)
    if (__builtin_expect((reinterpret_cast<uintptr_t>(GxPipe::g_pipe.cursor) & 3u) == 0, 1)) {
        MKW_GXPIPE_STORE("stfs", "f", val);
    } else {
        uint32_t bits;
        std::memcpy(&bits, &val, 4);
        MKW_GXPIPE_STORE("stw", "r", bits);
    }
#else
    std::memcpy(GxPipe::g_pipe.cursor, &val, 4);
#endif
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
