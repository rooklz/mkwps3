/* gxf_spu.c -- the GX command stream parsed on an SPU.
 *
 * The game thread writes GX commands into a ring in main memory and publishes how far
 * it has got. This program follows that position, parses each command exactly as the
 * PPU parser (gx_hw.cpp) does, display lists included, and writes the GPU thread a
 * record per command that changes something (gxf_shared.h). It keeps the CP, BP and
 * XF registers as the renderer will have them, so a write that leaves its register as
 * it was goes no further; a register it hasn't seen written yet always goes through.
 * Indexed XF loads read their matrices from guest memory here and go out as XF writes.
 *
 * Everything comes in by DMA through small windows, and the records go out the same
 * way, so the parse costs the PPE nothing and pulls nothing through its cache. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>
#include <string.h>
#include "gxf_shared.h"

#define TAG_CTL  0
#define TAG_RING 1
#define TAG_LIST 2
#define TAG_IDX  3
#define TAG_OUT  4

#define FOLD_MASK  0x3FFFFFFFu
#define MEM1_BYTES 0x01800000u
#define MEM2_BASE  0x10000000u
#define MEM2_BYTES 0x04000000u

static GxfControl ctl __attribute__((aligned(128)));
static uint64_t ctl_ea;

/* ---- registers ------------------------------------------------------------------ */

#define XF_WORDS 0x1058u

static uint32_t cp_vcd_lo, cp_vcd_hi, cp_vat[3][8];
static uint32_t cp_base[16], cp_stride[16];
static uint32_t vsize[8];
static uint32_t vsize_valid;

static uint32_t cp_raw[256];
static uint32_t cp_known[256 / 32];
static uint32_t bp_raw[256];
static uint32_t bp_known[256 / 32];
static uint32_t xf_mem[XF_WORDS] __attribute__((aligned(16)));
static uint32_t xf_known[(XF_WORDS + 31) / 32];

static inline int known(const uint32_t *bits, uint32_t i) { return (bits[i >> 5] >> (31 - (i & 31))) & 1; }
static inline void set_known(uint32_t *bits, uint32_t i) { bits[i >> 5] |= 0x80000000u >> (i & 31); }
static inline void clear_known(uint32_t *bits, uint32_t i) { bits[i >> 5] &= ~(0x80000000u >> (i & 31)); }

static uint32_t n_dropped, n_kept, n_unknown, n_lists, n_idx, n_idx_miss;

static inline uint32_t bits(uint32_t v, unsigned lsb, unsigned width) { return (v >> lsb) & ((1u << width) - 1u); }

static uint32_t component_size(uint32_t f) { return f <= 1 ? 1u : f <= 3 ? 2u : 4u; }
static uint32_t color_size(uint32_t f) { static const uint8_t k[8] = {2, 3, 4, 2, 3, 4, 0, 0}; return k[f & 7]; }
static uint32_t attr_size(uint32_t type, uint32_t direct) { return type == 1 ? direct : type == 2 ? 1u : type == 3 ? 2u : 0u; }

/* ComputeVertexSize, field by field. */
static uint32_t compute_vertex_size(unsigned vat)
{
    const uint32_t lo = cp_vcd_lo, hi = cp_vcd_hi;
    const uint32_t a = cp_vat[0][vat], b = cp_vat[1][vat], c = cp_vat[2][vat];
    static const uint8_t elem_lsb[8] = {21, 0, 9, 18, 27, 5, 14, 23};
    static const uint8_t fmt_lsb[8] = {22, 1, 10, 19, 28, 6, 15, 24};
    uint32_t size = 0, n;
    for (n = 0; n < 9; ++n)
        size += bits(lo, n, 1);
    size += attr_size(bits(lo, 9, 2), (bits(a, 0, 1) ? 3u : 2u) * component_size(bits(a, 1, 3)));
    {
        const uint32_t type = bits(lo, 11, 2);
        const uint32_t vectors = bits(a, 9, 1) ? 3u : 1u;
        uint32_t nrm = attr_size(type, 3u * vectors * component_size(bits(a, 10, 3)));
        if (bits(a, 31, 1) && vectors == 3u && type >= 2)
            nrm *= 3u;
        size += nrm;
    }
    size += attr_size(bits(lo, 13, 2), color_size(bits(a, 14, 3)));
    size += attr_size(bits(lo, 15, 2), color_size(bits(a, 18, 3)));
    for (n = 0; n < 8; ++n) {
        const uint32_t g = n == 0 ? a : n < 5 ? b : c;
        const uint32_t elems = bits(g, elem_lsb[n], 1) ? 2u : 1u;
        size += attr_size(bits(hi, 2 * n, 2), elems * component_size(bits(g, fmt_lsb[n], 3)));
    }
    return size;
}

static inline uint32_t vertex_size(unsigned vat)
{
    if (!(vsize_valid & (1u << vat))) {
        vsize[vat] = compute_vertex_size(vat);
        vsize_valid |= 1u << vat;
    }
    return vsize[vat];
}

/* ---- records out --------------------------------------------------------------- */

#define OUT_STAGE 4096u
static uint32_t out_buf[2][OUT_STAGE / 4] __attribute__((aligned(128)));
static uint32_t out_cur;          /* which stage buffer is filling */
static uint32_t out_n;            /* words in it */
static uint32_t out_head;         /* bytes written to the ring, wrapping at 2^32 */
static uint32_t out_tail_seen;
static uint32_t ticks_stall;

static void wait_tag(int tag)
{
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

/* Re-reads the PPE line (the game and GPU threads write it). */
static void ppe_line(void)
{
    mfc_get((volatile void *)&ctl.written, ctl.ppe_ea, 128, TAG_CTL, 0, 0);
    wait_tag(TAG_CTL);
}

static uint32_t ppe_tail(void)
{
    ppe_line();
    return ctl.out_tail;
}

static void out_publish(void)
{
    ctl.out_head = out_head;
    mfc_putf((volatile void *)&ctl.seen, ctl.spu_ea, 128, TAG_OUT, 0, 0);
}

/* Room for `bytes` at the head without passing the GPU thread's tail. */
static void out_room(uint32_t bytes)
{
    if (out_head - out_tail_seen + bytes <= ctl.out_bytes)
        return;
    {
        const uint32_t t0 = spu_read_decrementer();
        for (;;) {
            out_tail_seen = ppe_tail();
            if (out_head - out_tail_seen + bytes <= ctl.out_bytes || ctl.quit)
                break;
            {   /* about a microsecond between polls */
                const uint32_t d = spu_read_decrementer();
                while (d - spu_read_decrementer() < 80) {}
            }
        }
        ticks_stall += t0 - spu_read_decrementer();
    }
}

/* Sends the filling stage buffer to the ring, padded to 16 bytes. */
static void out_flush(void)
{
    uint32_t *buf = out_buf[out_cur];
    uint32_t bytes, off;
    if (!out_n)
        return;
    while (out_n & 3)
        buf[out_n++] = GXF_HDR(GXF_NOP, 0);
    bytes = out_n * 4u;
    off = out_head % ctl.out_bytes;
    if (off + bytes > ctl.out_bytes) {
        /* doesn't fit before the end: a wrap marker there, and the buffer at the start */
        static uint32_t wrap[4] __attribute__((aligned(16)));
        const uint32_t skip = ctl.out_bytes - off;
        out_room(skip + bytes);
        wrap[0] = GXF_HDR(GXF_WRAP, 0);
        mfc_put(wrap, ctl.out_ea + off, 16, TAG_OUT, 0, 0);
        wait_tag(TAG_OUT);
        out_head += skip;
        off = 0;
    } else {
        out_room(bytes);
    }
    mfc_put(buf, ctl.out_ea + off, bytes, TAG_OUT, 0, 0);
    out_head += bytes;
    out_cur ^= 1;
    out_n = 0;
    wait_tag(TAG_OUT);   /* the buffer we switch to was sent two flushes ago; this one must land before the head moves */
    out_publish();
}

static inline void out_need(uint32_t words)
{
    if (out_n + words > OUT_STAGE / 4 - 4)
        out_flush();
}

static inline void out_word(uint32_t w) { out_buf[out_cur][out_n++] = w; }

/* ---- input windows ------------------------------------------------------------- */

#define WIN_BYTES 16384u

typedef struct {
    uint8_t *buf;
    uint64_t lo, hi;     /* host addresses held */
    int tag;
} Window;

static uint8_t ring_win_buf[WIN_BYTES] __attribute__((aligned(128)));
static uint8_t list_win_buf[WIN_BYTES] __attribute__((aligned(128)));
static Window ring_win = {ring_win_buf, 0, 0, TAG_RING};
static Window list_win = {list_win_buf, 0, 0, TAG_LIST};

/* `n` bytes at host address `ea`, never beyond `limit` (16-aligned). */
static const uint8_t *win_get(Window *w, uint64_t ea, uint32_t n, uint64_t limit)
{
    if (ea < w->lo || ea + n > w->hi) {
        uint64_t lo = ea & ~15ull, len = WIN_BYTES;
        if (lo + len > limit)
            len = limit - lo;
        mfc_get(w->buf, lo, (uint32_t)len, w->tag, 0, 0);
        wait_tag(w->tag);
        w->lo = lo;
        w->hi = lo + len;
    }
    return w->buf + (ea - w->lo);
}

/* A stream of commands: the ring between two publish positions, or one display list. */
typedef struct {
    int ring;
    uint64_t pos, end;   /* ring: absolute positions; list: offsets */
    uint64_t base_ea;    /* list: host address of byte 0 */
    uint32_t guest;      /* list: guest address of byte 0 */
    uint64_t limit;      /* list: host address the reads stop at */
} Stream;

static uint8_t straddle[16] __attribute__((aligned(16)));

static const uint8_t *peek(Stream *s, uint64_t at, uint32_t n)
{
    if (!s->ring)
        return win_get(&list_win, s->base_ea + at, n, s->limit);
    {
        const uint32_t r = ctl.ring_bytes;
        const uint32_t off = (uint32_t)(at % r);
        if (off + n <= r)
            return win_get(&ring_win, ctl.ring_ea + off, n, ctl.ring_ea + r);
        {   /* across the wrap: the head from the end of the ring, the tail from its start */
            const uint32_t head = r - off;
            memcpy(straddle, win_get(&ring_win, ctl.ring_ea + off, head, ctl.ring_ea + r), head);
            memcpy(straddle + head, win_get(&ring_win, ctl.ring_ea, n - head, ctl.ring_ea + r), n - head);
            return straddle;
        }
    }
}

static inline uint32_t rd16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* The guest address of stream byte `at`, as the renderer reads it (sink_draw). */
static uint32_t guest_addr(const Stream *s, uint64_t at)
{
    if (!s->ring)
        return s->guest + (uint32_t)at;
    return ctl.ring_guest + (uint32_t)(at % ctl.ring_bytes);
}

static int range_backed(uint32_t addr, uint32_t len)
{
    const uint32_t f0 = addr & FOLD_MASK, f1 = f0 + len - 1;
    if (len == 0 || f1 < f0)
        return 0;
    if (f1 < MEM1_BYTES)
        return 1;
    return f0 >= MEM2_BASE && f1 < MEM2_BASE + MEM2_BYTES;
}

/* ---- register writes ----------------------------------------------------------- */

static void load_cp(uint32_t reg, uint32_t value)
{
    const unsigned n = reg & 0x0F;
    if (known(cp_known, reg) && cp_raw[reg] == value) {
        ++n_dropped;
        return;
    }
    cp_raw[reg] = value;
    set_known(cp_known, reg);
    switch (reg & 0xF0) {
    case 0x50: cp_vcd_lo = value; vsize_valid = 0; break;
    case 0x60: cp_vcd_hi = value; vsize_valid = 0; break;
    case 0x70: if (n < 8) { cp_vat[0][n] = value; vsize_valid &= ~(1u << n); } break;
    case 0x80: if (n < 8) { cp_vat[1][n] = value; vsize_valid &= ~(1u << n); } break;
    case 0x90: if (n < 8) { cp_vat[2][n] = value; vsize_valid &= ~(1u << n); } break;
    case 0xA0: cp_base[n] = value; break;
    case 0xB0: cp_stride[n] = value; break;
    default: break;
    }
    ++n_kept;
    out_need(2);
    out_word(GXF_HDR(GXF_CP, reg));
    out_word(value);
}

/* The writes gx_state_load_bp acts on whatever the value. */
static inline int bp_acts(uint32_t reg)
{
    return reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52 || (reg >= 0x64 && reg <= 0x68) || reg == 0xFE;
}

/* The mask register (0xFE) applies to the next write only, and not to the TEV colour
 * registers; after any other write it is back to all ones. A value that depends on a
 * register we never saw is unknown, and so is that register afterwards. */
static void load_bp(uint32_t word)
{
    const uint32_t reg = word >> 24;
    if (reg == 0xFE) {
        bp_raw[0xFE] = word & 0x00FFFFFFu;
        set_known(bp_known, 0xFE);
    } else {
        const uint32_t value = word & 0x00FFFFFFu;
        const int mask_known = known(bp_known, 0xFE);
        const uint32_t mask = bp_raw[0xFE];
        int v_known = 1;
        uint32_t v = value;
        if (reg < 0xE0 || reg > 0xE7) {
            if (!mask_known)
                v_known = 0;
            else if (mask != 0x00FFFFFFu) {
                if (known(bp_known, reg))
                    v = (bp_raw[reg] & ~mask) | (value & mask);
                else
                    v_known = 0;
            }
        }
        bp_raw[0xFE] = 0x00FFFFFFu;   /* spent either way */
        set_known(bp_known, 0xFE);
        /* A write that spends a pending mask goes through even when it changes nothing:
         * the GPU thread's mask is only cleared by a write it sees. */
        if (v_known && known(bp_known, reg) && v == bp_raw[reg] && !bp_acts(reg) &&
            mask_known && mask == 0x00FFFFFFu) {
            ++n_dropped;
            return;
        }
        if (v_known) {
            bp_raw[reg] = v;
            set_known(bp_known, reg);
        } else {
            clear_known(bp_known, reg);
        }
    }
    ++n_kept;
    out_need(2);
    out_word(GXF_HDR(GXF_BP, 0));
    out_word(word);
}

static void load_xf(uint32_t addr, const uint32_t *values, uint32_t count)
{
    uint32_t i, same = 1;
    for (i = 0; i < count; ++i) {
        const uint32_t a = addr + i;
        if (a >= XF_WORDS || !known(xf_known, a) || xf_mem[a] != values[i]) {
            same = 0;
            break;
        }
    }
    if (same) {
        ++n_dropped;
        return;
    }
    for (i = 0; i < count; ++i) {
        const uint32_t a = addr + i;
        if (a < XF_WORDS) {
            xf_mem[a] = values[i];
            set_known(xf_known, a);
        }
    }
    ++n_kept;
    out_need(1 + count);
    out_word(GXF_HDR(GXF_XF, (addr & 0xFFFFu) | ((count - 1) << 16)));
    for (i = 0; i < count; ++i)
        out_word(values[i]);
}

/* Matrix arrays: a few lines of guest memory, good for one publish (the game may
 * rewrite them for the next frame once it has sent the commands that use them). */
#define IDX_LINES 4
#define IDX_LINE 2048u
static uint8_t idx_buf[IDX_LINES][IDX_LINE] __attribute__((aligned(128)));
static uint64_t idx_ea[IDX_LINES];
static uint32_t idx_len[IDX_LINES];
static uint32_t idx_valid, idx_next;

/* `n` bytes at `ea`, a line at a time; `limit` is where guest memory ends. */
static const uint8_t *idx_read(uint64_t ea, uint32_t n, uint64_t limit)
{
    uint64_t len = IDX_LINE;
    unsigned i;
    for (i = 0; i < IDX_LINES; ++i)
        if ((idx_valid >> i) & 1 && ea >= idx_ea[i] && ea + n <= idx_ea[i] + idx_len[i])
            return idx_buf[i] + (ea - idx_ea[i]);
    ++n_idx_miss;
    i = idx_next++ % IDX_LINES;
    idx_ea[i] = ea & ~127ull;
    if (idx_ea[i] + len > limit)
        len = limit - idx_ea[i];
    idx_len[i] = (uint32_t)len;
    mfc_get(idx_buf[i], idx_ea[i], (uint32_t)len, TAG_IDX, 0, 0);
    wait_tag(TAG_IDX);
    idx_valid |= 1u << i;
    return idx_buf[i] + (ea - idx_ea[i]);
}

static void load_index(uint32_t op, uint32_t word)
{
    static const uint8_t arrays[4] = {12, 13, 14, 15};
    const uint32_t index = word >> 16, xf_addr = word & 0xFFF, count = ((word >> 12) & 0xF) + 1;
    const uint32_t array = arrays[(op >> 3) & 3];
    const uint32_t src = cp_base[array] + index * cp_stride[array];
    const int base_known = known(cp_known, 0xA0 | array) && known(cp_known, 0xB0 | array);
    ++n_idx;
    if (base_known && range_backed(src, count * 4u)) {
        const uint32_t fold = src & FOLD_MASK;
        const uint64_t ea = ctl.arena_ea + fold;
        const uint64_t limit = ctl.arena_ea + (fold < MEM1_BYTES ? MEM1_BYTES : MEM2_BASE + MEM2_BYTES);
        const uint8_t *p = idx_read(ea, count * 4u, limit);
        uint32_t values[16], i;
        for (i = 0; i < count; ++i)
            values[i] = rd32(p + i * 4u);
        load_xf(xf_addr, values, count);
        return;
    }
    {   /* the GPU thread reads it itself; what it leaves there is unknown here */
        uint32_t i;
        for (i = 0; i < count && xf_addr + i < XF_WORDS; ++i)
            clear_known(xf_known, xf_addr + i);
        ++n_kept;
        out_need(3);
        out_word(GXF_HDR(GXF_INDEX, op));
        out_word((index << 16) | xf_addr);
        out_word(count);
    }
}

/* ---- the parser (gx_hw.cpp Parse) ------------------------------------------------ */

static inline int run_joinable(uint32_t op)
{
    switch (op & 0xF8) {
    case 0x80: case 0x88: case 0x90: case 0x98: case 0xA0: return 1;
    default: return 0;
    }
}

static inline uint32_t run_indices(uint32_t op, uint32_t n)
{
    switch (op & 0xF8) {
    case 0x90: return n / 3u * 3u;
    case 0x98: case 0xA0: return n >= 3u ? (n - 2u) * 3u : 0u;
    default: return n / 4u * 6u;
    }
}

#define RUN_MAX_VERTS   4096u
#define RUN_MAX_INDICES 8192u
#define RUN_MAX_BYTES   (60u * 1024u)
#define RUN_MAX_PRIMS   4096u

static void parse(Stream *s);

static void call_list(uint32_t addr, uint32_t size)
{
    Stream l;
    ++n_lists;
    if (!range_backed(addr | 0x80000000u, size))
        return;
    l.ring = 0;
    l.pos = 0;
    l.end = size;
    l.guest = addr | 0x80000000u;
    l.base_ea = ctl.arena_ea + (addr & FOLD_MASK);
    l.limit = (l.base_ea + size + 15) & ~15ull;
    list_win.lo = list_win.hi = 0;   /* the game rewrites lists between calls */
    parse(&l);
}

/* Parses whole commands from s->pos to s->end, leaving s->pos at the first incomplete one. */
static void parse(Stream *s)
{
    while (s->pos < s->end) {
        const uint64_t have = s->end - s->pos;
        const uint8_t *p = peek(s, s->pos, have < 9 ? (uint32_t)have : 9u);
        const uint32_t op = p[0];

        if (op >= 0x80 && op < 0xC0) {
            uint32_t count, vs;
            uint64_t total;
            if (have < 3)
                break;
            count = rd16(p + 1);
            vs = vertex_size(op & 7);
            total = 3u + (uint64_t)count * vs;
            if (have < total)
                break;
            if (vs == 0 && count != 0) {
                ++n_unknown;
                s->pos += 3;
                continue;
            }
            if (!s->ring && run_joinable(op) && count != 0) {
                const uint32_t vat = op & 7;
                uint64_t run_end = s->pos + total, q = run_end;
                uint32_t prims = 1, verts = count, indices = run_indices(op, count);
                for (;;) {
                    const uint8_t *h;
                    uint32_t op2, c2, i2;
                    uint64_t t2;
                    while (q < s->end && *peek(s, q, 1) == 0x00)
                        ++q;
                    if (s->end - q < 3)
                        break;
                    h = peek(s, q, 3);
                    op2 = h[0];
                    if (op2 < 0x80 || op2 >= 0xC0 || (op2 & 7) != vat || !run_joinable(op2))
                        break;
                    c2 = rd16(h + 1);
                    t2 = 3u + (uint64_t)c2 * vs;
                    i2 = run_indices(op2, c2);
                    if (s->end - q < t2 || verts + c2 > RUN_MAX_VERTS || indices + i2 > RUN_MAX_INDICES ||
                        q + t2 - s->pos > RUN_MAX_BYTES || prims >= RUN_MAX_PRIMS)
                        break;
                    q += t2;
                    run_end = q;
                    ++prims;
                    verts += c2;
                    indices += i2;
                }
                if (prims > 1) {
                    out_need(7);
                    out_word(GXF_HDR(GXF_RUN, vat));
                    out_word(guest_addr(s, s->pos));
                    out_word((uint32_t)(run_end - s->pos));
                    out_word(prims);
                    out_word(verts);
                    out_word(indices);
                    out_word(vs);
                    s->pos = run_end;
                    continue;
                }
            }
            {
                uint32_t addr = guest_addr(s, s->pos + 3);
                if (s->ring) {
                    const uint32_t off = (uint32_t)(s->pos % ctl.ring_bytes);
                    if (off + total > ctl.ring_bytes) {
                        /* across the wrap: the GPU thread moves the head below the ring */
                        const uint32_t head = ctl.ring_bytes - off;
                        out_need(1);
                        out_word(GXF_HDR(GXF_WRAPCOPY, head));
                        addr = ctl.ring_guest - head + 3u;
                    }
                }
                out_need(4);
                out_word(GXF_HDR(GXF_DRAW, ((op & 0xF8) << 8) | (op & 7)));
                out_word(count);
                out_word(addr);
                out_word(vs);
            }
            s->pos += total;
            continue;
        }

        switch (op) {
        case 0x00: case 0x44: case 0x48:
            ++s->pos;
            continue;
        case 0x08:
            if (have < 6)
                return;
            load_cp(p[1], rd32(p + 2));
            s->pos += 6;
            continue;
        case 0x10: {
            uint32_t header, count, values[16], i;
            if (have < 5)
                return;
            header = rd32(p + 1);
            count = ((header >> 16) & 0xF) + 1;
            if (have < 5u + count * 4u)
                return;
            for (i = 0; i < count; ++i)
                values[i] = rd32(peek(s, s->pos + 5 + i * 4u, 4));
            load_xf(header & 0xFFFFu, values, count);
            s->pos += 5u + count * 4u;
            continue;
        }
        case 0x20: case 0x28: case 0x30: case 0x38:
            if (have < 5)
                return;
            load_index(op, rd32(p + 1));
            s->pos += 5;
            continue;
        case 0x40: {
            uint32_t addr, size;
            if (have < 9)
                return;
            addr = rd32(p + 1);
            size = rd32(p + 5);
            s->pos += 9;
            if (!s->ring)
                ++n_unknown;   /* one level only */
            else
                call_list(addr, size);
            continue;
        }
        case 0x61:
            if (have < 5)
                return;
            load_bp(rd32(p + 1));
            s->pos += 5;
            continue;
        default:
            ++n_unknown;
            ++s->pos;
            continue;
        }
    }
}

/* ---- the loop -------------------------------------------------------------------- */

int main(uint64_t control_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    Stream ring;
    (void)arg2; (void)arg3; (void)arg4;
    ctl_ea = control_ea;
    spu_write_decrementer(0xFFFFFFFFu);
    mfc_get(&ctl, ctl_ea, sizeof ctl, TAG_CTL, 0, 0);
    wait_tag(TAG_CTL);

    memset(&ring, 0, sizeof ring);
    ring.ring = 1;
    ctl.hello = GXF_HELLO;
    out_publish();
    wait_tag(TAG_OUT);

    for (;;) {
        uint64_t written, from;
        ppe_line();
        if (ctl.quit)
            break;
        out_tail_seen = ctl.out_tail;
        written = ctl.written;
        if (written == ctl.seen) {
            const uint32_t d = spu_read_decrementer();
            while (d - spu_read_decrementer() < 160) {}   /* ~2 us between polls */
            continue;
        }
        {
            const uint32_t t0 = spu_read_decrementer();
            ring.end = written;
            idx_valid = 0;             /* matrices may have changed since the last publish */
            ring_win.lo = ring_win.hi = 0;
            from = ring.pos;
            parse(&ring);
            if (ring.pos != from) {
                out_need(3);
                out_word(GXF_HDR(GXF_POS, 0));
                out_word((uint32_t)(ring.pos >> 32));
                out_word((uint32_t)ring.pos);
            }
            ctl.seen = written;
            ctl.unknown = n_unknown;
            ctl.lists = n_lists;
            ctl.dropped = n_dropped;
            ctl.kept = n_kept;
            ctl.idx_loads = n_idx;
            ctl.idx_misses = n_idx_miss;
            ctl.stall = ticks_stall;
            ctl.busy += t0 - spu_read_decrementer();
            out_flush();
            if (!out_n)
                out_publish();   /* `seen` moves even when nothing new went out */
            wait_tag(TAG_OUT);
        }
    }
    return 0;
}
