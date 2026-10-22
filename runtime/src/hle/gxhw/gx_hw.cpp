// The GX front end under the game's own GX library. See gx_hw.h.

#include "gx_hw.h"
#include "gx_pipe.h"

#include "guest_flat_memory.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "../os/os_internal.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);
extern "C" int mkw_lv2_thread_create(uint64_t* id, void (*entry)(void*), void* arg, int priority,
                                     uint64_t stack, const char* name);
extern "C" int mkw_lv2_thread_join(uint64_t id);
extern "C" void mkw_lv2_thread_exit(void);
extern "C" void mkw_rescue_register_thread(int gpu);
extern "C" void mkw_hprof_attach(int which);

namespace GxPipe {
Cursor g_pipe;
}

namespace GxHw {
namespace {

// ---------------------------------------------------------------------------
// Register map (offsets from 0xCC000000)
// ---------------------------------------------------------------------------

constexpr uint32_t kCpStatus = 0x0000;
constexpr uint32_t kCpControl = 0x0002;
constexpr uint32_t kCpClear = 0x0004;
constexpr uint16_t kCpGpLink = 0x0010;

constexpr uint32_t kPeInterrupt = 0x100A;   // 0 token enable, 1 finish enable, 2/3 status (W1C)
constexpr uint32_t kPeToken = 0x100E;

constexpr uint32_t kPiFifoBase = 0x300C;
constexpr uint32_t kPiFifoEnd = 0x3010;
constexpr uint32_t kPiFifoWptr = 0x3014;
constexpr uint32_t kPiWptrMask = 0x1FFFFFE0u;
constexpr uint32_t kPiWptrWrap = 0x20000000u;

constexpr uint32_t kOsInterruptCp = 17;
constexpr uint32_t kOsInterruptPeToken = 18;
constexpr uint32_t kOsInterruptPeFinish = 19;

uint8_t* g_shadow = nullptr;      // host view of 0xCC000000

inline uint16_t Shadow16(uint32_t off)
{
    uint16_t v;
    std::memcpy(&v, g_shadow + off, 2);
    return v;
}

inline uint32_t Shadow32(uint32_t off)
{
    uint32_t v;
    std::memcpy(&v, g_shadow + off, 4);
    return v;
}

inline void SetShadow16(uint32_t off, uint16_t v) { std::memcpy(g_shadow + off, &v, 2); }
inline void SetShadow32(uint32_t off, uint32_t v) { std::memcpy(g_shadow + off, &v, 4); }

// ---------------------------------------------------------------------------
// The ring
//
// Single producer (the game thread), single consumer (the GPU thread). The
// consumer linearises a command split by the wrap by copying its head into the
// pre-slack, just below the ring, so the parser only ever sees one contiguous
// span. Positions are absolute byte counts.
// ---------------------------------------------------------------------------

constexpr uint32_t kRingBytes = 2u << 20;
constexpr uint32_t kPreSlack = 512u << 10;   // longest command split by the wrap
constexpr uint32_t kChunk = 4096;            // producer publishes at least this often

// Arena placement: guest 0xB0000000 (fold 0x30000000) is unused by the Wii
// address map. The pre-slack sits first, then the ring, then the store slack.
constexpr uint32_t kRingFold = 0x30000000u;
constexpr uint32_t kRingFoldBytes = 3u << 20;   // pre-slack + ring + store slack, in 1 MiB pages

uint8_t* g_ringMem = nullptr;
uint8_t* g_ring = nullptr;

std::atomic<uint64_t> g_written{0};    // published by the producer
std::atomic<uint64_t> g_consumed{0};   // released by the consumer
std::atomic<bool> g_consumerWaiting{false};
std::mutex g_wakeLock;
std::condition_variable g_wake;

// Producer-side state (game thread only).
uint64_t g_lapBase = 0;          // absolute position of g_ring[0] in this lap
uint8_t* g_ringCursor = nullptr; // saved while the pipe records a display list
uint8_t* g_ringLimit = nullptr;
bool g_recording = false;        // pipe feeds a guest buffer, not the ring

alignas(128) uint8_t g_stage[32 + GxPipe::kPipeSlack + 32];

std::atomic<uint64_t> g_bytesWritten{0};
std::atomic<uint64_t> g_listBytesRecorded{0};
std::atomic<uint64_t> g_producerStalls{0};

uint64_t ProducerPosition()
{
    return g_lapBase + static_cast<uint64_t>(GxPipe::g_pipe.cursor - g_ring);
}

void WakeConsumer()
{
    if (g_consumerWaiting.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_wakeLock);
        g_wake.notify_one();
    }
}

void PublishRing(uint64_t position)
{
    if (position == g_written.load(std::memory_order_relaxed))
        return;
    g_bytesWritten.store(position, std::memory_order_relaxed);
    g_written.store(position, std::memory_order_release);
    WakeConsumer();
}

// Settles a ring cursor that reached its limit: folds an overrun past the end
// back to the start, publishes, and finds the next limit -- waiting for the
// consumer if the ring is full.
void RingOverflow()
{
    uint8_t*& cursor = GxPipe::g_pipe.cursor;
    uint8_t* const end = g_ring + kRingBytes;
    if (cursor >= end) {
        const size_t over = static_cast<size_t>(cursor - end);
        std::memmove(g_ring, end, over);
        g_lapBase += kRingBytes;
        cursor = g_ring + over;
    }
    const uint64_t pos = ProducerPosition();
    PublishRing(pos);

    // Room left before the producer would reach unconsumed bytes, keeping the
    // slack a store may overrun by.
    uint64_t free = 0;
    for (unsigned spins = 0;; ++spins) {
        const uint64_t consumed = g_consumed.load(std::memory_order_acquire);
        const uint64_t used = pos - consumed;
        free = kRingBytes - used;
        if (free > GxPipe::kPipeSlack + 256)
            break;
        if (spins == 0)
            g_producerStalls.fetch_add(1, std::memory_order_relaxed);
        WakeConsumer();
        std::this_thread::yield();
    }
    uint64_t room = free - GxPipe::kPipeSlack;
    if (room > kChunk)
        room = kChunk;
    const uint64_t toEnd = static_cast<uint64_t>(end - cursor);
    if (room > toEnd)
        room = toEnd;
    GxPipe::g_pipe.limit = cursor + (room ? room : 1);
}

// --- display-list recording -------------------------------------------------

uint8_t* GuestBytes(uint32_t addr)
{
    return MkwGuestArenaBase() + (addr & GuestFlat::kFoldMask);
}

// One 32-byte burst from the stage to the PI write pointer, advancing and
// wrapping it as the PI does.
void BurstToGuest(const uint8_t* bytes)
{
    const uint32_t base = Shadow32(kPiFifoBase) & kPiWptrMask;
    const uint32_t end = Shadow32(kPiFifoEnd) & kPiWptrMask;
    const uint32_t raw = Shadow32(kPiFifoWptr);
    uint32_t wptr = raw & kPiWptrMask;
    uint32_t wrap = raw & kPiWptrWrap;
    if (end <= base)
        return;   // no window yet: the bytes go nowhere
    if (GuestFlat::RangeBacked(wptr | 0x80000000u, 32)) {
        std::memcpy(GuestBytes(wptr), bytes, 32);
        g_listBytesRecorded.fetch_add(32, std::memory_order_relaxed);
    }
    // The PI wraps (and flags it) on reaching the end register, as Dolphin
    // models it; GXEndDisplayList reports a wrapped list as overflowed.
    wptr += 32;
    if (wptr >= end) {
        wptr = base;
        wrap = kPiWptrWrap;
    }
    SetShadow32(kPiFifoWptr, wptr | wrap);
}

void StageOverflow()
{
    uint8_t*& cursor = GxPipe::g_pipe.cursor;
    while (cursor - g_stage >= 32) {
        BurstToGuest(g_stage);
        const size_t rest = static_cast<size_t>(cursor - (g_stage + 32));
        std::memmove(g_stage, g_stage + 32, rest);
        cursor = g_stage + rest;
    }
}

void EnterRecording()
{
    if (g_recording)
        return;
    g_ringCursor = GxPipe::g_pipe.cursor;
    g_ringLimit = GxPipe::g_pipe.limit;
    GxPipe::g_pipe.cursor = g_stage;
    GxPipe::g_pipe.limit = g_stage + 32;
    g_recording = true;
}

void LeaveRecording()
{
    if (!g_recording)
        return;
    // A store still in the pipe goes wherever the pipe points next, as on
    // hardware; the guest flushes before switching, so this is normally empty.
    const size_t pending = static_cast<size_t>(GxPipe::g_pipe.cursor - g_stage);
    GxPipe::g_pipe.cursor = g_ringCursor;
    GxPipe::g_pipe.limit = g_ringLimit;
    g_recording = false;
    if (pending)
        GxPipe::WriteLong(g_stage, static_cast<uint32_t>(pending));
}

void UpdateRouting()
{
    // GXSetCPUFifo links the CPU and GP FIFOs for the main ring and unlinks
    // them to record a display list into the list's own buffer.
    if (Shadow16(kCpControl) & kCpGpLink)
        LeaveRecording();
    else
        EnterRecording();
}

// ---------------------------------------------------------------------------
// Interrupts raised by the GPU thread
// ---------------------------------------------------------------------------

std::atomic<uint32_t> g_finishRaised{0};
uint32_t g_finishDelivered = 0;
std::atomic<uint32_t> g_tokenRaised{0};
uint32_t g_tokenDelivered = 0;
std::atomic<uint64_t> g_finishes{0};
std::atomic<uint64_t> g_tokens{0};

// ---------------------------------------------------------------------------
// GPU thread: parsing
// ---------------------------------------------------------------------------

const GxSink* g_sink = nullptr;

struct CpState {
    uint32_t vcdLo = 0, vcdHi = 0;
    uint32_t vatA[8] = {}, vatB[8] = {}, vatC[8] = {};
    uint32_t vertexSize[8] = {};
    uint8_t sizeValid = 0;   // bit per VAT
};
CpState g_cp;

std::atomic<uint64_t> g_bytesParsed{0};
uint64_t g_listBytesParsed = 0;
uint64_t g_draws = 0;
uint64_t g_vertices = 0;
uint64_t g_listCalls = 0;
uint64_t g_unknownOpcodes = 0;
uint64_t g_gpuBusyTicks = 0;

inline uint16_t Rd16(const uint8_t* p)
{
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

inline uint32_t Rd32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint32_t Bits(uint32_t v, unsigned lsb, unsigned width)
{
    return (v >> lsb) & ((1u << width) - 1u);
}

uint32_t ComponentSize(uint32_t format)
{
    switch (format) {
    case 0: case 1: return 1;   // u8, s8
    case 2: case 3: return 2;   // u16, s16
    default: return 4;          // f32; 5-7 are undocumented and sized as f32
    }
}

uint32_t ColorSize(uint32_t format)
{
    static const uint8_t kSizes[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return kSizes[format & 7];
}

uint32_t AttrSize(uint32_t type, uint32_t directSize)
{
    switch (type) {
    case 1: return directSize;
    case 2: return 1;
    case 3: return 2;
    default: return 0;
    }
}

// Bytes one vertex of `vat` occupies in the stream. Getting this wrong by one
// byte desynchronises the parser for good, so it follows the CP register
// layout field by field.
uint32_t ComputeVertexSize(unsigned vat)
{
    const uint32_t lo = g_cp.vcdLo, hi = g_cp.vcdHi;
    const uint32_t a = g_cp.vatA[vat], b = g_cp.vatB[vat], c = g_cp.vatC[vat];
    uint32_t size = 0;

    for (unsigned n = 0; n < 9; ++n)   // position + eight texture matrix indices
        size += Bits(lo, n, 1);

    size += AttrSize(Bits(lo, 9, 2), (Bits(a, 0, 1) ? 3u : 2u) * ComponentSize(Bits(a, 1, 3)));

    {
        const uint32_t type = Bits(lo, 11, 2);
        const uint32_t vectors = Bits(a, 9, 1) ? 3u : 1u;
        uint32_t nrm = AttrSize(type, 3u * vectors * ComponentSize(Bits(a, 10, 3)));
        if (Bits(a, 31, 1) && vectors == 3u && type >= 2)
            nrm *= 3u;   // NBT with one index per vector
        size += nrm;
    }

    size += AttrSize(Bits(lo, 13, 2), ColorSize(Bits(a, 14, 3)));
    size += AttrSize(Bits(lo, 15, 2), ColorSize(Bits(a, 18, 3)));

    static const uint8_t kElemLsb[8] = {21, 0, 9, 18, 27, 5, 14, 23};
    static const uint8_t kFmtLsb[8] = {22, 1, 10, 19, 28, 6, 15, 24};
    const uint32_t group[8] = {a, b, b, b, b, c, c, c};
    for (unsigned n = 0; n < 8; ++n) {
        const uint32_t elems = Bits(group[n], kElemLsb[n], 1) ? 2u : 1u;
        size += AttrSize(Bits(hi, 2 * n, 2), elems * ComponentSize(Bits(group[n], kFmtLsb[n], 3)));
    }
    return size;
}

inline uint32_t VertexSize(unsigned vat)
{
    if (!(g_cp.sizeValid & (1u << vat))) {
        g_cp.vertexSize[vat] = ComputeVertexSize(vat);
        g_cp.sizeValid |= static_cast<uint8_t>(1u << vat);
    }
    return g_cp.vertexSize[vat];
}

void LoadCp(uint8_t reg, uint32_t value)
{
    const unsigned n = reg & 0x0F;
    switch (reg & 0xF0) {
    case 0x50: g_cp.vcdLo = value; g_cp.sizeValid = 0; break;
    case 0x60: g_cp.vcdHi = value; g_cp.sizeValid = 0; break;
    case 0x70: if (n < 8) { g_cp.vatA[n] = value; g_cp.sizeValid &= ~(1u << n); } break;
    case 0x80: if (n < 8) { g_cp.vatB[n] = value; g_cp.sizeValid &= ~(1u << n); } break;
    case 0x90: if (n < 8) { g_cp.vatC[n] = value; g_cp.sizeValid &= ~(1u << n); } break;
    default: break;
    }
    if (g_sink && g_sink->load_cp)
        g_sink->load_cp(g_sink->ctx, reg, value);
}

// The game thread's idle wait (IdleWaitUntil), and what ends it early: an
// interrupt raised here.
std::mutex g_idleLock;
std::condition_variable g_idleWake;

void WakeIdleGuest()
{
    { std::lock_guard<std::mutex> lock(g_idleLock); }
    g_idleWake.notify_one();
}

void LoadBp(uint32_t word)
{
    switch (word >> 24) {
    case 0x45:   // PE_DONE: the finish interrupt GXSetDrawDone waits for
        g_finishes.fetch_add(1, std::memory_order_relaxed);
        g_finishRaised.fetch_add(1, std::memory_order_release);
        WakeIdleGuest();
        break;
    case 0x47:   // PE_TOKEN
    case 0x48: { // PE_TOKEN_INT
        const uint16_t token = static_cast<uint16_t>(word);
        __atomic_store_n(reinterpret_cast<uint16_t*>(g_shadow + kPeToken), token, __ATOMIC_RELEASE);
        g_tokens.fetch_add(1, std::memory_order_relaxed);
        if ((word >> 24) == 0x48) {
            g_tokenRaised.fetch_add(1, std::memory_order_release);
            WakeIdleGuest();
        }
        break;
    }
    default:
        break;
    }
    if (g_sink && g_sink->load_bp)
        g_sink->load_bp(g_sink->ctx, word);
}

size_t Parse(const uint8_t* p, size_t n, bool inList);

void CallList(uint32_t addr, uint32_t size)
{
    ++g_listCalls;
    if (!GuestFlat::RangeBacked(addr | 0x80000000u, size)) {
        static unsigned reports;
        if (reports++ < 8)
            std::printf("[gx] display list %08x+%x is outside guest RAM; skipped\n", addr, size);
        return;
    }
    const size_t used = Parse(GuestBytes(addr), size, true);
    g_listBytesParsed += used;
}

// Runs: the strips of one shape follow each other with nothing in between,
// and handing them over together lets the renderer treat them as one draw.
// Limits match what one SPU job carries (spu_vtx_shared.h).
bool g_runsEnabled = true;
constexpr uint32_t kRunMaxVerts = 4096;
constexpr uint32_t kRunMaxIndices = 8192;
constexpr size_t kRunMaxBytes = 60u * 1024u;
constexpr uint32_t kRunMaxPrims = 4096;

inline bool RunJoinable(uint8_t op)
{
    switch (op & 0xF8) {
    case 0x80: case 0x88: case 0x90: case 0x98: case 0xA0: return true;
    default: return false;
    }
}

inline uint32_t RunIndices(uint8_t op, uint32_t n)
{
    switch (op & 0xF8) {
    case 0x90: return n / 3u * 3u;
    case 0x98: case 0xA0: return n >= 3u ? (n - 2u) * 3u : 0u;
    default: return n / 4u * 6u;
    }
}

// Parses whole commands from [p, p + n) and returns the bytes they took; an
// incomplete command at the end is left for the next call.
size_t Parse(const uint8_t* p, size_t n, bool inList)
{
    const uint8_t* const start = p;
    const uint8_t* const end = p + n;
    while (p < end) {
        const size_t have = static_cast<size_t>(end - p);
        const uint8_t op = *p;

        if (op >= 0x80 && op < 0xC0) {
            if (have < 3)
                break;
            const unsigned vat = op & 7;
            const uint16_t count = Rd16(p + 1);
            const uint32_t vsize = VertexSize(vat);
            const size_t total = 3u + static_cast<size_t>(count) * vsize;
            if (have < total)
                break;
            if (vsize == 0 && count != 0) {
                ++g_unknownOpcodes;
                p += 3;
                continue;
            }
            // Display lists only. FIFO commands live in the ring, which is
            // handed back to the game the moment this parse returns; a run's
            // SPU job reads its commands later and could find them rewritten.
            if (inList && g_runsEnabled && g_sink && g_sink->draw_run && RunJoinable(op) && count != 0) {
                const uint8_t* runEnd = p + total;
                const uint8_t* q = runEnd;
                uint32_t prims = 1, verts = count, indices = RunIndices(op, count);
                for (;;) {
                    while (q < end && *q == 0x00)
                        ++q;
                    if (end - q < 3)
                        break;
                    const uint8_t op2 = *q;
                    if (op2 < 0x80 || op2 >= 0xC0 || (op2 & 7) != vat || !RunJoinable(op2))
                        break;
                    const uint16_t c2 = Rd16(q + 1);
                    const size_t t2 = 3u + static_cast<size_t>(c2) * vsize;
                    const uint32_t i2 = RunIndices(op2, c2);
                    if (static_cast<size_t>(end - q) < t2 || verts + c2 > kRunMaxVerts ||
                        indices + i2 > kRunMaxIndices || static_cast<size_t>(q + t2 - p) > kRunMaxBytes ||
                        prims >= kRunMaxPrims)
                        break;
                    q += t2;
                    runEnd = q;
                    ++prims;
                    verts += c2;
                    indices += i2;
                }
                if (prims > 1) {
                    g_sink->draw_run(g_sink->ctx, vat, p, static_cast<uint32_t>(runEnd - p), prims, verts,
                                     indices, vsize);
                    g_draws += prims;
                    g_vertices += verts;
                    p = runEnd;
                    continue;
                }
            }
            if (g_sink && g_sink->draw)
                g_sink->draw(g_sink->ctx, op & 0xF8, vat, count, p + 3, vsize);
            ++g_draws;
            g_vertices += count;
            p += total;
            continue;
        }

        switch (op) {
        case 0x00:   // NOP
        case 0x44:   // metrics
        case 0x48:   // invalidate vertex cache
            ++p;
            continue;
        case 0x08:
            if (have < 6)
                goto incomplete;
            LoadCp(p[1], Rd32(p + 2));
            p += 6;
            continue;
        case 0x10: {
            if (have < 5)
                goto incomplete;
            const uint32_t header = Rd32(p + 1);
            const unsigned count = ((header >> 16) & 0xF) + 1;
            const size_t total = 5u + count * 4u;
            if (have < total)
                goto incomplete;
            if (g_sink && g_sink->load_xf) {
                uint32_t values[16];
                std::memcpy(values, p + 5, count * 4u);
                g_sink->load_xf(g_sink->ctx, static_cast<uint16_t>(header), values, count);
            }
            p += total;
            continue;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {
            if (have < 5)
                goto incomplete;
            const uint32_t word = Rd32(p + 1);
            if (g_sink && g_sink->load_index)
                g_sink->load_index(g_sink->ctx, op, static_cast<uint16_t>(word >> 16),
                                   static_cast<uint16_t>(word & 0xFFF),
                                   static_cast<uint8_t>(((word >> 12) & 0xF) + 1));
            p += 5;
            continue;
        }
        case 0x40: {
            if (have < 9)
                goto incomplete;
            const uint32_t addr = Rd32(p + 1);
            const uint32_t size = Rd32(p + 5);
            if (inList) {
                // One level only on hardware.
                ++g_unknownOpcodes;
            } else {
                CallList(addr, size);
            }
            p += 9;
            continue;
        }
        case 0x61:
            if (have < 5)
                goto incomplete;
            LoadBp(Rd32(p + 1));
            p += 5;
            continue;
        default:
            if (g_unknownOpcodes++ < 8)
                std::printf("[gx] unknown opcode %02x %s\n", op, inList ? "in a display list" : "in the FIFO");
            ++p;
            continue;
        }
    incomplete:
        break;
    }
    return static_cast<size_t>(p - start);
}

inline uint64_t Timebase()
{
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

std::atomic<bool> g_run{false};
uint64_t g_gpuThread = 0;

// The latest scan-out request. A newer request replaces an unseen one, as a
// later retrace's commit would on hardware.
std::mutex g_presentLock;
struct PresentRequest {
    uint32_t xfb, width, height;
    bool black;
};
PresentRequest g_present{};
std::atomic<uint32_t> g_presentSerial{0};
uint32_t g_presentSeen = 0;

void ServicePresent()
{
    const uint32_t serial = g_presentSerial.load(std::memory_order_acquire);
    if (serial == g_presentSeen)
        return;
    g_presentSeen = serial;
    PresentRequest req;
    {
        std::lock_guard<std::mutex> lock(g_presentLock);
        req = g_present;
    }
    if (g_sink && g_sink->present)
        g_sink->present(g_sink->ctx, req.xfb, req.width, req.height, req.black);
}

extern "C" void mkw_prof_parse_begin(void) __attribute__((weak));
extern "C" void mkw_prof_parse_end(void) __attribute__((weak));

void GpuThreadMain(void*)
{
    MkwPinArenaOnThisThread();
    mkw_rescue_register_thread(1);
    mkw_hprof_attach(0);
    if (g_sink && g_sink->start)
        g_sink->start(g_sink->ctx);
    uint64_t consumed = 0;
    while (g_run.load(std::memory_order_acquire)) {
        ServicePresent();
        const uint64_t written = g_written.load(std::memory_order_acquire);
        if (written == consumed) {
            if (g_sink && g_sink->idle)
                g_sink->idle(g_sink->ctx);
            std::unique_lock<std::mutex> lock(g_wakeLock);
            g_consumerWaiting.store(true, std::memory_order_release);
            if (g_written.load(std::memory_order_acquire) == consumed && g_run.load())
                g_wake.wait_for(lock, std::chrono::milliseconds(2));
            g_consumerWaiting.store(false, std::memory_order_release);
            continue;
        }

        const uint64_t t0 = Timebase();
        mkw_prof_parse_begin();
        const size_t off = static_cast<size_t>(consumed % kRingBytes);
        const size_t avail = static_cast<size_t>(written - consumed);
        const size_t first = (off + avail <= kRingBytes) ? avail : kRingBytes - off;
        size_t used = Parse(g_ring + off, first, false);
        if (used == first && first < avail) {
            // Ran exactly to the wrap; continue from the start next round.
        } else if (used < first && off + avail > kRingBytes) {
            // A command straddles the wrap: move its head below the ring so it
            // reads as one span with its tail.
            const size_t head = first - used;
            if (head <= kPreSlack) {
                std::memcpy(g_ring - head, g_ring + off + used, head);
                used += Parse(g_ring - head, head + (avail - first), false);
            } else {
                std::printf("[gx] %u-byte command across the ring wrap; dropped\n",
                            static_cast<unsigned>(head));
                used = first;
            }
        }
        consumed += used;
        g_bytesParsed.store(consumed, std::memory_order_relaxed);
        g_consumed.store(consumed, std::memory_order_release);
        mkw_prof_parse_end();
        g_gpuBusyTicks += Timebase() - t0;
        if (used == 0) {
            // Incomplete command and nothing newer yet.
            std::unique_lock<std::mutex> lock(g_wakeLock);
            g_consumerWaiting.store(true, std::memory_order_release);
            if (g_written.load(std::memory_order_acquire) == written && g_run.load())
                g_wake.wait_for(lock, std::chrono::milliseconds(2));
            g_consumerWaiting.store(false, std::memory_order_release);
        }
    }
    mkw_lv2_thread_exit();
}

bool g_initialized = false;

// Runs one guest interrupt handler from the OS table, the way the external
// interrupt vector would: interrupts off, the interrupted context as argument.
void RunGuestHandler(CpuContext* cpu, uint32_t interrupt)
{
    using namespace OsHleInternal;
    const uint32_t mask = 0x80000000u >> interrupt;
    if (g_interrupt_mask.load(std::memory_order_acquire) & mask)
        return;
    const uint32_t table = Memory::Read32(kInterruptHandlerTablePtrAddr);
    if (table == 0)
        return;
    const uint32_t handler = Memory::Read32(table + interrupt * 4u);
    if (handler == 0)
        return;
    const uint32_t context = Memory::Read32(kOSCurrentContextAddr);
    const uint32_t savedR3 = cpu->gpr[3], savedR4 = cpu->gpr[4];
    const int32_t level = OS__DisableInterrupts_801a65ac();
    OS_HLE_BeginDeferredGuestCallbacks();
    cpu->gpr[3] = interrupt;
    cpu->gpr[4] = context;
    try {
        InvokeIndirectCpu(handler, cpu);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        OS__RestoreInterrupts_801a65d4(level);
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
    OS__RestoreInterrupts_801a65d4(level);
    cpu->gpr[3] = savedR3;
    cpu->gpr[4] = savedR4;
}

} // namespace

// ---------------------------------------------------------------------------

void SetSink(const GxSink* sink)
{
    g_sink = sink;
}

void Initialize()
{
    if (g_initialized)
        return;
    if (!GuestFlat::BackShadow(0x0C000000u, 1u << 20, "gx-regs") ||
        !GuestFlat::BackShadow(0x0D000000u, 1u << 20, "hw-regs")) {
        std::printf("[gx] register shadow unavailable\n");
        std::abort();
    }
    g_shadow = MkwGuestArenaBase() + 0x0C000000u;
    if (FILE* f = std::fopen("/dev_hdd0/tmp/mkwii-noruns.txt", "rb")) {
        std::fclose(f);
        g_runsEnabled = false;
        std::printf("[gx] primitive runs disabled by flag file\n");
    }
    // The command processor idles until the guest links the FIFOs.
    SetShadow16(kCpStatus, 0x000C);

    // The ring lives in the guest arena, so a vertex in the FIFO has a guest
    // address (kRingGuestBase + offset) like a vertex in a display list, and
    // the renderer and its SPU jobs read both the same way.
    if (!GuestFlat::BackShadow(kRingFold, kRingFoldBytes, "gx-ring")) {
        std::printf("[gx] ring allocation failed\n");
        std::abort();
    }
    g_ringMem = MkwGuestArenaBase() + kRingFold;
    g_ring = g_ringMem + kPreSlack;
    // Until GXInit links the FIFOs every store records into the stage, whose
    // bursts go nowhere while the PI has no window.
    GxPipe::g_pipe.cursor = g_ring;
    GxPipe::g_pipe.limit = g_ring + kChunk;
    g_recording = false;
    EnterRecording();

    // The GPU thread hosts the whole renderer: give it a real stack and the
    // game thread's priority, so each owns one of the PPE's two hardware
    // threads.
    g_run.store(true);
    if (mkw_lv2_thread_create(&g_gpuThread, GpuThreadMain, nullptr, 1000, 1u << 20, "mkw_gpu") != 0) {
        std::printf("[gx] GPU thread creation failed\n");
        std::abort();
    }
    g_initialized = true;
    std::printf("[gx] front end up: %u KiB ring, GPU thread started\n", kRingBytes >> 10);
}

void Shutdown()
{
    if (!g_initialized)
        return;
    g_run.store(false);
    {
        std::lock_guard<std::mutex> lock(g_wakeLock);
        g_wake.notify_one();
    }
    mkw_lv2_thread_join(g_gpuThread);
    g_initialized = false;
}

void MmioWrite(uint32_t addr, uint64_t value, unsigned size)
{
    const uint32_t off = addr & 0x01FFFFFFu;
    if (!g_shadow)
        return;
    uint8_t* const dst = g_shadow + off;
    switch (off) {
    case kCpStatus:
        // Only the breakpoint bit is acknowledged here; the rest is status.
        SetShadow16(kCpStatus, static_cast<uint16_t>(Shadow16(kCpStatus) & ~(value & 0x10)));
        return;
    case kCpClear:
        SetShadow16(kCpStatus, static_cast<uint16_t>(Shadow16(kCpStatus) & ~(value & 0x3)));
        return;
    case kPeInterrupt: {
        // Enables are plain bits; the two status bits are write-one-to-clear.
        const uint16_t old = Shadow16(kPeInterrupt);
        const uint16_t v = static_cast<uint16_t>(value);
        SetShadow16(kPeInterrupt, static_cast<uint16_t>((v & 0x3) | (old & 0xC & ~(v & 0xC))));
        return;
    }
    default:
        break;
    }
    switch (size) {
    case 1: *dst = static_cast<uint8_t>(value); break;
    case 2: { const uint16_t v = static_cast<uint16_t>(value); std::memcpy(dst, &v, 2); break; }
    case 4: { const uint32_t v = static_cast<uint32_t>(value); std::memcpy(dst, &v, 4); break; }
    default: std::memcpy(dst, &value, 8); break;
    }
    if (off == kCpControl)
        UpdateRouting();
    else if (off == kPiFifoWptr && g_recording)
        GxPipe::g_pipe.cursor = g_stage;   // a new window starts with an empty pipe
}

uint64_t MmioRead(uint32_t addr, unsigned size)
{
    if (!g_shadow)
        return 0;
    const uint8_t* src = g_shadow + (addr & 0x01FFFFFFu);
    switch (size) {
    case 1: return *src;
    case 2: { uint16_t v; std::memcpy(&v, src, 2); return v; }
    case 4: { uint32_t v; std::memcpy(&v, src, 4); return v; }
    default: { uint64_t v; std::memcpy(&v, src, 8); return v; }
    }
}

void NotifyGuestRamWrite(uint32_t addr, uint32_t size)
{
    if (g_sink && g_sink->ram_written)
        g_sink->ram_written(g_sink->ctx, addr, size);
}

void Publish()
{
    if (!g_initialized)
        return;
    if (g_recording)
        PublishRing(g_lapBase + static_cast<uint64_t>(g_ringCursor - g_ring));
    else
        PublishRing(ProducerPosition());
}

bool InterruptPending()
{
    return g_finishRaised.load(std::memory_order_acquire) != g_finishDelivered ||
           g_tokenRaised.load(std::memory_order_acquire) != g_tokenDelivered;
}

bool IdleWaitUntil(std::chrono::steady_clock::time_point until)
{
    std::unique_lock<std::mutex> lock(g_idleLock);
    return g_idleWake.wait_until(lock, until, [] { return InterruptPending(); });
}

void ServiceInterrupts(CpuContext* cpu)
{
    if (!g_initialized || !cpu)
        return;
    Publish();
    if (!OS_HLE_InterruptsEnabled())
        return;

    const uint32_t finish = g_finishRaised.load(std::memory_order_acquire);
    if (finish != g_finishDelivered) {
        g_finishDelivered = finish;
        // Raised interrupts coalesce until acknowledged, as on hardware.
        const uint16_t pe = Shadow16(kPeInterrupt);
        SetShadow16(kPeInterrupt, static_cast<uint16_t>(pe | 0x8));
        if (pe & 0x2)
            RunGuestHandler(cpu, kOsInterruptPeFinish);
    }
    const uint32_t token = g_tokenRaised.load(std::memory_order_acquire);
    if (token != g_tokenDelivered) {
        g_tokenDelivered = token;
        const uint16_t pe = Shadow16(kPeInterrupt);
        SetShadow16(kPeInterrupt, static_cast<uint16_t>(pe | 0x4));
        if (pe & 0x1)
            RunGuestHandler(cpu, kOsInterruptPeToken);
    }
}

void RequestPresent(uint32_t xfb, uint32_t width, uint32_t height, bool black)
{
    if (!g_initialized)
        return;
    {
        std::lock_guard<std::mutex> lock(g_presentLock);
        g_present = PresentRequest{xfb, width, height, black};
    }
    g_presentSerial.fetch_add(1, std::memory_order_release);
    WakeConsumer();
}

namespace {
std::atomic<uint64_t> g_guestIdleTicks{0};
}

void NoteGuestIdle(uint64_t ticks)
{
    g_guestIdleTicks.fetch_add(ticks, std::memory_order_relaxed);
}

Stats GetStats()
{
    Stats s{};
    s.bytesWritten = g_bytesWritten.load(std::memory_order_relaxed);
    s.bytesParsed = g_bytesParsed.load(std::memory_order_relaxed);
    s.listBytesParsed = g_listBytesParsed;
    s.draws = g_draws;
    s.vertices = g_vertices;
    s.listCalls = g_listCalls;
    s.listBytesRecorded = g_listBytesRecorded.load(std::memory_order_relaxed);
    s.unknownOpcodes = g_unknownOpcodes;
    s.finishes = g_finishes.load(std::memory_order_relaxed);
    s.tokens = g_tokens.load(std::memory_order_relaxed);
    s.producerStalls = g_producerStalls.load(std::memory_order_relaxed);
    // Cell timebase: 79.8 MHz.
    s.gpuBusyNanos = g_gpuBusyTicks * 10000ull / 798ull;
    s.guestIdleNanos = g_guestIdleTicks.load(std::memory_order_relaxed) * 10000ull / 798ull;
    return s;
}

} // namespace GxHw

namespace GxPipe {

uint8_t* Overflow()
{
    if (GxHw::g_recording)
        GxHw::StageOverflow();
    else
        GxHw::RingOverflow();
    return g_pipe.cursor;
}

void WriteLong(const uint8_t* data, uint32_t bytes)
{
    while (bytes) {
        const uint32_t n = bytes < 32 ? bytes : 32;
        std::memcpy(g_pipe.cursor, data, n);
        Advance(n);
        data += n;
        bytes -= n;
    }
}

} // namespace GxPipe
