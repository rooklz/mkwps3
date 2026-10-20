/* spu_vtx.c — PPU side of the SPU vertex-decode pipeline.
 *
 * Owns the SPU thread running vtx_spu.elf, the job ring in main memory, and
 * the vertex arena the SPU writes into. The arena is main memory mapped for
 * the RSX (gcmMapMainMemory -> GCM_LOCATION_CELL), because an SPU cannot DMA
 * into RSX local memory: the GPU fetches SPU-produced vertices from XDR.
 *
 * SAFETY: every wait on the SPU is bounded. An unbounded spin here once
 * turned a stalled SPU into a console that no longer serviced devlink, FTP
 * or webMAN and needed a physical power-cycle. On timeout the fast path is
 * disabled for the session and everything falls back to PPU decoding.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <sys/spu.h>
#include <rsx/rsx.h>
#include "../../common/log.h"
#include "../../core/mem/memmap.h"
#include "../../core/gx/gx.h"
#include "../../video/rsx/spu_vtx_shared.h"
#include "../../common/phase_prof.h"

#define SPU_ARENA_BYTES (8u * 1024u * 1024u)
#define RING_BYTES      (SPU_OFF_SLOTS + SPU_VTX_RING * sizeof(SpuVtxJobSlot))
#define SPIN_LIMIT      20000000u

static u8   *s_ring;              /* raw, 128-aligned; see shared header */
/* Last observed SPU consumer position. Advisory only: re-read on a near-miss.
 * DONE is written by the SPU, so reading it is a coherent load of a line this
 * core does not own -- an L2 miss on the critical path of every draw. */
static u32   s_done_cache;
static u32   s_nspu = 1;          /* consumers actually started */

/* How many SPUs to run as vertex consumers. The console has six; this used
 * one. dolphin-spucount.txt overrides, which is how a bad count is backed out
 * without a rebuild. */
static u32 spu_count_wanted(void)
{
    FILE *f = fopen("/dev_hdd0/tmp/dolphin-spucount.txt", "r");
    long v = 5;                   /* leave one SPU for the system */
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = 5; fclose(f); }
    if (v < 1) v = 1;
    if (v > SPU_VTX_MAX_SPU) v = SPU_VTX_MAX_SPU;
    return (u32)v;
}

static u8   *s_skin;              /* per-slot xf.mem snapshot for skinned draws */
static u8   *s_arena;             /* main memory, RSX-mapped */
static u32   s_arena_off;         /* RSX IO offset of the arena */
static int   s_on;
static u32   s_grp;
static sysSpuImage s_img;

u64 g_spu_jobs, g_spu_fallbacks, g_spu_join_timeouts, g_spu_too_big;

static volatile u32 *ring_word(u32 off)
{ return (volatile u32 *)(s_ring + off); }

/* The ring is drained when EVERY consumer has passed `h`. Each consumer's DONE
 * counts in the global index space (it starts at its id and steps by the
 * consumer count), so "all jobs below h are finished" is exactly "every DONE
 * is at or past h". */
static u32 spu_done_min(void)
{
    u32 i, m = *ring_word(SPU_OFF_DONE);
    for (i = 1; i < s_nspu; i++) {
        u32 d = *(volatile u32 *)(s_ring + SPU_OFF_DONE + i * SPU_LINE);
        if ((s32)(d - m) < 0) m = d;
    }
    return m;
}

int spu_vtx_consumer_stats(unsigned i, u32 *w8)
{
    unsigned k;
    if (!s_ring || i >= s_nspu)
        return 0;
    for (k = 0; k < 8; k++)
        w8[k] = *(volatile u32 *)(s_ring + SPU_OFF_HB + i * SPU_LINE + k * 4u);
    return 1;
}

int g_spu_vtx_enable = 1;
int  spu_vtx_active(void)      { return s_on && g_spu_vtx_enable; }

/* The arena is a ring across frames, not a buffer reset every frame.
 *
 * A frame's end only queues its flip; the RSX can still be drawing it, from
 * this arena, while the next frame's jobs are being written. Resetting at the
 * frame edge let those jobs overwrite what the GPU was about to fetch. With
 * plain vertex arrays that was a corrupt triangle; with index buffers it is an
 * index fetched from the next frame's vertex data, a vertex fetch outside the
 * mapping, and an RSX that never answers again.
 *
 * So space is reused only behind a fence. At each frame edge the renderer
 * writes a fence and marks the ring position (spu_vtx_frame_mark); once the
 * RSX has passed that fence everything allocated before the mark is free. An
 * allocation that would run into space still in use waits for the oldest
 * mark's fence. Positions are virtual (they only grow); the physical offset
 * is the position modulo the arena size, and an allocation that would straddle
 * the end skips to the start. */
#define ARENA_MARKS 8u
static struct { u32 serial; u64 pos; } s_marks[ARENA_MARKS];
static u32 s_mark_head, s_mark_tail;
static u64 s_vhead, s_vtail;
u64 g_spu_arena_waits, g_spu_arena_fails;

extern u32 rsx_fence_done(void);
extern int rsx_fence_wait(u32 serial);

/* Free what the oldest frame used, waiting for the RSX if `wait`. */
static int arena_retire_oldest(int wait)
{
    u32 serial;
    if (s_mark_tail == s_mark_head)
        return 0;
    serial = s_marks[s_mark_tail % ARENA_MARKS].serial;
    if ((s32)(rsx_fence_done() - serial) < 0) {
        if (!wait)
            return 0;
        g_spu_arena_waits++;
        if (!rsx_fence_wait(serial))
            return 0;
    }
    s_vtail = s_marks[s_mark_tail % ARENA_MARKS].pos;
    s_mark_tail++;
    return 1;
}

void spu_vtx_frame_mark(u32 serial)
{
    while (arena_retire_oldest(0))
        ;
    if (s_mark_head - s_mark_tail == ARENA_MARKS && !arena_retire_oldest(1)) {
        /* The GPU is not getting through frames at all. Forget the oldest
         * mark rather than the newest; allocation keeps waiting on fences. */
        s_mark_tail++;
    }
    s_marks[s_mark_head % ARENA_MARKS].serial = serial;
    s_marks[s_mark_head % ARENA_MARKS].pos = s_vhead;
    s_mark_head++;
}

static u8 *arena_alloc(u32 bytes, u32 *io_off_out)
{
    u64 phys;
    if (!s_on || bytes > SPU_ARENA_BYTES)
        return NULL;
    phys = s_vhead % SPU_ARENA_BYTES;
    if (phys + bytes > SPU_ARENA_BYTES) {
        s_vhead += SPU_ARENA_BYTES - phys;
        phys = 0;
    }
    while (s_vhead + bytes - s_vtail > SPU_ARENA_BYTES) {
        if (!arena_retire_oldest(1)) {
            g_spu_arena_fails++;
            return NULL;
        }
    }
    s_vhead += bytes;
    *io_off_out = s_arena_off + (u32)phys;
    return s_arena + phys;
}

void spu_vtx_init(void)
{
    FILE *f = fopen("/dev_hdd0/tmp/dolphin-spu.txt", "rb");
    if (!f) { LOG_INFO(LOG_CORE, "spu_vtx: flag absent, SPU path off"); return; }
    fclose(f);
    LOG_INFO(LOG_CORE, "spu_vtx: initialising");

    s_ring = (u8 *)memalign(128, RING_BYTES);
    if (!s_ring) { LOG_WARN(LOG_CORE, "spu_vtx: ring alloc failed (%u B) -- path off", (unsigned)RING_BYTES); return; }
    memset(s_ring, 0, RING_BYTES);
    s_skin = (u8 *)memalign(128, SPU_VTX_RING * SPU_SKIN_BYTES);
    if (!s_skin) { LOG_WARN(LOG_CORE, "spu_vtx: skin snapshot alloc failed -- skinned draws stay on PPU"); }

    s_arena = (u8 *)memalign(1024 * 1024, SPU_ARENA_BYTES);
    if (!s_arena) { LOG_WARN(LOG_CORE, "spu_vtx: arena alloc failed (%u B) -- path off", (unsigned)SPU_ARENA_BYTES); return; }
    if (gcmMapMainMemory(s_arena, SPU_ARENA_BYTES, &s_arena_off) != 0) {
        LOG_WARN(LOG_CORE, "spu_vtx: gcmMapMainMemory failed");
        return;
    }

    sysSpuInitialize(6, 0);
    {
        static u8 elf[96 * 1024];
        FILE *ef = fopen("/dev_hdd0/game/DOLPHIN01/USRDIR/vtx_spu.elf", "rb");
        size_t n;
        u32 thr;
        sysSpuThreadArgument arg;
        sysSpuThreadGroupAttribute gattr;
        sysSpuThreadAttribute tattr;
        static const char gname[] = "gxvtx";
        static const char tname[] = "vtxdec";
        int rc;

        if (!ef) { LOG_WARN(LOG_CORE, "spu_vtx: elf open failed"); return; }
        n = fread(elf, 1, sizeof elf, ef);
        fclose(ef);

        /* sysSpuImageImport returned nonsense segment counts on this
         * firmware; build the image from the ELF by hand instead. */
        {
            static sysSpuSegment segs[16];
            u32 entry = 0, nseg = 0;
            if (sysSpuElfGetInformation(elf, &entry, &nseg) != 0 ||
                nseg == 0 || nseg > 16) {
                LOG_WARN(LOG_CORE, "spu_vtx: elf info failed (n=%u)", (unsigned)n);
                return;
            }
            if (sysSpuElfGetSegments(elf, segs, nseg) != 0) {
                LOG_WARN(LOG_CORE, "spu_vtx: elf segments failed");
                return;
            }
            s_img.type = SPU_IMAGE_TYPE_USER;
            s_img.entryPoint = entry;
            s_img.segments = (u32)(uintptr_t)segs;
            s_img.segmentCount = nseg;
            LOG_INFO(LOG_CORE, "spu_vtx: image entry=%08x segs=%u", entry, nseg);
        }

        memset(&gattr, 0, sizeof gattr);
        gattr.nsize = sizeof gname; gattr.name = gname;
        s_nspu = spu_count_wanted();
        /* Tell the image how many consumers there are BEFORE the group runs:
         * each SPU reads this once at startup to size its stride. */
        *ring_word(SPU_OFF_NSPU) = s_nspu;
        __asm__ volatile("sync" ::: "memory");
        if ((rc = sysSpuThreadGroupCreate(&s_grp, s_nspu, 100, &gattr)) != 0) {
            LOG_WARN(LOG_CORE, "spu_vtx: group create rc=%08x", rc);
            return;
        }
        memset(&tattr, 0, sizeof tattr);
        tattr.nsize = sizeof tname; tattr.name = tname;
        tattr.option = SPU_THREAD_ATTR_NONE;
        {   u32 i;
            for (i = 0; i < s_nspu; i++) {
                memset(&arg, 0, sizeof arg);
                arg.arg0 = (u64)(uintptr_t)s_ring;
                arg.arg1 = (u64)i;          /* consumer index */
                if ((rc = sysSpuThreadInitialize(&thr, s_grp, i, &s_img,
                                                 &tattr, &arg)) != 0) {
                    LOG_WARN(LOG_CORE, "spu_vtx: thread %u init rc=%08x", i, rc);
                    return;
                }
            }
        }
        if ((rc = sysSpuThreadGroupStart(s_grp)) != 0) {
            LOG_WARN(LOG_CORE, "spu_vtx: group start rc=%08x", rc);
            return;
        }
    }

    /* Wait (bounded) for the SPU to prove it reached main() and can DMA. */
    {
        u32 spins = 0, i;
        for (i = 0; i < s_nspu; i++) {
            spins = 0;
            while (*(volatile u32 *)(s_ring + SPU_OFF_HB + i * SPU_LINE)
                   != 0xC0DE0001u) {
                if (++spins > SPIN_LIMIT) {
                    LOG_WARN(LOG_CORE,
                             "spu_vtx: consumer %u NO HEARTBEAT -- path off", i);
                    return;
                }
            }
        }
    }
    s_on = 1;
    LOG_INFO(LOG_CORE, "spu_vtx: ONLINE with %u consumer(s) "
             "(heartbeat ok, arena io=%08x)", s_nspu, s_arena_off);
}

/* How much PPU time the join actually costs, and how much of it is spinning.
 * This used to be charged to gx_fifo, because the call sites sit outside the
 * renderer's own profiled regions -- which made FIFO parsing look like a third
 * of the frame when much of that was the PPU waiting on the SPU. */
unsigned long long g_spu_join_calls;
unsigned long long g_spu_join_spun;    /* joins that spun at least once */
unsigned long long g_spu_join_spins;   /* total spin iterations         */

void spu_vtx_join(void)
{
    u32 spins = 0;
    if (!s_on) return;
    g_spu_join_calls++;
    prof_enter(PH_SPUWAIT);
    /* Spin politely.
     *
     * `or 1,1,1` is `cctpl` in the Cell handbook's nop table (§10.6.2): change
     * the current thread's priority to low. On a core whose two hardware
     * threads share dispatch, a busy-wait at normal priority takes issue slots
     * from whatever is doing real work -- today only this thread runs, so the
     * effect is small, but the moment anything else is scheduled on the
     * sibling this is the difference between a wait and a tax. Restored to
     * medium on the way out. */
    __asm__ __volatile__("or 1,1,1" ::: "memory");
    while ((s32)(spu_done_min() - *ring_word(SPU_OFF_HEAD)) < 0) {
        if (++spins > SPIN_LIMIT) {
            g_spu_join_spins += spins;
            if (spins) g_spu_join_spun++;
            prof_exit();
            g_spu_join_timeouts++;
            s_on = 0;
            LOG_WARN(LOG_CORE, "spu_vtx: JOIN TIMEOUT (head=%u done=%u) -- "
                     "SPU path disabled, PPU decode from here",
                     *ring_word(SPU_OFF_HEAD), *ring_word(SPU_OFF_DONE));
            *ring_word(SPU_OFF_HEAD) = spu_done_min();
            s_done_cache = spu_done_min();
            __asm__ __volatile__("or 2,2,2" ::: "memory");   /* cctpm */
            return;
        }
    }
    __asm__ __volatile__("or 2,2,2" ::: "memory");           /* cctpm */
    g_spu_join_spins += spins;
    if (spins) g_spu_join_spun++;
    prof_exit();
}

u8 *spu_vtx_reserve(u32 count, u32 *io_off_out)
{
    /* Whole PAIRS of vertices, not whole cache lines. Two 64-byte vertices
     * are 128 bytes, a multiple of 16, so every reservation stays DMA-aligned
     * -- and every offset in the arena is a whole number of vertices from
     * every other, which is what lets the renderer join consecutive draws
     * into one indexed batch over vertices from several jobs. (Line rounding
     * put 128-byte gaps between jobs that no vertex index can address.) The
     * SPU rounds its put to 16 bytes to match (spu/vtx_spu.c). */
    u32 bytes = ((count + 1u) & ~1u) * SPU_RV_BYTES;
    return arena_alloc(bytes, io_off_out);
}

/* Room for a run's triangle list, in the same arena so the RSX reads it from
 * main memory like the vertices. Rounded to whole vertex pairs, which keeps
 * every vertex reservation a whole number of vertices from every other. */
u8 *spu_vtx_reserve_idx(u32 count, u32 *io_off_out)
{
    u32 bytes = (count * 2u + SPU_RV_BYTES * 2u - 1u) / (SPU_RV_BYTES * 2u) * (SPU_RV_BYTES * 2u);
    return arena_alloc(bytes, io_off_out);
}

/* Skin snapshots: copies of the xf.mem prefix that skinned jobs DMA.
 *
 * They used to be filled per job, into the slot of the job about to be
 * published -- 4.6 KB copied for every primitive with a matrix index, even
 * when nothing had written XF since the previous one. A shape is usually a
 * run of strips under one matrix set, so the copy is now keyed by the XF
 * generation and reused until XF changes.
 *
 * Snapshots therefore outlive a single job and get their own ring. A slot is
 * refilled only once every job that reads it has finished: `s_skin_last` is
 * the head of the newest job handed that slot. */
static u32 s_skin_next;            /* next slot to fill */
static u32 s_skin_cur;             /* slot holding the current snapshot */
static u32 s_skin_gen;
static int s_skin_valid;
static u32 s_skin_last[SPU_VTX_RING];
static u8  s_skin_used[SPU_VTX_RING];
u64 g_spu_skin_copies, g_spu_skin_reuses;

/* Returns the host EA the SPU should DMA, or 0 if this draw must stay on the
 * PPU. Called immediately before submit(); both read the same HEAD. */
uint64_t spu_vtx_skin_store(const void *src, unsigned bytes, u32 gen)
{
    u32 h, slot, spins = 0;
    u8 *d;
    if (!s_on || !s_skin || !src || bytes == 0 || bytes > SPU_SKIN_BYTES)
        return 0;
    h = *ring_word(SPU_OFF_HEAD);
    if (s_skin_valid && s_skin_gen == gen) {
        s_skin_last[s_skin_cur] = h;
        g_spu_skin_reuses++;
        return (uint64_t)(uintptr_t)(s_skin + s_skin_cur * SPU_SKIN_BYTES);
    }
    slot = s_skin_next++ & (SPU_VTX_RING - 1u);
    if (s_skin_used[slot]) {
        /* Jobs below DONE are finished; the slot is free once the newest
         * job that read it is. That was 64 snapshots ago, so this is
         * normally answered from the cache. */
        while ((s32)(s_done_cache - s_skin_last[slot]) <= 0) {
            s_done_cache = spu_done_min();
            if (++spins > SPIN_LIMIT) {
                g_spu_join_timeouts++;
                s_on = 0;
                LOG_WARN(LOG_CORE, "spu_vtx: SKIN STALL -- SPU path disabled");
                return 0;
            }
        }
    }
    d = s_skin + slot * SPU_SKIN_BYTES;
    memcpy(d, src, bytes);
    if (bytes < SPU_SKIN_BYTES) memset(d + bytes, 0, SPU_SKIN_BYTES - bytes);
    s_skin_used[slot] = 1;
    s_skin_last[slot] = h;
    s_skin_cur = slot;
    s_skin_gen = gen;
    s_skin_valid = 1;
    g_spu_skin_copies++;
    return (uint64_t)(uintptr_t)d;
}

int spu_vtx_submit(const SpuVtxJob *job)
{
    u32 h, spins = 0;
    if (!s_on) return -1;
    h = *ring_word(SPU_OFF_HEAD);
    /* Only re-read DONE when the CACHED value says the ring might be full.
     * DONE is written by the SPU, so every read of it is a coherent load of a
     * line this core does not own -- an L2 miss on the critical path of every
     * single draw. In a race that is 15,817 reads a frame to answer a question
     * whose answer is almost always "plenty of room". */
    if (h - s_done_cache >= SPU_VTX_RING)
        s_done_cache = spu_done_min();
    while (h - s_done_cache >= SPU_VTX_RING) {
        s_done_cache = spu_done_min();
        if (++spins > SPIN_LIMIT) {
            g_spu_join_timeouts++;
            s_on = 0;
            LOG_WARN(LOG_CORE, "spu_vtx: RING STALL -- SPU path disabled");
            *ring_word(SPU_OFF_HEAD) = spu_done_min();
            s_done_cache = spu_done_min();
            return -1;
        }
    }
    memcpy(s_ring + SPU_OFF_SLOTS +
           (h & (SPU_VTX_RING - 1u)) * sizeof(SpuVtxJobSlot),
           job, sizeof *job);
    /* `lwsync`, not `sync`.
     *
     * What this barrier has to guarantee is store-store: the job slot must be
     * visible before the HEAD that publishes it. lwsync orders exactly that
     * among cacheable, coherent stores, which is what the ring is -- while
     * `sync` is the heavyweight barrier that additionally drains the store
     * queue and waits for every outstanding operation. Cell's PPE charges
     * hundreds of cycles for that, and it was paid TWICE on every draw.
     *
     * The second `sync`, after the HEAD store, guarded nothing at all: no
     * store after it needs ordering against it, and the SPU's view of HEAD is
     * already ordered by the barrier above. It is gone. */
    __asm__ volatile("lwsync" ::: "memory");
    *ring_word(SPU_OFF_HEAD) = h + 1;
    g_spu_jobs++;
    return 0;
}

/* The SPU's own accounting, published into the heartbeat line every 256 jobs.
 * Word 0 stays the heartbeat so the online check is unaffected; the rest are
 * decrementer ticks (79.8 MHz timebase, ~12.53 ns each). */
void spu_vtx_spustat(u32 *jobs, u32 *poll, u32 *work, u32 *dma)
{
    if (!s_ring) { *jobs = *poll = *work = *dma = 0; return; }
    *jobs = *ring_word(SPU_OFF_HB + 4);
    *poll = *ring_word(SPU_OFF_HB + 8);
    *work = *ring_word(SPU_OFF_HB + 12);
    *dma  = *ring_word(SPU_OFF_HB + 16);
}

void spu_vtx_stat(void (*emit)(const char *fmt, ...))
{
    if (!s_ring) { emit("spu: no ring"); return; }
    emit("spu: on=%d head=%u done=%u hb=%08x jobs=%llu fb=%llu timeouts=%llu",
         s_on, *ring_word(SPU_OFF_HEAD), *ring_word(SPU_OFF_DONE),
         *ring_word(SPU_OFF_HB), (unsigned long long)g_spu_jobs,
         (unsigned long long)g_spu_fallbacks,
         (unsigned long long)g_spu_join_timeouts);
}

void spu_vtx_shutdown(void)
{
    if (!s_ring) return;
    *ring_word(SPU_OFF_QUIT) = 1;
    __asm__ volatile("sync" ::: "memory");
    s_on = 0;
}
