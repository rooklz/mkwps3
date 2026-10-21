// Host stand-in for the MFC: DMA becomes memcpy, after checking each transfer against the
// rules the real MFC enforces (an illegal one stops the SPU thread on hardware).
//
// The host is little-endian and the guest's PBs are big-endian, which the SPU reads as
// they are. So transfers on the PB tag (ax_spu.c's TAG_PB) swap each halfword, the way a
// big-endian SPU would see them; everything else (buses, coefficients, job lines) is
// host-native on both sides, and samples are bytes.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

inline uint64_t g_spuTestDmas = 0;

inline void SpuTestCheckDma(const void* ls, uint64_t ea, uint32_t size)
{
    const uintptr_t l = reinterpret_cast<uintptr_t>(ls);
    bool ok;
    if (size == 1 || size == 2 || size == 4 || size == 8)
        ok = (l & 15) == (ea & 15) && ea % size == 0;
    else
        ok = size != 0 && size % 16 == 0 && size <= 16384 && (l & 15) == 0 && (ea & 15) == 0;
    if (!ok) {
        std::fprintf(stderr, "illegal DMA: ls %#lx ea %#llx size %u\n", static_cast<unsigned long>(l),
                     static_cast<unsigned long long>(ea), size);
        std::abort();
    }
    ++g_spuTestDmas;
}

inline void SpuTestCopy(void* dst, const void* src, uint32_t size, int tag)
{
    if (tag != 1) {
        std::memcpy(dst, src, size);
        return;
    }
    if ((size & 1) || (reinterpret_cast<uintptr_t>(dst) & 1)) {
        std::fprintf(stderr, "PB DMA splits a halfword (size %u)\n", size);
        std::abort();
    }
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (uint32_t i = 0; i < size; i += 2) {
        const uint8_t a = s[i], b = s[i + 1];
        d[i] = b;
        d[i + 1] = a;
    }
}

#define mfc_get(ls, ea, size, tag, tid, rid) \
    (SpuTestCheckDma((ls), (ea), (size)), SpuTestCopy((ls), reinterpret_cast<const void*>(static_cast<uintptr_t>(ea)), (size), (tag)))
#define mfc_getf(ls, ea, size, tag, tid, rid) mfc_get(ls, ea, size, tag, tid, rid)
#define mfc_put(ls, ea, size, tag, tid, rid) \
    (SpuTestCheckDma((ls), (ea), (size)), SpuTestCopy(reinterpret_cast<void*>(static_cast<uintptr_t>(ea)), (ls), (size), (tag)))
#define mfc_write_tag_mask(mask) ((void)(mask))
#define mfc_read_tag_status_all() 0u
#define spu_read_decrementer() 0u
#define spu_write_decrementer(v) ((void)(v))
#define spu_read_in_mbox() 0u
