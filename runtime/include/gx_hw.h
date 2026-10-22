#pragma once
// The GX front end below the game's own GX library.
//
// Upstream replaces the GX library with HLE calls into aurora. Here the
// translated library runs unmodified and talks to "hardware" exactly as it
// does on a Wii:
//
//   - its command stream arrives through the write-gather pipe (gx_pipe.h)
//     and is parsed on a second PPU thread, the way the GP consumes the FIFO
//     while the CPU builds the next frame;
//   - its CP/PE/PI register traffic lands in a shadow page backing
//     0xCC000000, so reads return what was written or what the GPU thread
//     last reported, and the writes with side effects (FIFO routing, PE
//     interrupt acknowledges) are handled on the way in;
//   - the PE finish/token interrupts the GPU thread raises are delivered to
//     the guest's own handlers at the next scheduler safe point.
//
// Everything a renderer needs is handed to a GxSink on the GPU thread.

#include <chrono>
#include <cstdint>

struct CpuContext;

extern "C" {

// Parsed commands, delivered in stream order on the GPU thread. Pointers into
// the stream are valid only for the duration of the call.
struct GxSink {
    void* ctx;
    void (*load_cp)(void* ctx, uint8_t reg, uint32_t value);
    void (*load_xf)(void* ctx, uint16_t addr, const uint32_t* values, unsigned count);
    // `value` carries the register number in its top byte, as in the stream.
    void (*load_bp)(void* ctx, uint32_t value);
    void (*load_index)(void* ctx, uint8_t op, uint16_t index, uint16_t xfAddr, uint8_t count);
    void (*draw)(void* ctx, uint8_t primitive, unsigned vat, uint16_t vertexCount,
                 const uint8_t* vertices, uint32_t vertexSize);
    // A VI retrace committed `xfb` (guest address, 0 = none) for display.
    void (*present)(void* ctx, uint32_t xfb, uint32_t width, uint32_t height, bool black);
    // The GPU thread is about to wait for more commands.
    void (*idle)(void* ctx);
    // Guest RAM in [addr, addr + size) was rewritten by DMA (any thread).
    void (*ram_written)(void* ctx, uint32_t addr, uint32_t size);
    // First thing on the GPU thread: bring the renderer up there.
    void (*start)(void* ctx);
    // Consecutive triangle-type primitives of one VAT: `prims` commands,
    // headers included, `bytes` long from `commands`; `indices` is the
    // triangle-list index count they make. Optional; without it every
    // primitive arrives through draw.
    void (*draw_run)(void* ctx, unsigned vat, const uint8_t* commands, uint32_t bytes, uint32_t prims,
                     uint32_t vertices, uint32_t indices, uint32_t vertexSize);
};
// The renderer restates this table in C (renderer_glue.c) and asserts the
// same size; change both together.
static_assert(sizeof(GxSink) == 11 * sizeof(void*), "GxSink differs from renderer_glue.c");

} // extern "C"

namespace GxHw {

// After Memory::Init: backs the register shadow, allocates the ring and starts
// the GPU thread.
void Initialize();
void Shutdown();

// Install the renderer. Call before Initialize or with the GPU thread idle.
void SetSink(const GxSink* sink);

// Guest stores to 0xCC000000..0xCDFFFFFF other than the gather pipe.
void MmioWrite(uint32_t addr, uint64_t value, unsigned size);
// Checked reads of the same range (fast-path reads hit the shadow directly).
uint64_t MmioRead(uint32_t addr, unsigned size);

// Guest RAM changed behind the CPU's back (DVD/locked-cache DMA, cache
// invalidates): textures the renderer derived from the range are stale.
void NotifyGuestRamWrite(uint32_t addr, uint32_t size);

// Game thread: make every byte written to the pipe visible to the GPU thread.
void Publish();

// Game thread, at a scheduler safe point with guest interrupts enabled:
// publish the pipe, then run the guest handlers for any interrupts the GPU
// thread raised since the last call.
void ServiceInterrupts(CpuContext* cpu);

// True when an interrupt is waiting for ServiceInterrupts.
bool InterruptPending();

// Game thread, idle: sleeps until `until`, or until the GPU thread raises an
// interrupt (true then). Most idle waits are for a finish or a token, and
// sleeping out a fixed poll interval would hold the frame up that long.
bool IdleWaitUntil(std::chrono::steady_clock::time_point until);

// A VI retrace committed `xfb` for scan-out. The GPU thread shows the latest
// request between commands; the guest only commits an XFB whose copy GXDrawDone
// already reported finished, so it is complete by then.
void RequestPresent(uint32_t xfb, uint32_t width, uint32_t height, bool black);

// Cell timebase ticks (79.8 MHz).
inline uint64_t HostTicks()
{
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

// Time the game thread spent in the scheduler's idle loop, for the
// busy-per-frame figure in the stats report.
void NoteGuestIdle(uint64_t ticks);

struct Stats {
    uint64_t bytesWritten;     // into the ring
    uint64_t bytesParsed;
    uint64_t listBytesParsed;  // inside called display lists
    uint64_t draws;
    uint64_t vertices;
    uint64_t listCalls;
    uint64_t listBytesRecorded;
    uint64_t unknownOpcodes;
    uint64_t finishes;
    uint64_t tokens;
    uint64_t producerStalls;   // pipe full, game thread waited
    uint64_t gpuBusyNanos;
    uint64_t guestIdleNanos;
};
Stats GetStats();

} // namespace GxHw
