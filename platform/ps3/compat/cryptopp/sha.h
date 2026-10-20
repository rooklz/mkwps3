#pragma once
// CryptoPP::SHA1 as the runtime uses it (NAND SHA handles, ES digests), as a
// self-contained SHA-1 for the PS3 build.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace CryptoPP {

class SHA1 {
public:
    static constexpr size_t DIGESTSIZE = 20;

    SHA1() { Restart(); }

    void Restart()
    {
        h_[0] = 0x67452301u; h_[1] = 0xEFCDAB89u; h_[2] = 0x98BADCFEu; h_[3] = 0x10325476u; h_[4] = 0xC3D2E1F0u;
        len_ = 0;
        fill_ = 0;
    }

    void Update(const uint8_t* data, size_t size)
    {
        len_ += size;
        while (size) {
            const size_t n = (64 - fill_) < size ? (64 - fill_) : size;
            std::memcpy(buf_ + fill_, data, n);
            fill_ += n;
            data += n;
            size -= n;
            if (fill_ == 64) {
                Block(buf_);
                fill_ = 0;
            }
        }
    }

    void Final(uint8_t* digest)
    {
        const uint64_t bits = len_ * 8;
        const uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (fill_ != 56)
            Update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        Update(lenBytes, 8);
        for (int i = 0; i < 5; ++i) {
            digest[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
        }
        Restart();
    }

    void TruncatedFinal(uint8_t* digest, size_t size)
    {
        uint8_t full[DIGESTSIZE];
        Final(full);
        std::memcpy(digest, full, size < DIGESTSIZE ? size : DIGESTSIZE);
    }

    void CalculateDigest(uint8_t* digest, const uint8_t* data, size_t size)
    {
        Restart();
        Update(data, size);
        Final(digest);
    }

private:
    static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void Block(const uint8_t* p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }

    uint32_t h_[5];
    uint64_t len_;
    size_t fill_;
    uint8_t buf_[64];
};

} // namespace CryptoPP
