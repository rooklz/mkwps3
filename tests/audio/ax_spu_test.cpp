// Host check of the SPU mixer (platform/ps3/spu/ax_spu.c, ax_spu_core.h) against the PPU's
// voice path (ax_voice.h): random PB chains over random sample data, mixed both ways. Every
// bus sample, every PB byte written back and the voice count have to match. The SPU program
// itself runs here, its DMA turned into checked copies (stub/spu_mfcio.h).
//
//   make -C tests/audio ax_spu_test && tests/audio/ax_spu_test
#include "ax_voice.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace AxDspHle {
std::atomic<uint8_t*> g_mixMem1{nullptr};
std::atomic<uint8_t*> g_mixMem2{nullptr};
// As on the mix worker: guest accesses outside MEM1/MEM2 read as 0 and writes are skipped,
// which is what the SPU does.
thread_local bool t_onMixWorker = true;
void ReportMixAddressOutOfRange(uint32_t, size_t) {}
std::atomic<uint32_t> g_aramWindowGeneration{1};

// ax_memory.cpp's ARAM resolution, over the test's MEM1 and MEM2.
static void AramAliases(uint32_t addr, uint32_t out[2])
{
    if (addr & 0x10000000u) {
        const uint32_t off = addr & static_cast<uint32_t>(Memory::kMem2Size - 1u);
        out[0] = 0x10000000u | off;
        out[1] = 0x90000000u | off;
    } else {
        const uint32_t off = addr & 0x01ffffffu;
        out[0] = off;
        out[1] = 0x80000000u | off;
    }
}

bool ResolveAramWindow(uint32_t addr, AramWindow& w)
{
    const uint32_t begin = addr & ~(kAramWindowSize - 1u);
    uint32_t cand[2];
    AramAliases(begin, cand);
    for (uint32_t c : cand) {
        if (const uint8_t* host = MixResolveRange(c, kAramWindowSize)) {
            w.begin = begin;
            w.end = begin + kAramWindowSize;
            w.generation = g_aramWindowGeneration.load();
            w.host = host;
            return true;
        }
    }
    w.begin = w.end = 0;
    w.host = nullptr;
    return false;
}

uint8_t ReadAramByteSlow(uint32_t addr)
{
    uint32_t cand[2];
    AramAliases(addr, cand);
    for (uint32_t c : cand)
        if (const uint8_t* host = MixResolveRange(c, 1))
            return *host;
    return 0;
}
} // namespace AxDspHle

// The SPU program, with its entry point renamed out of the way.
#define main ax_spu_main
#include "ax_spu.c"
#undef main

static_assert(sizeof(AxsPB) == sizeof(AxDspHle::AXPBWii), "AxsPB must mirror AXPBWii");
static_assert(offsetof(AxsPB, running) == offsetof(AxDspHle::AXPBWii, running), "AxsPB layout");
static_assert(offsetof(AxsPB, updates) == offsetof(AxDspHle::AXPBWii, updates), "AxsPB layout");
static_assert(offsetof(AxsPB, filt) == offsetof(AxDspHle::AXPBWii, hpf), "AxsPB layout");
static_assert(offsetof(AxsPB, remote) == offsetof(AxDspHle::AXPBWii, remote), "AxsPB layout");
static_assert(offsetof(AxsPB, remote_iir) == offsetof(AxDspHle::AXPBWii, remote_iir), "AxsPB layout");
static_assert(offsetof(AxsPB, pad) == offsetof(AxDspHle::AXPBWii, pad), "AxsPB layout");

namespace {

std::mt19937 rng(20261021);
uint32_t Rnd(uint32_t n) { return n ? rng() % n : 0u; }

std::vector<uint8_t> g_guestMem1(Memory::kMem1Size);
std::vector<uint8_t> g_guestMem2(Memory::kMem2Size);
alignas(128) int16_t g_coefFit[0x800];
alignas(128) int16_t g_coefWide[0x800];

uint8_t* Host(uint32_t addr, size_t bytes) { return AxDspHle::MixResolveRange(addr, bytes); }

// A sample position in the accelerator's units (nibbles, bytes or halfwords) for a format,
// mostly in MEM1 or MEM2, now and then near a region end or where nothing is mapped.
uint32_t SampleAddress(uint16_t format)
{
    uint32_t byte;
    switch (Rnd(10)) {
    case 0: byte = 0x01800000u + Rnd(0x7f0000u); break;                  // MEM1 hole: silence
    case 1: byte = 0x01800000u - 1 - Rnd(6000); break;                   // end of MEM1
    case 2: byte = 0x14000000u - 1 - Rnd(6000); break;                   // end of MEM2
    case 3: case 4: case 5: byte = 0x10000000u + Rnd(0x03ff0000u); break;
    default: byte = Rnd(0x017f0000u); break;
    }
    const uint32_t size = format & 3u;
    return size == 0 ? byte * 2 : (size == 1 ? byte : byte / 2);
}

void RandomPB(AxDspHle::AXPBWii& pb)
{
    std::memset(&pb, 0, sizeof pb);
    uint16_t* raw = reinterpret_cast<uint16_t*>(&pb);
    for (size_t i = 0; i < sizeof pb / 2; ++i)
        raw[i] = static_cast<uint16_t>(rng());
    pb.running = Rnd(4) ? 1 : static_cast<uint16_t>(Rnd(3));
    pb.is_stream = Rnd(4) == 0;
    pb.src_type = static_cast<uint16_t>(Rnd(10) < 7 ? 0 : Rnd(4));
    pb.coef_select = static_cast<uint16_t>(Rnd(10) < 8 ? Rnd(4) : Rnd(8));
    uint32_t ratio;
    switch (Rnd(10)) {
    case 0: ratio = 0x00010000u; break;
    case 1: ratio = Rnd(0x8000u); break;
    case 2: ratio = 0x00100000u + Rnd(0x00500000u); break;          // > 1024 samples a frame
    default: ratio = 0x4000u + Rnd(0x3c000u); break;
    }
    pb.src.ratio_hi = static_cast<uint16_t>(ratio >> 16);
    pb.src.ratio_lo = static_cast<uint16_t>(ratio);
    const uint32_t f = Rnd(10);
    const uint16_t format = f < 6 ? 0 : (f < 8 ? 0x0a : 0x19);
    pb.audio_addr.sample_format = format;
    pb.audio_addr.looping = Rnd(2);
    const uint32_t start = SampleAddress(format);
    uint32_t len = Rnd(4) == 0 ? Rnd(40) + 1 : Rnd(5000) + 1;
    uint32_t end = start + len;
    if (format == 0 && Rnd(2))
        end = (end & ~15u) + Rnd(2);
    const uint32_t cur = start + Rnd(len + 4);
    pb.audio_addr.loop_addr_hi = static_cast<uint16_t>(start >> 16);
    pb.audio_addr.loop_addr_lo = static_cast<uint16_t>(start);
    pb.audio_addr.end_addr_hi = static_cast<uint16_t>(end >> 16);
    pb.audio_addr.end_addr_lo = static_cast<uint16_t>(end);
    pb.audio_addr.cur_addr_hi = static_cast<uint16_t>(cur >> 16);
    pb.audio_addr.cur_addr_lo = static_cast<uint16_t>(cur);
    if (Rnd(2))
        for (auto& c : pb.adpcm.coefs) c = static_cast<int16_t>(static_cast<int16_t>(rng()) >> 4);
    pb.lpf.on = Rnd(3) == 0;
    pb.biquad.on = Rnd(3) == 0;
    pb.remote = Rnd(3) == 0;
    pb.remote_iir.on = static_cast<uint16_t>(Rnd(3));
    // Mostly the channel bits the game uses, sometimes anything.
    uint32_t control = Rnd(4) ? (rng() & 0x7fff001fu) : rng();
    pb.mixer_control_hi = static_cast<uint16_t>(control >> 16);
    pb.mixer_control_lo = static_cast<uint16_t>(control);
    if (Rnd(3) == 0)
        pb.vol_env.cur_volume_delta = 0;
}

struct Buses {
    alignas(128) int32_t regular[12][96];
    alignas(128) int32_t wm[8][18];
};

} // namespace

int main()
{
    using namespace AxDspHle;
    for (auto& b : g_guestMem1) b = static_cast<uint8_t>(rng());
    for (auto& b : g_guestMem2) b = static_cast<uint8_t>(rng());
    g_mixMem1 = g_guestMem1.data();
    g_mixMem2 = g_guestMem2.data();
    for (uint32_t i = 0; i < 0x800; ++i) {
        g_coefFit[i] = static_cast<int16_t>(static_cast<int16_t>(rng()) >> 3);   // |sum| of four < 0x10000
        g_coefWide[i] = static_cast<int16_t>(rng());
    }

    static Buses refBus, spuBus, initBus;
    long failures = 0, voices = 0, pbsTotal = 0;
    const int scenarios = 6000;
    for (int scenario = 0; scenario < scenarios; ++scenario) {
        const PBLayout layout = static_cast<PBLayout>(1 + Rnd(3));
        const bool newFilter = Rnd(2);
        const bool fit32 = Rnd(2);
        const int16_t* coeffs = Rnd(5) ? (fit32 ? g_coefFit : g_coefWide) : nullptr;

        // A chain of PBs, 32-aligned like AX allocates them, in MEM1 or MEM2.
        const uint32_t count = 1 + Rnd(40);
        const bool inMem2 = Rnd(4) == 0;
        uint32_t addr = (inMem2 ? 0x90000000u + Rnd(0x03f00000u) : 0x80000000u + Rnd(0x01700000u)) & ~31u;
        std::vector<uint32_t> addrs;
        for (uint32_t i = 0; i < count; ++i) {
            addrs.push_back(addr);
            addr += ((sizeof(AXPBWii) + 31) & ~31u) + Rnd(4) * 32;
        }
        const uint32_t spanBegin = addrs.front();
        const uint32_t spanBytes = addr - spanBegin + 64;
        for (uint32_t i = 0; i < count; ++i) {
            AXPBWii pb;
            RandomPB(pb);
            const uint32_t next = i + 1 < count ? addrs[i + 1] : (Rnd(10) == 0 ? 0x7000000u + Rnd(0x1000u) : 0u);
            pb.next_pb_hi = static_cast<uint16_t>(next >> 16);
            pb.next_pb_lo = static_cast<uint16_t>(next);
            pb.this_pb_hi = static_cast<uint16_t>(addrs[i] >> 16);
            pb.this_pb_lo = static_cast<uint16_t>(addrs[i]);
            WritePBWithLayout(addrs[i], pb, layout);
        }
        uint8_t* span = Host(spanBegin, spanBytes);
        if (!span) {
            --scenario;
            continue;
        }
        const std::vector<uint8_t> before(span, span + spanBytes);
        for (auto& row : initBus.regular) for (auto& v : row) v = static_cast<int32_t>(rng()) >> Rnd(20);
        for (auto& row : initBus.wm) for (auto& v : row) v = static_cast<int32_t>(rng()) >> Rnd(20);

        // The PPU: ProcessPBList's loop for the new command layouts.
        refBus = initBus;
        AXBuffers buffers{};
        for (size_t i = 0; i < 12; ++i) buffers[i] = refBus.regular[i];
        for (size_t i = 0; i < 8; ++i) buffers[12 + i] = refBus.wm[i];
        uint32_t refVoices = 0;
        {
            uint32_t pbAddr = spanBegin, guard = 0;
            while (pbAddr && guard++ < 256) {
                if (MixRead16(pbAddr + static_cast<uint32_t>(offsetof(AXPBWii, running))) != 1) {
                    pbAddr = Hilo(MixRead16(pbAddr), MixRead16(pbAddr + 2));
                    continue;
                }
                AXPBWii pb{};
                ReadPBWithLayout(pbAddr, pb, layout);
                const AXMixControl control = ConvertMixerControl(Hilo(pb.mixer_control_hi, pb.mixer_control_lo));
                refVoices += ProcessVoice(pb, buffers, kAxSamplesPerFrame, control, coeffs, newFilter, fit32);
                WritePBWithLayout(pbAddr, pb, layout);
                pbAddr = Hilo(pb.next_pb_hi, pb.next_pb_lo);
            }
        }
        const std::vector<uint8_t> refAfter(span, span + spanBytes);
        std::memcpy(span, before.data(), spanBytes);

        // The SPU: a job as ax_mix.cpp builds one, run by the SPU program.
        spuBus = initBus;
        std::array<PBSegment, 3> segments{};
        const size_t nseg = PBSegmentsForLayout(layout, segments);
        std::memset(&job, 0, sizeof job);
        job.mem1_ea = reinterpret_cast<uintptr_t>(g_guestMem1.data());
        job.mem2_ea = reinterpret_cast<uintptr_t>(g_guestMem2.data());
        job.bus_ea = reinterpret_cast<uintptr_t>(spuBus.regular);
        job.wmbus_ea = reinterpret_cast<uintptr_t>(spuBus.wm);
        job.coef_ea = reinterpret_cast<uintptr_t>(coeffs);
        static AxSpuResult posted;
        job.result_ea = reinterpret_cast<uintptr_t>(&posted);
        job.pb_addr = spanBegin;
        job.seq = static_cast<uint32_t>(scenario + 1);
        job.flags = (newFilter ? AX_SPU_NEW_FILTER : 0u) | (fit32 ? AX_SPU_COEFFS_FIT32 : 0u);
        job.nseg = static_cast<uint32_t>(nseg);
        for (size_t i = 0; i < nseg; ++i) {
            job.seg[i][0] = static_cast<uint16_t>(segments[i].guestOffset);
            job.seg[i][1] = static_cast<uint16_t>(segments[i].structOffset);
            job.seg[i][2] = static_cast<uint16_t>(segments[i].byteCount);
        }
        run_job();
        voices += refVoices;
        pbsTotal += posted.pbs;

        bool bad = posted.seq != job.seq || posted.voices != refVoices;
        if (std::memcmp(&refBus, &spuBus, sizeof refBus) != 0)
            bad = true;
        if (std::memcmp(refAfter.data(), span, spanBytes) != 0)
            bad = true;
        if (bad && ++failures <= 5) {
            std::printf("scenario %d: layout %d newFilter %d fit32 %d coeffs %d, voices %u/%u\n", scenario,
                        static_cast<int>(layout), newFilter, fit32, coeffs != nullptr, posted.voices, refVoices);
            for (size_t i = 0; i < 12 * 96; ++i)
                if ((&refBus.regular[0][0])[i] != (&spuBus.regular[0][0])[i]) {
                    std::printf("  first bus difference: bus %zu sample %zu: %d vs %d\n", i / 96, i % 96,
                                (&refBus.regular[0][0])[i], (&spuBus.regular[0][0])[i]);
                    break;
                }
            for (uint32_t i = 0; i < spanBytes; ++i)
                if (refAfter[i] != span[i]) {
                    std::printf("  first PB difference at +%#x\n", i);
                    break;
                }
        }
        std::memcpy(span, before.data(), spanBytes);
    }
    std::printf("%d scenarios, %ld voices, %ld PBs walked, %llu DMAs: %ld mismatches\n", scenarios, voices,
                pbsTotal, static_cast<unsigned long long>(g_spuTestDmas), failures);
    return failures ? 1 : 0;
}
