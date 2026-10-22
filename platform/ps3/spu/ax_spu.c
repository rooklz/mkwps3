/* ax_spu.c -- the AX voice mix on an SPU.
 *
 * The PPU runs everything in an AX command list except the voices: it sets the buses
 * up, hands the PB list to this program, and picks the buses up again for the aux and
 * output commands (ax_mix.cpp). Here each running voice is decoded, resampled, filtered
 * and mixed exactly as the PPU would (ax_spu_core.h), with guest memory reached by DMA.
 * The SPU and the guest are both big-endian, so PBs and samples come across as they are.
 *
 * The SPU waits on its inbound mailbox between jobs, so it costs nothing while idle. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>
#include <string.h>
#include "ax_spu_job.h"

#define TAG_JOB 0
#define TAG_PB  1   /* PB reads and writes, in order (reads are fenced behind writes) */
#define TAG_BUS 2
#define TAG_SMP 3

#define MEM1_SIZE 0x01800000u
#define MEM2_SIZE 0x04000000u

static AxSpuJob    job    __attribute__((aligned(128)));
static AxSpuResult result __attribute__((aligned(128)));
static int32_t bus[12][96]  __attribute__((aligned(128)));
static int32_t wmbus[8][18] __attribute__((aligned(128)));
static int16_t coef[0x800]  __attribute__((aligned(128)));
static uint64_t coef_loaded;

static uint64_t g_mem1, g_mem2;
static uint32_t g_bad;

/* MixResolveRange: the host address of a guest range in MEM1 or MEM2 through any of their
 * aliases, and where that region ends; 0 when the range isn't wholly inside one. */
static uint64_t guest_ea(uint32_t addr, uint32_t bytes, uint64_t *region_end)
{
    uint32_t off;
    if (bytes == 0 || bytes > MEM2_SIZE || addr > 0xFFFFFFFFu - (bytes - 1))
        return 0;
    if (addr < MEM1_SIZE || addr - 0x80000000u < MEM1_SIZE || addr - 0xC0000000u < MEM1_SIZE) {
        off = addr < MEM1_SIZE ? addr : (addr - 0x80000000u < MEM1_SIZE ? addr - 0x80000000u : addr - 0xC0000000u);
        if (bytes > MEM1_SIZE - off || !g_mem1)
            return 0;
        *region_end = g_mem1 + MEM1_SIZE;
        return g_mem1 + off;
    }
    if (addr - 0x10000000u < MEM2_SIZE || addr - 0x90000000u < MEM2_SIZE || addr - 0xD0000000u < MEM2_SIZE) {
        off = addr - 0x10000000u < MEM2_SIZE ? addr - 0x10000000u
            : (addr - 0x90000000u < MEM2_SIZE ? addr - 0x90000000u : addr - 0xD0000000u);
        if (bytes > MEM2_SIZE - off || !g_mem2)
            return 0;
        *region_end = g_mem2 + MEM2_SIZE;
        return g_mem2 + off;
    }
    return 0;
}

/* Decrementer ticks spent waiting for PB and sample reads, for the result line. */
static uint32_t g_wait_pb, g_wait_smp;

static void wait_tag(int tag)
{
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

static void wait_tag_timed(int tag, uint32_t *ticks)
{
    const uint32_t t0 = spu_read_decrementer();
    wait_tag(tag);
    *ticks += t0 - spu_read_decrementer();
}

/* ---- PBs: the last 128-aligned span read, kept current with our own writes ---------- */

/* 32 KB: AX keeps its PBs in one array, so the running checks, next pointers and PBs of
 * every voice usually come in one window. */
static uint8_t pb_line[32 * 1024] __attribute__((aligned(128)));
static uint64_t pb_line_ea;
static uint32_t pb_line_len;
static uint32_t pb_line_guest;   /* the guest address pb_line starts at, as last asked for */

static void axs_guest_get(void *dst, uint32_t addr, uint32_t bytes)
{
    uint64_t end = 0, ea;
    if (pb_line_len && addr - pb_line_guest < pb_line_len && bytes <= pb_line_len - (addr - pb_line_guest)) {
        memcpy(dst, pb_line + (addr - pb_line_guest), bytes);
        return;
    }
    ea = guest_ea(addr, bytes, &end);
    if (!ea) {
        memset(dst, 0, bytes);
        ++g_bad;
        return;
    }
    if (!(pb_line_len && ea >= pb_line_ea && ea + bytes <= pb_line_ea + pb_line_len)) {
        const uint64_t base = ea & ~127ull;
        uint64_t len = sizeof pb_line, done;
        if (base + len > end)
            len = end - base;          /* region ends are 16-aligned */
        for (done = 0; done < len; done += 16384)
            mfc_getf(pb_line + done, base + done, (uint32_t)(len - done < 16384 ? len - done : 16384), TAG_PB, 0, 0);
        wait_tag_timed(TAG_PB, &g_wait_pb);
        pb_line_ea = base;
        pb_line_len = (uint32_t)len;
    }
    pb_line_guest = addr - (uint32_t)(ea - pb_line_ea);
    memcpy(dst, pb_line + (ea - pb_line_ea), bytes);
}

/* Writes go out at their exact size: neighbouring guest bytes are the game's. Staged with
 * the destination's alignment, as the MFC requires for the small transfers at the ends. */
static uint8_t put_stage[16 * 1024] __attribute__((aligned(128)));
static uint32_t put_used;

static void axs_guest_put(uint32_t addr, const void *src, uint32_t bytes)
{
    uint64_t end = 0, ea = guest_ea(addr, bytes, &end);
    uint8_t *ls;
    uint32_t n = bytes;
    if (!ea) {
        ++g_bad;
        return;
    }
    if (pb_line_len && ea < pb_line_ea + pb_line_len && ea + bytes > pb_line_ea) {
        const uint64_t lo = ea > pb_line_ea ? ea : pb_line_ea;
        const uint64_t hi = ea + bytes < pb_line_ea + pb_line_len ? ea + bytes : pb_line_ea + pb_line_len;
        memcpy(pb_line + (lo - pb_line_ea), (const uint8_t *)src + (lo - ea), (size_t)(hi - lo));
    }
    if (put_used + bytes + 32 > sizeof put_stage) {
        wait_tag(TAG_PB);
        put_used = 0;
    }
    ls = put_stage + put_used + (uint32_t)(ea & 15);
    memcpy(ls, src, bytes);
    put_used = (put_used + (uint32_t)(ea & 15) + bytes + 15) & ~15u;
    while (n && (ea & 15)) {
        uint32_t s = (ea & 1) ? 1 : (ea & 2) ? 2 : (ea & 4) ? 4 : 8;
        while (s > n) s >>= 1;
        mfc_put(ls, ea, s, TAG_PB, 0, 0);
        ls += s; ea += s; n -= s;
    }
    if (n >= 16) {
        const uint32_t m = n & ~15u;
        mfc_put(ls, ea, m, TAG_PB, 0, 0);
        ls += m; ea += m; n -= m;
    }
    while (n) {
        uint32_t s = 8;
        while (s > n) s >>= 1;
        mfc_put(ls, ea, s, TAG_PB, 0, 0);
        ls += s; ea += s; n -= s;
    }
}

/* ---- samples: a 4 KB window of ARAM ------------------------------------------------ */

static uint8_t smp[4096] __attribute__((aligned(128)));
static uint32_t smp_lo, smp_hi;   /* ARAM addresses held, [lo, hi); empty when equal */

/* AramAliasesFor + MixResolveRange: ARAM is MEM1 below bit 28 (offsets past 24 MB read as
 * silence) and MEM2 above it. Returns 0 when nothing backs the address. */
static uint64_t aram_ea(uint32_t addr, uint32_t *avail)
{
    if (addr & 0x10000000u) {
        const uint32_t off = addr & (MEM2_SIZE - 1u);
        *avail = MEM2_SIZE - off;
        return g_mem2 ? g_mem2 + off : 0;
    }
    {
        const uint32_t off = addr & 0x01ffffffu;
        if (off >= MEM1_SIZE || !g_mem1)
            return 0;
        *avail = MEM1_SIZE - off;
        return g_mem1 + off;
    }
}

static const uint8_t *axs_aram_range(uint32_t addr, uint32_t bytes)
{
    if (!(addr >= smp_lo && addr - smp_lo + bytes <= smp_hi - smp_lo)) {
        uint32_t avail = 0, len;
        const uint64_t ea = aram_ea(addr, &avail);
        const uint32_t skew = (uint32_t)(ea & 15);
        if (!ea)
            return 0;
        len = avail + skew;
        if (len > sizeof smp)
            len = sizeof smp;
        mfc_get(smp, ea - skew, len, TAG_SMP, 0, 0);
        wait_tag_timed(TAG_SMP, &g_wait_smp);
        smp_lo = addr - skew;
        smp_hi = smp_lo + len;
        if (bytes > len - skew)
            return 0;
    }
    return smp + (addr - smp_lo);
}

static uint8_t axs_aram_byte(uint32_t addr)
{
    const uint8_t *p = axs_aram_range(addr, 1);
    return p ? *p : 0;
}

#include "ax_spu_core.h"

/* ---- the job loop ---------------------------------------------------------------- */

static void run_job(void)
{
    int32_t *const buses[AXS_BUS_COUNT] = {
        bus[0], bus[1], bus[2], bus[3], bus[4], bus[5], bus[6], bus[7], bus[8], bus[9], bus[10], bus[11],
        wmbus[0], wmbus[1], wmbus[2], wmbus[3], wmbus[4], wmbus[5], wmbus[6], wmbus[7],
    };
    AxsSegment seg[3];
    uint32_t i, pbs = 0;
    const uint32_t t0 = spu_read_decrementer();

    g_mem1 = job.mem1_ea;
    g_mem2 = job.mem2_ea;
    g_bad = 0;
    g_wait_pb = g_wait_smp = 0;
    axs_v_check = (job.flags & AX_SPU_CHECK_VECTOR) != 0;
    axs_v_mismatch = 0;
    pb_line_len = 0;
    put_used = 0;
    smp_lo = smp_hi = 0;   /* the game may have written new samples since the last frame */
    for (i = 0; i < 3; ++i) {
        seg[i].guest = job.seg[i][0];
        seg[i].host = job.seg[i][1];
        seg[i].bytes = job.seg[i][2];
    }
    if (job.coef_ea && job.coef_ea != coef_loaded) {
        mfc_get(coef, job.coef_ea, sizeof coef, TAG_BUS, 0, 0);
        coef_loaded = job.coef_ea;
    }
    mfc_get(bus, job.bus_ea, sizeof bus, TAG_BUS, 0, 0);
    mfc_get(wmbus, job.wmbus_ea, sizeof wmbus, TAG_BUS, 0, 0);
    wait_tag(TAG_BUS);

    result.voices = axs_process_pb_list(job.pb_addr, buses, seg, job.nseg > 3 ? 3 : job.nseg,
                                        job.coef_ea ? coef : 0, (job.flags & AX_SPU_COEFFS_FIT32) != 0,
                                        (job.flags & AX_SPU_NEW_FILTER) != 0, &pbs);

    mfc_put(bus, job.bus_ea, sizeof bus, TAG_BUS, 0, 0);
    mfc_put(wmbus, job.wmbus_ea, sizeof wmbus, TAG_BUS, 0, 0);
    wait_tag(TAG_BUS);
    wait_tag(TAG_PB);
    result.seq = job.seq;
    result.pbs = pbs;
    result.bad = g_bad;
    result.ticks = t0 - spu_read_decrementer();
    result.wait_pb = g_wait_pb;
    result.wait_smp = g_wait_smp;
    result.vector_mismatches = axs_v_mismatch;
    mfc_put(&result, job.result_ea, sizeof result, TAG_JOB, 0, 0);
    wait_tag(TAG_JOB);
}

int main(uint64_t job_ea, uint64_t result_ea, uint64_t arg3, uint64_t arg4)
{
    (void)arg3; (void)arg4;
    spu_write_decrementer(0xFFFFFFFFu);
    memset(&result, 0, sizeof result);
    result.voices = AX_SPU_HELLO;
    mfc_put(&result, result_ea, sizeof result, TAG_JOB, 0, 0);
    wait_tag(TAG_JOB);
    for (;;) {
        const uint32_t seq = spu_read_in_mbox();
        mfc_get(&job, job_ea, sizeof job, TAG_JOB, 0, 0);
        wait_tag(TAG_JOB);
        if (job.seq != seq)
            continue;   /* not a job the PPU finished writing; it never sends one */
        run_job();
    }
    return 0;
}
