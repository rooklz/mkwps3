// Host-side check of the AX voice decoder and resampler fast paths against the
// straightforward versions they replaced, on random voices: every sample and every
// bit of the voice state left behind has to match.
//
//   make -C tests/audio
#include "ax_internal.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
namespace AxDspHle {
std::atomic<uint8_t*> g_mixMem1{nullptr};
std::atomic<uint8_t*> g_mixMem2{nullptr};
thread_local bool t_onMixWorker = false;
void ReportMixAddressOutOfRange(uint32_t, size_t) {}
}
namespace AxDspHle { std::atomic<uint32_t> g_aramWindowGeneration{1}; }
static std::vector<uint8_t> g_aram(1u << 20);
static bool PageFails(uint32_t page) { return page % 7 == 3; }
namespace AxDspHle {
bool ResolveAramWindow(uint32_t addr, AramWindow& w) {
    const uint32_t begin = addr & ~(kAramWindowSize - 1u);
    if (begin + kAramWindowSize > g_aram.size() || PageFails(begin >> kAramWindowShift)) {
        w.begin = w.end = 0; w.host = nullptr; return false;
    }
    w.begin = begin; w.end = begin + kAramWindowSize;
    w.generation = g_aramWindowGeneration.load(); w.host = g_aram.data() + begin;
    return true;
}
uint8_t ReadAramByteSlow(uint32_t addr) { return addr < g_aram.size() ? g_aram[addr] : 0; }
}
using namespace AxDspHle;
int main() {
    std::mt19937 rng(1234);
    auto rnd = [&](uint32_t n) { return n ? rng() % n : 0u; };
    for (auto& b : g_aram) b = rng();
    long failures = 0, samples = 0, runs = 0;
    for (int scenario = 0; scenario < 200000; ++scenario) {
        AXPBWii pb{};
        const uint32_t fmtPick = rnd(10);
        const uint16_t format = fmtPick < 8 ? 0 : (fmtPick == 8 ? 0x0a : 0x19);
        pb.audio_addr.sample_format = format;
        for (auto& c : pb.adpcm.coefs) c = static_cast<int16_t>(rng());
        if (rnd(2)) for (auto& c : pb.adpcm.coefs) c = static_cast<int16_t>(static_cast<int16_t>(rng()) >> 4);
        pb.adpcm.gain = rng();
        pb.adpcm.pred_scale = rng();
        pb.adpcm.yn1 = rng(); pb.adpcm.yn2 = rng();
        pb.adpcm_loop_info.pred_scale = rng(); pb.adpcm_loop_info.yn1 = rng(); pb.adpcm_loop_info.yn2 = rng();
        pb.audio_addr.looping = rnd(2);
        pb.is_stream = rnd(3) == 0;
        pb.running = 1;
        // Units: nibbles for ADPCM, bytes for PCM8, halfwords for PCM16.
        const uint32_t unitsPerByte2 = format == 0 ? 2 : 1;
        const uint32_t limit = format == 0x0a ? (uint32_t)g_aram.size() / 2 : (uint32_t)g_aram.size() * unitsPerByte2 / (format == 0 ? 1 : 1);
        uint32_t start = rnd(limit - 70000);
        uint32_t len = rnd(4) == 0 ? rnd(40) + 1 : rnd(3000) + 1;
        if (rnd(3) == 0) len = (len & ~15u) + rnd(3);
        uint32_t end = start + len;
        if (format == 0 && rnd(2)) { end = (end & ~15u) + rnd(2); }
        uint32_t cur = start + rnd(len + 4);
        if (rnd(8) == 0) cur |= 0x80000000u;
        if (format == 0 && rnd(4) == 0) cur = (cur & ~15u) | (2 + rnd(14));
        pb.audio_addr.loop_addr_hi = start >> 16; pb.audio_addr.loop_addr_lo = start;
        pb.audio_addr.end_addr_hi = end >> 16; pb.audio_addr.end_addr_lo = end;
        pb.audio_addr.cur_addr_hi = cur >> 16; pb.audio_addr.cur_addr_lo = cur;
        AXPBWii a = pb, b = pb;
        const int frames = 1 + rnd(6);
        for (int f = 0; f < frames; ++f) {
            const uint32_t n = 1 + rnd(rnd(4) == 0 ? 1024 : 200);
            std::vector<int16_t> oa(n), ob(n);
            Accelerator x; x.Setup(&a);
            for (uint32_t i = 0; i < n; ++i) oa[i] = x.ReadSample();
            x.Commit();
            Accelerator y; y.Setup(&b);
            y.ReadSamples(ob.data(), n);
            y.Commit();
            ++runs; samples += n;
            if (oa != ob || std::memcmp(&a, &b, sizeof a) != 0) {
                if (failures++ < 5) {
                    uint32_t i = 0; while (i < n && oa[i] == ob[i]) ++i;
                    std::printf("decode mismatch scenario %d frame %d fmt %x start %u end %u cur %x n %u first diff %u (%d vs %d) pbdiff %d\n",
                                scenario, f, format, start, end, cur, n, i, i < n ? oa[i] : 0, i < n ? ob[i] : 0,
                                std::memcmp(&a, &b, sizeof a) != 0);
                }
                break;
            }
        }
    }
    std::printf("decode: %ld runs, %ld samples, %ld failures\n", runs, samples, failures);

    // Resamplers.
    long rfail = 0;
    std::vector<int16_t> coefSmall(0x800), coefBig(0x800);
    for (size_t i = 0; i < 0x800; ++i) { coefSmall[i] = static_cast<int16_t>(rng()) >> 2; coefBig[i] = static_cast<int16_t>(rng()); }
    for (int t = 0; t < 300000; ++t) {
        const uint32_t count = rnd(3) ? 96 : 1 + rnd(96);
        uint32_t ratio = rnd(5) == 0 ? 0x10000 : 0x800 + rnd(0x40000);
        const uint16_t srcType = rnd(5) == 0 ? rnd(4) : rnd(2);
        uint32_t pos = rnd(0x10000);
        const bool big = rnd(2);
        const int16_t* coeffs = rnd(6) == 0 ? nullptr : (big ? coefBig.data() : coefSmall.data()) + 0x200 * rnd(4);
        const uint64_t needed = (srcType == 0 || srcType == 1) ? (uint64_t(pos) + uint64_t(count) * ratio) >> 16 : count;
        if (needed > 1024) continue;
        std::vector<int16_t> line(needed + 4);
        for (auto& v : line) v = rng();
        int16_t lastA[4], lastB[4];
        std::memcpy(lastA, line.data(), 8); std::memcpy(lastB, line.data(), 8);
        std::vector<int16_t> oa(count), ob(count);
        const uint32_t pa = ResampleAudio([&](uint32_t i) { return line[4 + i]; }, oa.data(), count, lastA, pos, ratio, srcType, coeffs);
        const uint32_t pb2 = ResampleLine(line.data(), ob.data(), count, lastB, pos, ratio, srcType, coeffs, !big);
        if (pa != pb2 || oa != ob || std::memcmp(lastA, lastB, 8) != 0) {
            if (rfail++ < 5) std::printf("resample mismatch t %d srcType %u ratio %x pos %x big %d coeffs %d\n", t, srcType, ratio, pos, big, coeffs != nullptr);
        }
    }
    std::printf("resample: %ld failures\n", rfail);
    return failures || rfail;
}
