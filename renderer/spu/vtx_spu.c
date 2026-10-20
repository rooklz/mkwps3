/* vtx_spu.c — SPU vertex decoder.
 *
 * Decodes GX vertex streams into RenderVertex records, replacing the PPU's
 * per-vertex C loop (measured at 26-35% of an in-race frame). One SPU thread
 * polls the job ring in main memory, DMAs in the stream (and, for indexed
 * attributes, a bounded window of the array), decodes into LS, and DMAs
 * RenderVertex records back out to the RSX-mapped arena.
 *
 * Every PPU<->SPU field lives on its own 128-byte line (see
 * spu_vtx_shared.h): whole-line DMAs are MFC-legal at any size and the two
 * processors never contend for a line.
 *
 * LS budget (256KB total): 64K stream + 64K window + 44K output + ~10K code
 * and statics, leaving ~70K of stack. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <stdint.h>
#include "src/video/rsx/spu_vtx_shared.h"

#define TAG_IN   0
#define TAG_OUT  1
#define TAG_DONE 2

static uint8_t  line_in[SPU_LINE]   __attribute__((aligned(128)));
static uint8_t  line_out[SPU_LINE]  __attribute__((aligned(128)));
static SpuVtxJobSlot jobslot        __attribute__((aligned(128)));
static uint8_t  stream_buf[64*1024] __attribute__((aligned(128)));
static uint8_t  window_buf[SPU_VTX_MAX_WINDOW] __attribute__((aligned(128)));
static float    out_buf[SPU_VTX_MAX_VERTS * SPU_RV_FLOATS] __attribute__((aligned(128)));
static uint32_t skin_buf[SPU_SKIN_U32S] __attribute__((aligned(128)));

static uint64_t g_ring;

/* ---------------- self-measurement ----------------------------------
 *
 * The PPU could see only that it waited; it could not see what for. The SPU
 * decrementer counts down at the 79.8 MHz timebase (12.5 ns a tick), which is
 * ample for per-job accounting and costs a single channel read. Deltas are
 * taken modulo 2^32 because it counts DOWN and wraps. */
static uint32_t st_jobs;    /* jobs in THIS window (reset each publish) */
static uint32_t st_poll;    /* ticks spent looking for work             */
static uint32_t st_work;    /* ticks inside the job path                */
static uint32_t st_dma;     /* ticks blocked in dma_wait, ALL contexts  */
static uint32_t st_workdma; /* ticks blocked in dma_wait inside a job   */
static uint32_t st_total;   /* jobs since boot, for a sanity check      */
static uint32_t st_gather;  /* elements fetched one DMA at a time       */
static uint32_t st_gjobs;   /* jobs with any attribute in gather mode   */
static uint8_t  stat_out[SPU_LINE] __attribute__((aligned(128)));

static inline uint32_t dec_now(void) { return spu_read_decrementer(); }
static inline uint32_t dec_since(uint32_t t0) { return (t0 - dec_now()) & 0xFFFFFFFFu; }

static void dma_get(void *ls, uint64_t ea, uint32_t size, int tag)
{
    uint8_t *p = (uint8_t *)ls;
    while (size) {
        uint32_t n = size > 16384u ? 16384u : size;
        mfc_get(p, ea, n, tag, 0, 0);
        p += n; ea += n; size -= n;
    }
}
static void dma_put(void *ls, uint64_t ea, uint32_t size, int tag)
{
    uint8_t *p = (uint8_t *)ls;
    while (size) {
        uint32_t n = size > 16384u ? 16384u : size;
        mfc_put(p, ea, n, tag, 0, 0);
        p += n; ea += n; size -= n;
    }
}
static void dma_wait(int tag)
{
    uint32_t t0 = dec_now();
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
    st_dma += dec_since(t0);
}

/* Fetch [ea, ea+size) into a 128-byte-aligned LS buffer, returning the offset
 * within that buffer where the requested bytes begin.
 *
 * The MFC accepts sizes of 1/2/4/8 or a MULTIPLE OF 16 only, and wants the EA
 * and LS address to share alignment (SPU MFC manual 3.1: "transfer sizes of
 * 1, 2, 4, 8, and multiples of 16-bytes ... peak performance ... both the EA
 * and LA are 128-byte aligned"). Guest vertex streams sit at arbitrary
 * addresses with arbitrary lengths, so issuing them raw is an ILLEGAL
 * transfer that silently wedges the MFC queue -- which is precisely how the
 * first working SPU build still failed to retire a single job. Rounding the
 * EA down and the length up to whole 128-byte lines makes every transfer
 * both legal and the hardware's fast case. */
static uint32_t dma_get_window(void *ls_base, uint64_t ea, uint32_t size,
                               int tag)
{
    uint32_t skew  = (uint32_t)(ea & 127u);
    uint32_t total = (skew + size + 127u) & ~127u;
    dma_get(ls_base, ea - skew, total, tag);
    return skew;
}
/* Publish one u32 by writing its whole (SPU-owned) line. */
static void publish(uint32_t off, uint32_t val)
{
    *(uint32_t *)line_out = val;
    dma_put(line_out, g_ring + off, SPU_LINE, TAG_OUT);
    dma_wait(TAG_OUT);
}
static uint32_t fetch(uint32_t off)
{
    dma_get(line_in, g_ring + off, SPU_LINE, TAG_IN);
    dma_wait(TAG_IN);
    return *(uint32_t *)line_in;
}

/* The PPU-written half of the ring header in ONE transfer.
 *
 * HEAD and QUIT sit on separate 128-byte lines (deliberately -- they must not
 * share a line with anything the SPU writes), so reading them with two
 * `fetch` calls cost two full round trips to main memory on EVERY pass of the
 * idle loop, just to ask whether there was work. One 384-byte get covers
 * HEAD, DONE and QUIT together for the price of one. */
static uint8_t hdr_in[384] __attribute__((aligned(128)));
static void fetch_hdr(void)
{
    dma_get(hdr_in, g_ring, 384u, TAG_IN);
    dma_wait(TAG_IN);
}

/* This consumer's identity. Jobs are partitioned statically: this SPU takes
 * every ring index congruent to g_id modulo g_nspu, so several SPUs share the
 * ring without an atomic or a lock between them. */
static uint32_t g_id, g_nspu = 1;
#define HDR_U32(off) (*(uint32_t *)(hdr_in + (off)))

/* DONE without waiting for it to land.
 *
 * run_job has already waited for the vertex data put, so the data is in
 * memory before this is even issued and the PPU cannot observe DONE early.
 * Waiting here added a full round trip to the critical path of every job --
 * and that path is exactly what the PPU is spinning on. */
static uint8_t done_out[SPU_LINE] __attribute__((aligned(128)));
static void publish_done(uint32_t val)
{
    mfc_write_tag_mask(1u << TAG_DONE);
    mfc_read_tag_status_all();          /* the PREVIOUS one; long since done */
    *(uint32_t *)done_out = val;
    mfc_put(done_out, g_ring + SPU_OFF_DONE + g_id * SPU_LINE,
            SPU_LINE, TAG_DONE, 0, 0);
}

static void publish_stats(void)
{
    uint32_t *w = (uint32_t *)stat_out;
    w[0] = 0xC0DE0001u;                 /* heartbeat, unchanged */
    w[1] = st_jobs; w[2] = st_poll; w[3] = st_work; w[4] = st_workdma;
    w[5] = st_total;
    w[6] = st_gather; w[7] = st_gjobs;
    dma_put(stat_out, g_ring + SPU_OFF_HB, SPU_LINE, TAG_OUT);
    dma_wait(TAG_OUT);
    /* Per-window, not cumulative: a running total divided by a running job
     * count is dominated by however long the SPU sat idle in a menu, which
     * says nothing about what a job costs. */
    st_jobs = st_poll = st_work = st_workdma = 0;
    st_gather = st_gjobs = 0;
}

/* Wide reads without byte-by-byte assembly.
 *
 * The SPU has no byte load: `p[0]` is a quadword load, a rotate and a mask, so
 * building a 32-bit field out of four of them runs to twenty-odd instructions
 * -- for a value whose bytes were already adjacent and already in the right
 * order, guest and SPE both being big-endian. Measured cost of the old form:
 * ~2.0 us of pure compute per six-vertex job, which the PPU then sat and span
 * on. The packed struct states the unaligned access and lets the compiler
 * pick its own sequence. */
struct un16 { uint16_t v; } __attribute__((packed));
struct un32 { uint32_t v; } __attribute__((packed));
static inline uint16_t rd16(const uint8_t *p)
{ return ((const struct un16 *)p)->v; }
static inline uint32_t rd32(const uint8_t *p)
{ return ((const struct un32 *)p)->v; }
static inline float rdf32(const uint8_t *p)
{ union { uint32_t u; float f; } v; v.u = rd32(p); return v.f; }

/* Every scale is an exact power of two, so the table is bit-identical to the
 * division it replaces -- and the division ran once per COMPONENT per vertex
 * on a processor with no divide instruction. */
static float frac_scale(unsigned frac)
{
    static const float k[32] = {
        1.0f/(1u<<0),  1.0f/(1u<<1),  1.0f/(1u<<2),  1.0f/(1u<<3),
        1.0f/(1u<<4),  1.0f/(1u<<5),  1.0f/(1u<<6),  1.0f/(1u<<7),
        1.0f/(1u<<8),  1.0f/(1u<<9),  1.0f/(1u<<10), 1.0f/(1u<<11),
        1.0f/(1u<<12), 1.0f/(1u<<13), 1.0f/(1u<<14), 1.0f/(1u<<15),
        1.0f/(1u<<16), 1.0f/(1u<<17), 1.0f/(1u<<18), 1.0f/(1u<<19),
        1.0f/(1u<<20), 1.0f/(1u<<21), 1.0f/(1u<<22), 1.0f/(1u<<23),
        1.0f/(1u<<24), 1.0f/(1u<<25), 1.0f/(1u<<26), 1.0f/(1u<<27),
        1.0f/(1u<<28), 1.0f/(1u<<29), 1.0f/(1u<<30), 1.0f/(1u<<31)
    };
    return k[frac & 31u];
}

static float dequant(const uint8_t *p, unsigned fmt, unsigned frac, unsigned i)
{
    float scale = frac_scale(frac);
    switch (fmt) {
    case SVF_U8:  return (float)p[i] * scale;
    case SVF_S8:  return (float)(int8_t)p[i] * scale;
    case SVF_U16: return (float)rd16(p + i*2) * scale;
    case SVF_S16: return (float)(int16_t)rd16(p + i*2) * scale;
    default:      return rdf32(p + i*4);
    }
}
static unsigned fmt_bytes(unsigned fmt)
{ return fmt <= SVF_S8 ? 1u : fmt <= SVF_S16 ? 2u : 4u; }

static void col_decode(const uint8_t *p, unsigned fmt, float *rgba)
{
    switch (fmt) {
    case SVC_RGB565: { uint16_t v = rd16(p);
        rgba[0]=(float)((v>>11)&31)/31.0f; rgba[1]=(float)((v>>5)&63)/63.0f;
        rgba[2]=(float)(v&31)/31.0f; rgba[3]=1.0f; break; }
    case SVC_RGB8: case SVC_RGBX8:
        rgba[0]=p[0]/255.0f; rgba[1]=p[1]/255.0f; rgba[2]=p[2]/255.0f;
        rgba[3]=1.0f; break;
    case SVC_RGBA4: { uint16_t v = rd16(p);
        rgba[0]=(float)((v>>12)&15)/15.0f; rgba[1]=(float)((v>>8)&15)/15.0f;
        rgba[2]=(float)((v>>4)&15)/15.0f;  rgba[3]=(float)(v&15)/15.0f; break; }
    case SVC_RGBA6: { uint32_t v=((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
        rgba[0]=(float)((v>>18)&63)/63.0f; rgba[1]=(float)((v>>12)&63)/63.0f;
        rgba[2]=(float)((v>>6)&63)/63.0f;  rgba[3]=(float)(v&63)/63.0f; break; }
    default:
        rgba[0]=p[0]/255.0f; rgba[1]=p[1]/255.0f;
        rgba[2]=p[2]/255.0f; rgba[3]=p[3]/255.0f; break;
    }
}
static unsigned col_bytes(unsigned fmt)
{
    switch (fmt) {
    case SVC_RGB565: case SVC_RGBA4: return 2u;
    case SVC_RGB8:   case SVC_RGBA6: return 3u;
    default:                         return 4u;
    }
}

static uint32_t window_off[SPU_VTX_MAX_ATTRS];

/* Same bone multiply the PPU decode loop does, so a multi-bone strip matches
 * that path byte for byte in intent: position is the 3x4 at xf.mem[idx*4],
 * normal is the 3x3 at xf.mem[0x400 + 3*(idx&31)]. Guest floats are already
 * host-endian in the snapshot. */
static float skin_f(unsigned i)
{
    union { uint32_t u; float f; } c;
    c.u = skin_buf[i];
    return c.f;
}
static void skin_vertex(float *ov, unsigned idx, int have_nrm)
{
    unsigned b = (idx & 0x3Fu) * 4u;
    float px = ov[0], py = ov[1], pz = ov[2];
    float m0 = skin_f(b+0),  m1 = skin_f(b+1),  m2 = skin_f(b+2),  m3 = skin_f(b+3);
    float m4 = skin_f(b+4),  m5 = skin_f(b+5),  m6 = skin_f(b+6),  m7 = skin_f(b+7);
    float m8 = skin_f(b+8),  m9 = skin_f(b+9),  m10= skin_f(b+10), m11= skin_f(b+11);
    ov[0] = m0*px + m1*py + m2*pz + m3;
    ov[1] = m4*px + m5*py + m6*pz + m7;
    ov[2] = m8*px + m9*py + m10*pz + m11;
    if (have_nrm) {
        unsigned n = 0x400u + 3u * (idx & 31u);
        float nx = ov[3], ny = ov[4], nz = ov[5];
        float n0 = skin_f(n+0), n1 = skin_f(n+1), n2 = skin_f(n+2);
        float n3 = skin_f(n+3), n4 = skin_f(n+4), n5 = skin_f(n+5);
        float n6 = skin_f(n+6), n7 = skin_f(n+7), n8 = skin_f(n+8);
        ov[3] = n0*nx + n1*ny + n2*nz;
        ov[4] = n3*nx + n4*ny + n5*nz;
        ov[5] = n6*nx + n7*ny + n8*nz;
    }
}

/* Byte offset of attribute `a`'s field within a vertex. The recipe lists the
 * attributes in stream order, so this is the running size of everything before
 * it -- the same walk the decode loop does, hoisted so the scan can address
 * each index directly. */
static uint32_t attr_pos(const SpuVtxJob *j, unsigned want)
{
    uint32_t off = 0;
    unsigned a;
    for (a = 0; a < j->nattrs && a < want; a++) {
        const SpuVtxAttr *at = &j->attr[a];
        if (at->kind == SVA_PNMTXIDX || at->kind == SVA_TEXMTXIDX) { off += 1; continue; }
        if (at->mode != SVM_DIRECT) { off += (at->mode == SVM_IDX8) ? 1u : 2u; continue; }
        /* Exactly the advances the decode loop below makes: colours are the
         * only kind measured with col_bytes; position, normal and every
         * texture coordinate use count x fmt_bytes. Getting this wrong reads
         * the indices from the wrong byte and silently corrupts geometry. */
        switch (at->kind) {
        case SVA_COL0: case SVA_COL1:
            off += col_bytes(at->fmt); break;
        default:
            off += (uint32_t)at->count * fmt_bytes(at->fmt); break;
        }
    }
    return off;
}

static uint16_t local_base[SPU_VTX_MAX_ATTRS];
static uint8_t  gather[SPU_VTX_MAX_ATTRS];
/* Staging for the one-element fallback above; 128-aligned for the MFC. */
static uint8_t  elem_buf[256] __attribute__((aligned(128)));

/* A job's primitives in stream order: one headerless primitive for a plain
 * job, `prims` commands for a run. NOP padding between commands is skipped. */
typedef struct {
    const uint8_t *vp;
    uint32_t n;
    uint8_t  op;
} PrimSpan;

typedef struct {
    const SpuVtxJob *j;
    const uint8_t   *sb;
    uint32_t         pos;
    unsigned         left;
} PrimWalk;

static void prim_walk_init(PrimWalk *w, const SpuVtxJob *j, const uint8_t *sb)
{
    w->j = j; w->sb = sb; w->pos = 0;
    w->left = (j->flags & SPU_JOB_RUN) ? j->prims : 1u;
}

static int prim_next(PrimWalk *w, PrimSpan *ps)
{
    const SpuVtxJob *j = w->j;
    if (!w->left)
        return 0;
    w->left--;
    if (!(j->flags & SPU_JOB_RUN)) {
        ps->vp = w->sb; ps->n = j->vert_count; ps->op = 0;
        return 1;
    }
    while (w->pos < j->stream_bytes && w->sb[w->pos] == 0)
        w->pos++;
    if (w->pos + 3u > j->stream_bytes)
        return 0;
    ps->op = w->sb[w->pos];
    ps->n  = rd16(w->sb + w->pos + 1);
    ps->vp = w->sb + w->pos + 3;
    w->pos += 3u + ps->n * j->vert_size;
    if (w->pos > j->stream_bytes)
        return 0;
    return 1;
}

/* The triangle list a primitive becomes, numbered from `first`. Must match
 * batch_indices (gx_render.c) and batch_index_count exactly: the PPU sizes
 * the draw from the latter before this runs. */
static uint16_t idx_buf[SPU_RUN_MAX_IDX] __attribute__((aligned(128)));

static uint32_t emit_indices(uint8_t op, uint32_t first, uint32_t n, uint32_t at)
{
    uint16_t *o = idx_buf + at;
    uint32_t k, need;
    switch (op & 0xF8u) {
    case 0x90: need = n / 3u * 3u; break;
    case 0x98: case 0xA0: need = n >= 3u ? (n - 2u) * 3u : 0u; break;
    case 0x80: case 0x88: need = n / 4u * 6u; break;
    default: return at;
    }
    if (at + need > SPU_RUN_MAX_IDX)
        return at;
    switch (op & 0xF8u) {
    case 0x90:
        for (k = 0; k + 2u < n; k += 3u, o += 3) {
            o[0] = (uint16_t)(first + k); o[1] = (uint16_t)(first + k + 1u);
            o[2] = (uint16_t)(first + k + 2u);
        }
        break;
    case 0x98:
        for (k = 0; k + 2u < n; k++, o += 3) {
            o[0] = (uint16_t)(first + k + (k & 1u));
            o[1] = (uint16_t)(first + k + 1u - (k & 1u));
            o[2] = (uint16_t)(first + k + 2u);
        }
        break;
    case 0xA0:
        for (k = 1; k + 1u < n; k++, o += 3) {
            o[0] = (uint16_t)first; o[1] = (uint16_t)(first + k);
            o[2] = (uint16_t)(first + k + 1u);
        }
        break;
    default:
        for (k = 0; k + 3u < n; k += 4u, o += 6) {
            o[0] = (uint16_t)(first + k);      o[1] = (uint16_t)(first + k + 1u);
            o[2] = (uint16_t)(first + k + 2u); o[3] = (uint16_t)(first + k);
            o[4] = (uint16_t)(first + k + 2u); o[5] = (uint16_t)(first + k + 3u);
        }
        break;
    }
    return at + need;
}

static void decode_vertex(const SpuVtxJob *j, const uint8_t *vp, float *ov, int skinning)
{
    const uint8_t *sp = vp;
    unsigned a, t;
    unsigned pidx = 0;
    int have_idx = 0, have_nrm = 0;
    ov[0]=ov[1]=ov[2]=0.0f;
    ov[3]=0.0f; ov[4]=0.0f; ov[5]=1.0f;
    ov[6]=ov[7]=ov[8]=ov[9]=1.0f;
    for (t=0;t<4;t++){ ov[10+t*3]=0.0f; ov[11+t*3]=0.0f; ov[12+t*3]=1.0f; }

    for (a = 0; a < j->nattrs; a++) {
        const SpuVtxAttr *at = &j->attr[a];
        const uint8_t *src;
        if (at->kind == SVA_END) break;
        if (at->kind == SVA_PNMTXIDX) {
            if (skinning) { pidx = *sp & 0x3Fu; have_idx = 1; }
            sp += 1; continue;
        }
        if (at->kind == SVA_TEXMTXIDX) {
            sp += 1; continue;
        }
        if (at->mode == SVM_DIRECT) {
            src = sp;
        } else {
            uint32_t idx = (at->mode == SVM_IDX8) ? *sp : rd16(sp);
            sp += (at->mode == SVM_IDX8) ? 1u : 2u;
            idx -= local_base[a];
            if (gather[a]) {
                /* Window did not fit: fetch this one element. Slow,
                 * and correct, and it happens on the consumer that
                 * has time to spare rather than on the PPE. */
                dma_get(elem_buf, at->array_ea +
                        ((uint32_t)local_base[a] + idx) * at->stride,
                        (at->stride + 15u) & ~15u, TAG_IN);
                dma_wait(TAG_IN);
                src = elem_buf;
                st_gather++;
            } else {
                src = window_buf + window_off[a] + idx * at->stride;
            }
        }
        switch (at->kind) {
        case SVA_POS: {
            unsigned i, n = at->count;
            for (i = 0; i < n && i < 3u; i++)
                ov[i] = dequant(src, at->fmt, at->frac, i);
            if (at->mode == SVM_DIRECT) sp += n * fmt_bytes(at->fmt);
            break; }
        case SVA_NRM: {
            unsigned i, n = at->count;
            have_nrm = 1;
            for (i = 0; i < 3u && i < n; i++)
                ov[3+i] = dequant(src, at->fmt, at->frac, i);
            if (at->mode == SVM_DIRECT) sp += n * fmt_bytes(at->fmt);
            break; }
        case SVA_COL0:
            col_decode(src, at->fmt, &ov[6]);
            if (at->mode == SVM_DIRECT) sp += col_bytes(at->fmt);
            break;
        case SVA_COL1:
            if (at->mode == SVM_DIRECT) sp += col_bytes(at->fmt);
            break;
        default: {
            unsigned i, n = at->count;
            unsigned tn = at->kind - SVA_TEX0;
            if (tn < 4u)
                for (i = 0; i < n && i < 2u; i++)
                    ov[10 + tn*3 + i] = dequant(src, at->fmt, at->frac, i);
            if (at->mode == SVM_DIRECT) sp += n * fmt_bytes(at->fmt);
            break; }
        }
    }
    if (have_idx) skin_vertex(ov, pidx, have_nrm);
}

static void run_job(const SpuVtxJob *j)
{
    const int run = (j->flags & SPU_JOB_RUN) != 0;
    uint32_t win_used = 0, stream_skew, stream_len;
    unsigned a;
    PrimWalk w;
    PrimSpan ps;

    stream_len = run ? j->stream_bytes : (uint32_t)j->vert_count * j->vert_size;
    if (stream_len > sizeof stream_buf - 128u)
        stream_len = sizeof stream_buf - 128u;
    stream_skew = dma_get_window(stream_buf, j->stream_ea, stream_len, TAG_IN);
    if (j->skin_ea)
        dma_get(skin_buf, j->skin_ea, SPU_SKIN_BYTES, TAG_IN);

    if (j->flags & SPU_JOB_SCAN) {
        /* The PPU handed over the index scan. The indices live in the vertex
         * stream, so that has to land first -- one extra round trip here, and
         * none at all on the PPE, which is the side that is short of time. */
        dma_wait(TAG_IN);
        for (a = 0; a < j->nattrs; a++) {
            const SpuVtxAttr *at = &j->attr[a];
            uint32_t mn = 0xFFFFFFFFu, mx = 0, span, need, off, ao, v2;
            window_off[a] = 0; local_base[a] = 0; gather[a] = 0;
            if (at->mode == SVM_DIRECT || at->kind == SVA_END) continue;

            ao = attr_pos(j, a);
            prim_walk_init(&w, j, stream_buf + stream_skew);
            while (prim_next(&w, &ps)) {
                const uint8_t *sp2 = ps.vp + ao;
                if (ps.n > j->vert_count)
                    ps.n = j->vert_count;
                for (v2 = 0; v2 < ps.n; v2++, sp2 += j->vert_size) {
                    uint32_t idx = (at->mode == SVM_IDX8) ? *sp2 : rd16(sp2);
                    if (idx < mn) mn = idx;
                    if (idx > mx) mx = idx;
                }
            }
            if (mn > mx) continue;              /* no vertices at all */
            local_base[a] = (uint16_t)mn;
            span = (mx - mn + 1u) * at->stride;
            off  = mn * at->stride;
            /* Only fetch what the PPU vouched for, and only if it fits. */
            if (off + span > j->window_len[a]) { gather[a] = 1; st_gjobs++; continue; }
            need = ((uint32_t)((at->array_ea + off) & 127u) + span + 127u)
                   & ~127u;
            if (win_used + need > SPU_VTX_MAX_WINDOW) { gather[a] = 1; st_gjobs++; continue; }
            window_off[a] = win_used +
                dma_get_window(window_buf + win_used, at->array_ea + off,
                               span, TAG_IN);
            win_used += need;
        }
        dma_wait(TAG_IN);
    } else {
        for (a = 0; a < j->nattrs; a++) {
            const SpuVtxAttr *at = &j->attr[a];
            uint32_t need;
            window_off[a] = 0; local_base[a] = at->base_idx; gather[a] = 0;
            if (at->mode == SVM_DIRECT || at->kind == SVA_END) continue;
            need = ((uint32_t)(at->array_ea & 127u) + j->window_len[a] + 127u)
                   & ~127u;
            if (win_used + need > SPU_VTX_MAX_WINDOW) continue;
            window_off[a] = win_used +
                dma_get_window(window_buf + win_used, at->array_ea,
                               j->window_len[a], TAG_IN);
            win_used += need;
        }
        dma_wait(TAG_IN);
    }

    {
        /* Output goes out in chunks of SPU_VTX_MAX_VERTS: 512 vertices are
         * 45,056 bytes, a multiple of 16, so every chunk after the first
         * starts DMA-aligned. dest_ea is 16-aligned and its reservation is a
         * whole number of vertex PAIRS (spu_vtx_reserve: 176 bytes each), so
         * rounding the last length up to 16 bytes stays inside it. */
        const int skinning = j->skin_ea != 0;
        uint32_t done = 0, chunk = 0, nidx = 0, v;
        float *ov = out_buf;
        prim_walk_init(&w, j, stream_buf + stream_skew);
        while (prim_next(&w, &ps)) {
            /* Never more vertices than the PPU reserved, whatever the stream
             * says by the time it is read: the indices stay inside the draw
             * and the output inside its reservation. */
            if (ps.n > (uint32_t)j->vert_count - done)
                ps.n = (uint32_t)j->vert_count - done;
            if (run)
                nidx = emit_indices(ps.op, done, ps.n, nidx);
            for (v = 0; v < ps.n; v++) {
                decode_vertex(j, ps.vp + v * j->vert_size, ov, skinning);
                ov += SPU_RV_FLOATS;
                done++;
                if (++chunk == SPU_VTX_MAX_VERTS) {
                    dma_put(out_buf, j->dest_ea + (uint64_t)(done - chunk) * SPU_RV_BYTES,
                            chunk * SPU_RV_BYTES, TAG_OUT);
                    dma_wait(TAG_OUT);
                    chunk = 0;
                    ov = out_buf;
                }
            }
        }
        if (chunk)
            dma_put(out_buf, j->dest_ea + (uint64_t)(done - chunk) * SPU_RV_BYTES,
                    (chunk * SPU_RV_BYTES + 15u) & ~15u, TAG_OUT);
        if (run) {
            /* A short list is padded with degenerate triangles up to what
             * the PPU already told the RSX to draw. */
            if (nidx > j->nidx)
                nidx = j->nidx;             /* the reservation's size */
            while (nidx < j->nidx && nidx < SPU_RUN_MAX_IDX)
                idx_buf[nidx++] = 0;
            if (nidx)
                dma_put(idx_buf, j->idx_ea, (nidx * 2u + 15u) & ~15u, TAG_OUT);
        }
        dma_wait(TAG_OUT);
    }
}

int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t next;
    (void)arg3; (void)arg4;
    g_ring = arg1;                     /* sysSpuThreadArgument.arg0 */
    g_id   = (uint32_t)arg2;           /* consumer index, 0..nspu-1        */

    spu_write_decrementer(0xFFFFFFFFu); /* free-running, for the accounting */

    /* The consumer count is written by the PPU before the group starts, so a
     * single image serves any number of SPUs and the count stays a runtime
     * choice. Clamp: a zero here would make the stride zero and spin forever
     * on one job. */
    g_nspu = fetch(SPU_OFF_NSPU);
    if (g_nspu == 0 || g_nspu > SPU_VTX_MAX_SPU) g_nspu = 1;
    if (g_id >= g_nspu) g_id = 0;
    next = g_id;                       /* first job that belongs to me */

    /* Publish my starting position BEFORE the heartbeat.
     *
     * DONE means "the lowest job index I have not finished", so at startup it
     * is my id. Without this it stayed at the memset zero, and a consumer that
     * had not yet been handed any work pinned the PPU's minimum at 0 -- so
     * spu_vtx_join waited for a job that did not exist and burned the whole
     * spin limit (measured: one join, 20,000,001 iterations). Publishing here,
     * before the heartbeat the PPU waits on, means the PPU never observes an
     * unpublished DONE. */
    publish(SPU_OFF_DONE + g_id * SPU_LINE, next);
    publish(SPU_OFF_HB   + g_id * SPU_LINE, 0xC0DE0001u);

    for (;;) {
        uint32_t t_poll = dec_now();
        fetch_hdr();
        if (HDR_U32(SPU_OFF_QUIT)) break;
        /* Unsigned compare, so a HEAD that has not yet reached my next slot
         * means idle. HEAD only grows. */
        if ((int32_t)(HDR_U32(SPU_OFF_HEAD) - next) <= 0) {
            st_poll += dec_since(t_poll); continue;
        }
        st_poll += dec_since(t_poll);

        {
            uint32_t t_work = dec_now();
            uint32_t d0 = st_dma;
            dma_get(&jobslot, g_ring + SPU_OFF_SLOTS +
                    (uint64_t)(next & (SPU_VTX_RING - 1u)) * sizeof(SpuVtxJobSlot),
                    sizeof(SpuVtxJobSlot), TAG_IN);
            dma_wait(TAG_IN);
            run_job(&jobslot.job);
            next += g_nspu;            /* skip the slots my peers own */
            publish_done(next);
            st_work += dec_since(t_work);
            st_workdma += st_dma - d0;
        }
        /* Every 256 jobs, not every job: the stats put is itself a round trip
         * and must not become part of what it is measuring. */
        st_jobs++; st_total++;
        if (st_jobs >= 256u) publish_stats();
    }
    publish(SPU_OFF_HB + g_id * SPU_LINE, 0xC0DEDEADu);
    spu_thread_exit(0);
    return 0;
}
