// Host test for runtime/src/hle/thp_decode_core.h (the native THPVideoDecode).
//
//   clang++ -std=c++17 -O2 -I runtime/src/hle tools/thp-test/thp_test.cpp -o thp_test
//   thp_test <outdir> <file.thp>... [--png 0,60,300] [--time 300] [--compare 30]
//            [--ffyuv raw.yuv --ffframes 5]
//
// For every file: decodes the first frames with the core into GX I8 tiled
// planes (as the game's texture buffers receive them), de-tiles them, compares
// against an independent reference decoder (bit-serial Huffman, double-precision
// direct IDCT, same trunc/clamp), prints a hash of the tiled output (to compare
// builds, e.g. the VMX path on a big-endian PowerPC host against the portable
// one), writes PNGs of the requested frames, and times the core. --ffyuv
// compares the first file against `ffmpeg -f rawvideo -pix_fmt yuv420p` output.
// A built-in self test covers the status codes, marker handling, DRI, scan
// alignment and the unaligned-output staging path.

#include "thp_decode_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t BE16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(n));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// ---- THP container ----------------------------------------------------------

struct ThpFile {
    std::vector<uint8_t> data;
    uint32_t version = 0, numFrames = 0, firstFrameSize = 0, compOffset = 0, firstFrameOffset = 0;
    float fps = 0;
    uint32_t numComponents = 0;
    uint8_t compTypes[16] = {};
    uint32_t width = 0, height = 0, videoType = 0;
    bool hasAudio = false;
};

bool OpenThp(const std::string& path, ThpFile& t)
{
    if (!ReadFile(path, t.data) || t.data.size() < 0x30 || std::memcmp(t.data.data(), "THP\0", 4) != 0)
        return false;
    const uint8_t* d = t.data.data();
    t.version = BE32(d + 4);
    uint32_t fpsBits = BE32(d + 16);
    std::memcpy(&t.fps, &fpsBits, 4);
    t.numFrames = BE32(d + 20);
    t.firstFrameSize = BE32(d + 24);
    t.compOffset = BE32(d + 32);
    t.firstFrameOffset = BE32(d + 40);
    const uint8_t* c = d + t.compOffset;
    t.numComponents = BE32(c);
    std::memcpy(t.compTypes, c + 4, 16);
    const uint8_t* info = c + 20;
    for (uint32_t i = 0; i < t.numComponents; ++i) {
        if (t.compTypes[i] == 0) {
            t.width = BE32(info);
            t.height = BE32(info + 4);
            info += 8;
            if (t.version == 0x11000) {
                t.videoType = BE32(info);
                info += 4;
            }
        } else if (t.compTypes[i] == 1) {
            t.hasAudio = true;
            info += t.version == 0x11000 ? 16 : 12;
        }
    }
    return t.width != 0;
}

// Frame i: [next total size][prev total size][component sizes...][data...].
struct FrameRef {
    uint32_t offset, size, videoOffset, videoSize;
};

std::vector<FrameRef> ListFrames(const ThpFile& t, uint32_t maxFrames)
{
    std::vector<FrameRef> frames;
    uint32_t off = t.firstFrameOffset, size = t.firstFrameSize;
    const uint32_t n = std::min(t.numFrames, maxFrames);
    for (uint32_t i = 0; i < n && off + size <= t.data.size(); ++i) {
        const uint8_t* h = t.data.data() + off;
        FrameRef r;
        r.offset = off;
        r.size = size;
        r.videoOffset = 8 + 4 * t.numComponents;
        uint32_t compSize = 0;
        for (uint32_t c = 0; c < t.numComponents; ++c) {
            if (t.compTypes[c] == 0) {
                r.videoSize = BE32(h + 8 + 4 * c);
                break;
            }
            compSize += BE32(h + 8 + 4 * c);
        }
        r.videoOffset += compSize;
        frames.push_back(r);
        off += size;
        size = BE32(h);
    }
    return frames;
}

// 32-byte aligned buffer with a byte offset, like the game's read buffers.
struct Buffer {
    std::unique_ptr<uint8_t[]> raw;
    uint8_t* p = nullptr;
    size_t size = 0;
    void Alloc(size_t n, size_t misalign = 0)
    {
        raw.reset(new uint8_t[n + 64 + misalign]);
        std::memset(raw.get(), 0, n + 64 + misalign);
        p = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw.get()) + 31) & ~uintptr_t(31)) + misalign;
        size = n;
    }
};

// ---- planes -------------------------------------------------------------------

struct Planes {
    uint32_t w = 0, h = 0; // Y size (h rounded to the bands written)
    std::vector<uint8_t> y, u, v;
};

void Detile(const uint8_t* tiled, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
    // GX I8: 8x4 tiles of 32 bytes, w/8 tiles per tile row.
    out.resize(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            out[size_t(y) * w + x] = tiled[(y >> 2) * (w * 4) + (x >> 3) * 32 + (y & 3) * 8 + (x & 7)];
}

struct CoreOutput {
    Buffer y, u, v;
    uint32_t sizeY = 0, sizeC = 0;
};

ThpCore::Frame g_frame;
ThpCore::Workspace g_work;

int32_t CoreDecode(const uint8_t* src, size_t avail, CoreOutput& out, size_t outMisalign = 0)
{
    const int32_t st = ThpCore::ParseHeaders(src, src + avail, g_frame);
    if (st != ThpCore::kOk)
        return st;
    const uint32_t bands = ThpCore::BandCount(g_frame);
    out.sizeY = bands * ThpCore::LumaBandBytes(g_frame);
    out.sizeC = bands * ThpCore::ChromaBandBytes(g_frame);
    if (out.y.size != out.sizeY || out.y.p == nullptr || outMisalign) {
        out.y.Alloc(out.sizeY, outMisalign);
        out.u.Alloc(out.sizeC, outMisalign);
        out.v.Alloc(out.sizeC, outMisalign);
    }
    if (!ThpCore::DecodeScan(g_frame, src + avail, g_work, out.y.p, out.u.p, out.v.p))
        return -1;
    return 0;
}

void CoreToPlanes(const CoreOutput& o, Planes& p)
{
    const uint32_t bands = ThpCore::BandCount(g_frame);
    p.w = g_frame.width;
    p.h = bands * 16;
    Detile(o.y.p, p.w, p.h, p.y);
    Detile(o.u.p, p.w / 2, p.h / 2, p.u);
    Detile(o.v.p, p.w / 2, p.h / 2, p.v);
}

// ---- reference decoder ------------------------------------------------------
// Written from the JPEG spec and the THPDec reading, sharing no code with the core.

struct RefHuff {
    int maxcode[18], valptr[17], mincode[17];
    uint8_t vals[256];
    bool defined = false;
};

struct RefBits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t bitpos = 0;
    int Bit()
    {
        const uint64_t byte = bitpos >> 3;
        const int b = (p + byte < end) ? (p[byte] >> (7 - (bitpos & 7))) & 1 : 0;
        ++bitpos;
        return b;
    }
    int Bits(int n)
    {
        int v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | Bit();
        return v;
    }
};

int RefDecode(RefBits& br, const RefHuff& h)
{
    if (!h.defined)
        return 0; // THPDec's zeroed table: symbol 0, no bits
    int code = 0;
    for (int l = 1; l <= 16; ++l) {
        code = (code << 1) | br.Bit();
        if (h.maxcode[l] >= 0 && code <= h.maxcode[l])
            return h.vals[(h.valptr[l] + code - h.mincode[l]) & 255];
    }
    return 0; // no code of 16 bits or less: symbol 0 after 16 bits, as the core
}

int RefExtend(int v, int s) { return s == 0 ? 0 : (v < (1 << (s - 1)) ? v - (1 << s) + 1 : v); }

struct RefState {
    int q[4][64] = {};
    RefHuff huff[4];
    int w = 0, h = 0;
    int compQ[3] = {}, compDc[3] = {}, compAc[3] = {};
    bool restart = false;
    int restartInterval = 0;
};

const int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                         41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                         30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

void RefIdct(const int* coef, const int* q, uint8_t* out, int stride)
{
    static double cosT[8][8];
    static bool init = false;
    if (!init) {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                cosT[x][u] = (u == 0 ? std::sqrt(0.5) : 1.0) * std::cos((2 * x + 1) * u * M_PI / 16.0);
        init = true;
    }
    double tmp[8][8];
    for (int v = 0; v < 8; ++v)
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int u = 0; u < 8; ++u)
                s += cosT[x][u] * coef[v * 8 + u] * q[v * 8 + u];
            tmp[v][x] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int v = 0; v < 8; ++v)
                s += cosT[y][v] * tmp[v][x];
            int val = int(s / 4.0 + 128.0); // trunc toward zero, as psq_st
            out[y * stride + x] = uint8_t(val < 0 ? 0 : val > 255 ? 255 : val);
        }
}

bool RefDecodeFrame(const uint8_t* src, size_t avail, Planes& out)
{
    RefState st;
    const uint8_t* c = src;
    const uint8_t* end = src + avail;
    for (;;) {
        if (c[0] != 0xFF)
            return false;
        ++c;
        while (*c == 0xFF)
            ++c;
        const int m = *c++;
        if (m == 0xD8)
            continue;
        const int len = int(BE16(c));
        const uint8_t* seg = c + 2;
        if (m == 0xDB) {
            for (const uint8_t* s = seg; s < c + len; s += 65)
                for (int i = 0; i < 64; ++i)
                    st.q[s[0] & 3][kZigzag[i]] = s[1 + i];
        } else if (m == 0xC4) {
            for (const uint8_t* s = seg; s < c + len;) {
                const int idx = (s[0] & 15) * 2 + (s[0] >> 4);
                RefHuff& h = st.huff[idx];
                const uint8_t* counts = s + 1;
                int total = 0;
                for (int i = 0; i < 16; ++i)
                    total += counts[i];
                std::memcpy(h.vals, s + 17, total);
                int code = 0, k = 0;
                for (int l = 1; l <= 16; ++l) {
                    if (counts[l - 1] == 0) {
                        h.maxcode[l] = -1;
                    } else {
                        h.valptr[l] = k;
                        h.mincode[l] = code;
                        code += counts[l - 1];
                        k += counts[l - 1];
                        h.maxcode[l] = code - 1;
                    }
                    code <<= 1;
                }
                h.defined = true;
                s += 17 + total;
            }
        } else if (m == 0xC0) {
            st.h = int(BE16(seg + 1));
            st.w = int(BE16(seg + 3));
            for (int i = 0; i < 3; ++i)
                st.compQ[i] = seg[6 + i * 3 + 2] & 3;
        } else if (m == 0xDD) {
            st.restart = true;
            st.restartInterval = int(BE16(seg));
        } else if (m == 0xDA) {
            for (int i = 0; i < 3; ++i) {
                st.compDc[i] = (seg[2 + i * 2] >> 4) * 2;
                st.compAc[i] = (seg[2 + i * 2] & 15) * 2 + 1;
            }
            c += len;
            break;
        } else if ((m >= 0xE0 && m <= 0xEF) || m == 0xFE) {
            // skipped
        } else {
            return false;
        }
        c += len;
    }

    const int mcusX = (st.w + 15) / 16;
    const int bands = (st.h + 15) / 16;
    out.w = uint32_t(st.w);
    out.h = uint32_t(bands * 16);
    const int cw = st.w / 2;
    // The core writes MCU columns past w/16 into the tile layout's next tile
    // row; the reference only covers w % 16 == 0 frames.
    if (st.w % 16 != 0)
        return false;
    out.y.assign(size_t(st.w) * out.h, 0);
    out.u.assign(size_t(cw) * out.h / 2, 0);
    out.v.assign(size_t(cw) * out.h / 2, 0);

    RefBits br{c, end};
    int pred[3] = {0, 0, 0};
    int restartLeft = st.restartInterval;
    int coef[64];
    for (int by = 0; by < bands; ++by) {
        for (int mx = 0; mx < mcusX; ++mx) {
            for (int b = 0; b < 6; ++b) {
                const int comp = b < 4 ? 0 : b - 3;
                std::memset(coef, 0, sizeof(coef));
                const int s = RefDecode(br, st.huff[st.compDc[comp]]) & 15;
                pred[comp] = int16_t(pred[comp] + RefExtend(br.Bits(s), s));
                coef[0] = pred[comp];
                for (int k = 1; k < 64;) {
                    const int rs = RefDecode(br, st.huff[st.compAc[comp]]);
                    const int r = rs >> 4, sz = rs & 15;
                    if (sz) {
                        k += r;
                        coef[kZigzag[std::min(k, 63)]] = RefExtend(br.Bits(sz), sz);
                        ++k;
                    } else if (r == 15) {
                        k += 16;
                    } else {
                        break;
                    }
                }
                if (b < 4) {
                    const int x0 = mx * 16 + (b & 1) * 8, y0 = by * 16 + (b >> 1) * 8;
                    RefIdct(coef, st.q[st.compQ[0]], &out.y[size_t(y0) * st.w + x0], st.w);
                } else {
                    std::vector<uint8_t>& plane = b == 4 ? out.u : out.v;
                    RefIdct(coef, st.q[st.compQ[comp]], &plane[size_t(by * 8) * cw + mx * 8], cw);
                }
            }
            if (st.restart) {
                restartLeft = (restartLeft - 1) & 0xFFFF;
                if (restartLeft == 0) {
                    restartLeft = st.restartInterval;
                    br.bitpos = (br.bitpos + 7) & ~uint64_t(7);
                    pred[0] = pred[1] = pred[2] = 0;
                }
            }
        }
    }
    return true;
}

// ---- comparison / images ----------------------------------------------------

struct Diff {
    int maxAbs = 0;
    uint64_t nonzero = 0, total = 0;
    double sum = 0;
    void Add(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t aw, uint32_t bw, uint32_t w,
             uint32_t h)
    {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const int d = int(a[size_t(y) * aw + x]) - int(b[size_t(y) * bw + x]);
                maxAbs = std::max(maxAbs, std::abs(d));
                nonzero += d != 0;
                sum += d;
                ++total;
            }
    }
};

uint32_t Crc32(uint32_t crc, const uint8_t* p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// PNG with stored (uncompressed) deflate blocks: no zlib needed.
bool WritePng(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 3 + 1));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last)
            break;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8)
        z.push_back(uint8_t(adler >> s));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    auto put32 = [&](uint32_t v) {
        const uint8_t bytes[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        std::fwrite(bytes, 1, 4, f);
    };
    auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
        put32(uint32_t(n));
        uint32_t crc = Crc32(0, reinterpret_cast<const uint8_t*>(type), 4);
        if (n)
            crc = Crc32(crc, data, n);
        std::fwrite(type, 1, 4, f);
        if (n)
            std::fwrite(data, 1, n, f);
        put32(crc);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, f);
    const uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                              uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                              8, 2, 0, 0, 0};
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), z.size());
    chunk("IEND", nullptr, 0);
    std::fclose(f);
    return true;
}

void ToRgb(const Planes& p, uint32_t h, std::vector<uint8_t>& rgb)
{
    rgb.resize(size_t(p.w) * h * 3);
    const uint32_t cw = p.w / 2;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < p.w; ++x) {
            const double Y = p.y[size_t(y) * p.w + x];
            const double U = p.u[size_t(y / 2) * cw + x / 2] - 128.0;
            const double V = p.v[size_t(y / 2) * cw + x / 2] - 128.0;
            const double rgbv[3] = {Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U};
            for (int i = 0; i < 3; ++i)
                rgb[(size_t(y) * p.w + x) * 3 + i] = uint8_t(std::lround(std::clamp(rgbv[i], 0.0, 255.0)));
        }
}

std::string BaseName(const std::string& path)
{
    std::string b = path.substr(path.find_last_of('/') + 1);
    return b.substr(0, b.find_last_of('.'));
}

// ---- self test ----------------------------------------------------------------

int g_failures = 0;
void Check(bool ok, const char* what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    g_failures += !ok;
}

std::vector<uint8_t> Splice(const std::vector<uint8_t>& v, size_t at, const std::vector<uint8_t>& ins)
{
    std::vector<uint8_t> r(v.begin(), v.begin() + at);
    r.insert(r.end(), ins.begin(), ins.end());
    r.insert(r.end(), v.begin() + at, v.end());
    return r;
}

bool SameOutput(const CoreOutput& a, const CoreOutput& b)
{
    return a.sizeY == b.sizeY && std::memcmp(a.y.p, b.y.p, a.sizeY) == 0 && std::memcmp(a.u.p, b.u.p, a.sizeC) == 0 &&
           std::memcmp(a.v.p, b.v.p, a.sizeC) == 0;
}

int32_t DecodeBytes(const std::vector<uint8_t>& bytes, CoreOutput& out, size_t srcMisalign = 0, size_t outMisalign = 0)
{
    Buffer b;
    b.Alloc(bytes.size(), srcMisalign);
    std::memcpy(b.p, bytes.data(), bytes.size());
    return CoreDecode(b.p, bytes.size() + 32, out, outMisalign);
}

void SelfTest(const ThpFile& t)
{
    std::printf("self test (%ux%u frame 0):\n", t.width, t.height);
    const FrameRef fr = ListFrames(t, 1)[0];
    const std::vector<uint8_t> frame(t.data.begin() + fr.offset + fr.videoOffset,
                                     t.data.begin() + fr.offset + fr.videoOffset + fr.videoSize);
    CoreOutput base, o;
    Check(DecodeBytes(frame, base) == 0, "frame decodes");

    // Status codes.
    const std::vector<uint8_t> sof = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03,
                                      0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01};
    Check(DecodeBytes({0x00, 0xD8}, o) == 3, "no 0xFF before marker -> 3");
    Check(DecodeBytes({0xFF, 0xD8, 0xFF, 0xC2, 0x00, 0x02}, o) == 11, "SOF2 -> 11");
    Check(DecodeBytes({0xFF, 0xD8, 0xFF, 0xD9}, o) == 11, "EOI before SOS -> 11");
    Check(DecodeBytes({0xFF, 0xD8, 0xFF, 0xD0}, o) == 11, "RST0 -> 11");
    Check(DecodeBytes({0xFF, 0xD8, 0xFF, 0xF0, 0x00, 0x02}, o) == 11, "JPG0 (F0) -> 11");
    {
        std::vector<uint8_t> s = sof;
        s[6] = 12;
        Check(DecodeBytes(s, o) == 10, "SOF precision 12 -> 10");
        s = sof;
        s[11] = 1;
        Check(DecodeBytes(s, o) == 12, "SOF one component -> 12");
        s = sof;
        s[13] = 0x21;
        Check(DecodeBytes(s, o) == 19, "Y sampling 2x1 -> 19");
        s = sof;
        s[19] = 0x22;
        Check(DecodeBytes(s, o) == 19, "V sampling 2x2 -> 19");
        s = sof;
        const std::vector<uint8_t> sos = {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00};
        s.insert(s.end(), sos.begin(), sos.end());
        Check(DecodeBytes(s, o) == 15, "SOS without DHT -> 15");
        s = sof;
        const std::vector<uint8_t> sos1 = {0xFF, 0xDA, 0x00, 0x0C, 0x01, 0x01, 0x00};
        s.insert(s.end(), sos1.begin(), sos1.end());
        Check(DecodeBytes(s, o) == 12, "SOS one component -> 12");
    }

    // Marker handling that must not change the picture.
    Check(DecodeBytes(Splice(frame, 2, {0xFF, 0xFE, 0x00, 0x06, 'T', 'E', 'S', 'T'}), o) == 0 && SameOutput(base, o),
          "COM segment skipped");
    Check(DecodeBytes(Splice(frame, 2, {0xFF, 0xE1, 0x00, 0x04, 0x12, 0x34}), o) == 0 && SameOutput(base, o),
          "APP1 segment skipped");
    Check(DecodeBytes(Splice(frame, 2, {0xFF, 0xFF, 0xFF}), o) == 0 && SameOutput(base, o), "0xFF fill bytes skipped");
    Check(DecodeBytes(Splice(frame, 2, {0xFF, 0xDD, 0x00, 0x04, 0xFF, 0xFF}), o) == 0 && SameOutput(base, o),
          "DRI longer than the frame: no restart");

    // Scan start alignment (the bit reader starts at the aligned word).
    bool aligned = true;
    for (size_t mis = 1; mis < 4; ++mis)
        aligned = aligned && DecodeBytes(frame, o, mis) == 0 && SameOutput(base, o);
    Check(aligned, "source misaligned by 1..3 bytes");

    // Unaligned outputs go through the staging bands.
    CoreOutput mis;
    Check(DecodeBytes(frame, mis, 0, 8) == 0 && SameOutput(base, mis), "outputs misaligned by 8 bytes (staging)");

    // A real restart interval: the stream was not encoded for it, so both
    // decoders produce the same garbage if they agree on the semantics.
    {
        const std::vector<uint8_t> dri = Splice(frame, 2, {0xFF, 0xDD, 0x00, 0x04, 0x00, 0x07});
        CoreOutput r;
        Planes core, ref;
        const bool ok = DecodeBytes(dri, r) == 0;
        CoreToPlanes(r, core);
        Buffer b;
        b.Alloc(dri.size());
        std::memcpy(b.p, dri.data(), dri.size());
        const bool refOk = RefDecodeFrame(b.p, dri.size() + 32, ref);
        Diff d;
        if (ok && refOk)
            d.Add(core.y, ref.y, core.w, ref.w, core.w, core.h);
        Check(ok && refOk && d.maxAbs <= 1, "DRI 7: core matches reference restart handling");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <outdir> <file.thp>... [--png 0,60] [--time N] [--ffyuv f --ffframes N]\n",
                     argv[0]);
        return 2;
    }
    const std::string outDir = argv[1];
    std::vector<std::string> files;
    std::vector<uint32_t> pngFrames = {0};
    uint32_t timeFrames = 300, ffFrames = 0, compareFrames = 30;
    std::string ffyuv;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--png" && i + 1 < argc) {
            pngFrames.clear();
            for (const char* s = argv[++i]; *s;) {
                pngFrames.push_back(uint32_t(std::strtoul(s, const_cast<char**>(&s), 10)));
                if (*s == ',')
                    ++s;
            }
        } else if (a == "--time" && i + 1 < argc) {
            timeFrames = uint32_t(std::atoi(argv[++i]));
        } else if (a == "--compare" && i + 1 < argc) {
            compareFrames = uint32_t(std::atoi(argv[++i]));
        } else if (a == "--ffyuv" && i + 1 < argc) {
            ffyuv = argv[++i];
        } else if (a == "--ffframes" && i + 1 < argc) {
            ffFrames = uint32_t(std::atoi(argv[++i]));
        } else {
            files.push_back(a);
        }
    }
#if THP_CORE_VMX
    std::printf("IDCT path: VMX\n");
#else
    std::printf("IDCT path: portable VMX emulation\n");
#endif

    bool selfTested = false;
    for (size_t fi = 0; fi < files.size(); ++fi) {
        ThpFile t;
        if (!OpenThp(files[fi], t)) {
            std::printf("%s: not a THP file\n", files[fi].c_str());
            ++g_failures;
            continue;
        }
        const std::string name = BaseName(files[fi]);
        std::printf("%s: v%x %ux%u %.2f fps, %u frames, %u components%s\n", name.c_str(), t.version, t.width,
                    t.height, t.fps, t.numFrames, t.numComponents, t.hasAudio ? " (audio)" : "");
        if (!selfTested) {
            SelfTest(t);
            selfTested = true;
        }

        const uint32_t maxNeeded = std::max({timeFrames, compareFrames, ffFrames,
                                             pngFrames.empty() ? 0u : *std::max_element(pngFrames.begin(), pngFrames.end()) + 1});
        const std::vector<FrameRef> frames = ListFrames(t, maxNeeded);

        // Frames copied into 32-byte aligned buffers the way the THP reader
        // loads them (whole frame), decoding from the video component.
        std::vector<Buffer> bufs(frames.size());
        for (size_t i = 0; i < frames.size(); ++i) {
            bufs[i].Alloc(frames[i].size);
            std::memcpy(bufs[i].p, t.data.data() + frames[i].offset, frames[i].size);
        }

        // Reference comparison.
        Diff dy, duv;
        CoreOutput out;
        uint32_t compared = 0;
        int errors = 0;
        uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a over the tiled planes
        auto hashBytes = [&](const uint8_t* p, size_t n) {
            for (size_t k = 0; k < n; ++k)
                hash = (hash ^ p[k]) * 0x100000001B3ull;
        };
        for (uint32_t i = 0; i < std::min<uint32_t>(compareFrames, uint32_t(frames.size())); ++i) {
            const uint8_t* src = bufs[i].p + frames[i].videoOffset;
            const size_t avail = frames[i].size - frames[i].videoOffset + 32;
            if (CoreDecode(src, avail, out) != 0) {
                ++errors;
                continue;
            }
            hashBytes(out.y.p, out.sizeY);
            hashBytes(out.u.p, out.sizeC);
            hashBytes(out.v.p, out.sizeC);
            Planes core, ref;
            CoreToPlanes(out, core);
            if (!RefDecodeFrame(src, avail, ref)) {
                ++errors;
                continue;
            }
            dy.Add(core.y, ref.y, core.w, ref.w, core.w, core.h);
            duv.Add(core.u, ref.u, core.w / 2, ref.w / 2, core.w / 2, core.h / 2);
            duv.Add(core.v, ref.v, core.w / 2, ref.w / 2, core.w / 2, core.h / 2);
            ++compared;
        }
        std::printf("  vs reference (%u frames): Y max|d| %d, %.4f%% differ, mean %+.5f; UV max|d| %d, %.4f%% differ, mean %+.5f%s\n",
                    compared, dy.maxAbs, 100.0 * dy.nonzero / std::max<uint64_t>(dy.total, 1), dy.sum / std::max<uint64_t>(dy.total, 1),
                    duv.maxAbs, 100.0 * duv.nonzero / std::max<uint64_t>(duv.total, 1), duv.sum / std::max<uint64_t>(duv.total, 1),
                    errors ? " DECODE ERRORS" : "");
        std::printf("  tiled output hash (%u frames): %016llx\n", compared, static_cast<unsigned long long>(hash));
        g_failures += errors != 0 || dy.maxAbs > 1 || duv.maxAbs > 1;

        // ffmpeg comparison (first file only).
        if (fi == 0 && !ffyuv.empty() && ffFrames) {
            std::vector<uint8_t> ff;
            if (ReadFile(ffyuv, ff)) {
                const size_t fsz = size_t(t.width) * t.height * 3 / 2;
                Diff fy, fuv;
                for (uint32_t i = 0; i < ffFrames && (i + 1) * fsz <= ff.size() && i < frames.size(); ++i) {
                    if (CoreDecode(bufs[i].p + frames[i].videoOffset, frames[i].size - frames[i].videoOffset + 32, out) != 0)
                        continue;
                    Planes core;
                    CoreToPlanes(out, core);
                    std::vector<uint8_t> fY(ff.begin() + i * fsz, ff.begin() + i * fsz + size_t(t.width) * t.height);
                    std::vector<uint8_t> fU(ff.begin() + i * fsz + size_t(t.width) * t.height,
                                            ff.begin() + i * fsz + size_t(t.width) * t.height * 5 / 4);
                    std::vector<uint8_t> fV(ff.begin() + i * fsz + size_t(t.width) * t.height * 5 / 4,
                                            ff.begin() + (i + 1) * fsz);
                    fy.Add(core.y, fY, core.w, t.width, t.width, t.height);
                    fuv.Add(core.u, fU, core.w / 2, t.width / 2, t.width / 2, t.height / 2);
                    fuv.Add(core.v, fV, core.w / 2, t.width / 2, t.width / 2, t.height / 2);
                }
                std::printf("  vs ffmpeg (%u frames): Y max|d| %d, %.3f%% differ, mean %+.4f; UV max|d| %d, %.3f%% differ, mean %+.4f\n",
                            ffFrames, fy.maxAbs, 100.0 * fy.nonzero / std::max<uint64_t>(fy.total, 1),
                            fy.sum / std::max<uint64_t>(fy.total, 1), fuv.maxAbs,
                            100.0 * fuv.nonzero / std::max<uint64_t>(fuv.total, 1), fuv.sum / std::max<uint64_t>(fuv.total, 1));
            }
        }

        // PNGs.
        for (uint32_t idx : pngFrames) {
            if (idx >= frames.size())
                continue;
            if (CoreDecode(bufs[idx].p + frames[idx].videoOffset, frames[idx].size - frames[idx].videoOffset + 32, out) != 0)
                continue;
            Planes core;
            CoreToPlanes(out, core);
            std::vector<uint8_t> rgb;
            ToRgb(core, t.height, rgb);
            const std::string path = outDir + "/" + name + "_" + std::to_string(idx) + ".png";
            std::printf("  wrote %s\n", path.c_str());
            WritePng(path, rgb, core.w, t.height);
        }

        // Timing: whole decode (header parse + scan) per frame.
        const uint32_t n = std::min<uint32_t>(timeFrames, uint32_t(frames.size()));
        if (n) {
            uint64_t bytes = 0;
            const int reps = std::max(1, int(1200 / n));
            double best = 1e30;
            for (int r = 0; r < reps + 1; ++r) {
                const auto t0 = std::chrono::steady_clock::now();
                for (uint32_t i = 0; i < n; ++i) {
                    CoreDecode(bufs[i].p + frames[i].videoOffset, frames[i].size - frames[i].videoOffset + 32, out);
                    if (r == 0)
                        bytes += frames[i].videoSize;
                }
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (r > 0)
                    best = std::min(best, s);
            }
            std::printf("  timing: %.3f ms/frame over %u frames (%.1f KB/frame avg, %u MCUs/frame)\n", best * 1000.0 / n,
                        n, bytes / 1024.0 / n, uint32_t(g_frame.mcusPerRow) * ThpCore::BandCount(g_frame));
        }
    }
    std::printf("%s\n", g_failures ? "FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
