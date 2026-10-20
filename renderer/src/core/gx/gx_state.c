/* gx_state.c — what the command stream means.
 *
 * See gx_state.h. This file is the sink the parser calls into: it applies XF
 * and BP writes, keeps the decoded state a renderer reads, and forwards draws
 * and framebuffer copies to a backend.
 */
#include "gx_state.h"
#include "../mem/memmap.h"
#include "../../common/log.h"

#include <string.h>

static GXState s_gx;

GXState *gx_state(void) { return &s_gx; }

/* ------------------------------------------------------------------ */
/* XF                                                                   */
/* ------------------------------------------------------------------ */

/* XF values are IEEE floats in the command stream, delivered as raw words. The
 * reinterpretation goes through memcpy rather than a pointer cast: the JIT is
 * built with -fno-strict-aliasing but this file is not required to be, and a
 * cast here is exactly the kind of thing that works until an optimiser
 * notices. */
static float word_to_float(u32 v)
{
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static void xf_decode(XFState *xf, u16 addr)
{
    if (addr >= XF_VIEWPORT && addr < XF_VIEWPORT + 6) {
        unsigned i;
        for (i = 0; i < 3; i++) {
            xf->viewport_scale[i]  = word_to_float(xf->mem[XF_VIEWPORT + i]);
            xf->viewport_offset[i] = word_to_float(xf->mem[XF_VIEWPORT + 3 + i]);
        }
        return;
    }
    if (addr >= XF_PROJECTION && addr < XF_PROJECTION + 7) {
        unsigned i;
        for (i = 0; i < 6; i++)
            xf->projection[i] = word_to_float(xf->mem[XF_PROJECTION + i]);
        /* The seventh word is not a coefficient: it selects between an
         * orthographic and a perspective matrix, which changes what the six
         * mean. Reading it as a float would give a denormal and no warning. */
        xf->projection_orthographic = xf->mem[XF_PROJECTION + 6] & 1u;
        return;
    }
    if (addr == XF_NUMTEXGENS) {
        xf->num_texgens = xf->mem[XF_NUMTEXGENS] & 0xFu;
        return;
    }
    if (addr == XF_NUMCOLORS) {
        xf->num_colorchans = xf->mem[XF_NUMCOLORS] & 0x3u;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Parser sink                                                          */
/* ------------------------------------------------------------------ */

u32 g_xf_generation;   /* bumped on every XF memory write; lets the render
                        * backend skip re-uploading unchanged matrices */

/* Bumped only when the *program* changes. Matrix loads bump
 * g_xf_generation on every draw, and the shader lookup was hashing the
 * whole TEV state on the back of that. The hash does not read matrix
 * values, so a matrix write must not force it. */
u32 g_prog_generation;

/* Narrower generations for the per-draw constant caches: matrix loads bump
 * g_xf_generation on every draw, but lights, material colours and the
 * post-transform matrices are written rarely, and keying their uploads on the
 * global counter rebuilt them -- with int-to-float conversions the PPE pays
 * dearly for -- on every draw. A write anywhere in a range bumps its counter. */
u32 g_xf_light_generation;   /* lights, channel colours and controls */
u32 g_xf_post_generation;    /* post-transform matrices and their indices */

static void xf_note_region(u16 a)
{
    if ((a >= 0x0600u && a < 0x0680u) || (a >= 0x100Au && a <= 0x1011u))
        g_xf_light_generation++;
    else if ((a >= 0x0500u && a < 0x0600u) || (a >= 0x1050u && a < 0x1058u))
        g_xf_post_generation++;
}

static int xf_addr_affects_program(u16 a)
{
    if (a == XF_NUMTEXGENS || a == XF_NUMCOLORS || a == XF_DUALTEXTRAN)
        return 1;
    if (a == (u16)(XF_PROJECTION + 6u))
        return 1;                          /* ortho bit, not the coefficients */
    if (a >= 0x100Eu && a <= 0x1011u)
        return 1;                          /* colour-channel control */
    if (a >= 0x1040u && a < 0x1048u)
        return 1;                          /* texgen inputs */
    if (a >= 0x1050u && a < 0x1058u)
        return 1;                          /* texgen matrix index */
    return 0;
}

/* Raster state (blend, depth, scissor, EFB copy, fog distances) is not
 * compiled into the program. Everything else the TEV hash reads is.
 * `oldv`/`newv` are the 24-bit register values. Texture address, cache
 * layout and filter are sampler state: the hash only reads the wrap
 * nibble of mode0 and, for an indirect stage, the size in image0. */
static int bp_affects_program(u8 reg, u32 oldv, u32 newv)
{
    if (reg <= 0x05u) return 1;
    if (reg >= 0x06u && reg <= 0x1Fu) return 1;
    if (reg >= 0x25u && reg <= 0x2Fu) return 1;
    if ((reg >= 0x80u && reg <= 0x83u) || (reg >= 0xA0u && reg <= 0xA3u))
        return ((oldv ^ newv) & 0xFu) != 0;
    if ((reg >= 0x88u && reg <= 0x8Bu) || (reg >= 0xA8u && reg <= 0xABu))
        return 1;
    if (reg >= 0xC0u && reg <= 0xE7u) return 1;
    if (reg >= 0xF1u && reg <= 0xFDu) return 1;
    return 0;
}

void gx_state_load_xf(GXState *g, u16 addr, const u32 *values, unsigned count)
{
    g_xf_generation++;
    unsigned i;
    int prog = 0;

    for (i = 0; i < count; i++) {
        u16 a = (u16)(addr + i);
        if (a >= XF_MEM_SIZE) {
            /* Beyond what we model. Reported once rather than per write,
             * because a title that does this does it every frame. */
            LOG_WARN_ONCE(LOG_VIDEO, "XF write to %04x is outside modelled memory",
                          a);
            continue;
        }
        if (xf_addr_affects_program(a) && g->xf.mem[a] != values[i])
            prog = 1;
        if (g->xf.mem[a] != values[i])
            xf_note_region(a);
        g->xf.mem[a] = values[i];
        xf_mirror_touch(&g->xf, a);
        xf_decode(&g->xf, a);
    }
    if (prog)
        g_prog_generation++;
}

/* Bumped on every BP write, the counterpart of g_xf_generation.
 *
 * The renderer's shader lookup hashes the whole XF and TEV state on EVERY
 * draw, and the hash walk -- not the table probe -- is what it costs. A title
 * sets a material and issues many draws with it, so the hash almost always
 * produces the answer it produced last time. With both generations available
 * the lookup can tell "nothing has changed" in two compares. */
u32 g_bp_generation;

void gx_state_load_bp(GXState *g, u8 reg, u32 value)
{
    g_bp_generation++;
    {
        u32 masked = value & 0x00FFFFFFu;
        if (g->bp.raw[reg] != masked &&
            bp_affects_program(reg, g->bp.raw[reg], masked))
            g_prog_generation++;
    }
    u64 before = g->bp.copies;

    bp_write(&g->bp, reg, value);

    /* An EFB copy is the only BP write with a side effect, and it is the one
     * that makes anything visible, so it is forwarded rather than left for the
     * backend to notice by polling. */
    if (g->bp.copies != before && g->backend.efb_copy)
        g->backend.efb_copy(g->backend.ctx, g, &g->bp.copy);
}

void gx_state_draw(GXState *g, GXPrimitive prim, unsigned vat,
                   u16 vertex_count, u32 data_addr, u32 vertex_size)
{

    if (!g->backend.draw) {
        g->draws_dropped++;
        return;
    }
    g->backend.draw(g->backend.ctx, g, prim, vat, vertex_count,
                    data_addr, vertex_size);
}

void gx_state_draw_run(GXState *g, unsigned vat, u32 cmd_addr, u32 cmd_bytes,
                       u32 prims, u32 verts, u32 nidx, u32 vertex_size)
{
    if (!g->backend.draw_run) {
        g->draws_dropped += prims;
        return;
    }
    g->backend.draw_run(g->backend.ctx, g, vat, cmd_addr, cmd_bytes, prims,
                        verts, nidx, vertex_size);
}

void gx_state_load_index(GXState *g, u8 which, u16 index, u16 xf_addr, u8 count)
{
    g_xf_generation++;
    const GXCPRegs *cp = &g->parser.cp;
    unsigned array;
    u32 src;
    const u8 *host;
    unsigned i;

    /* The four index commands name four different arrays. Their numbering is
     * the hardware's and does not follow from the opcode arithmetically, which
     * is why it is a table rather than a shift. */
    switch (which) {
    /* CP arrays 12-15, NOT 0-3. Arrays 0-11 are the vertex ATTRIBUTE arrays
     * (positions, normals, colours, texcoords); the four indexed-XF commands
     * have their own bases at 12-15 (Dolphin: CPArray::XF_A..XF_D). Reading
     * from 0-3 fetched "matrices" out of the position/normal vertex data --
     * every skinned or matrix-palette object (i.e. ALL 3D models) transformed
     * by garbage and left the screen: menus survived because their direct
     * XF loads use matrix 0 via the command stream. */
    case GX_LOAD_INDX_A: array = 12; break;  /* position matrices */
    case GX_LOAD_INDX_B: array = 13; break;  /* normal matrices   */
    case GX_LOAD_INDX_C: array = 14; break;  /* texture matrices  */
    case GX_LOAD_INDX_D: array = 15; break;  /* light objects     */
    default: return;
    }

    src = cp->array_base[array] + (u32)index * cp->array_stride[array];
    /* One translation for the whole run instead of a region scan per word:
     * a skinned model loads a 12-word matrix this way per bone per draw. */
    host = (mem_valid_span(src) >= (u32)count * 4u) ? (const u8 *)mem_ptr(src)
                                                    : NULL;

    for (i = 0; i < count; i++) {
        u16 a = (u16)(xf_addr + i);
        if (a >= XF_MEM_SIZE)
            break;
        {
            u32 w = host ? dol_be32(host + i * 4u) : mem_read32(src + i * 4u);
            if (xf_addr_affects_program(a) && g->xf.mem[a] != w)
                g_prog_generation++;
            if (g->xf.mem[a] != w)
                xf_note_region(a);
            g->xf.mem[a] = w;
            xf_mirror_touch(&g->xf, a);
        }
        xf_decode(&g->xf, a);
    }
}

/* ------------------------------------------------------------------ */

u32 g_cp_generation;

/* CP register loads. The command parser lives in the runtime now
 * (gx_hw.cpp) and hands every load here, so the vertex descriptor, attribute
 * tables and array bases the renderer reads stay in GXState. */
void gx_state_load_cp(GXState *g, u8 reg, u32 value)
{
    GXCPRegs *cp = &g->parser.cp;
    unsigned n = reg & 0x0Fu;

    g_cp_generation++;

    switch (reg & 0xF0u) {
    case CP_REG_MATINDEX_A:  cp->matindex_a = value; break;
    case CP_REG_MATINDEX_B:  cp->matindex_b = value; break;
    case CP_REG_VCD_LO:
        if (cp->vcd_lo != value) {
            cp->vcd_lo = value;
            g_prog_generation++;
        }
        break;
    case CP_REG_VCD_HI:
        if (cp->vcd_hi != value) {
            cp->vcd_hi = value;
            g_prog_generation++;
        }
        break;
    case CP_REG_VAT_A:       if (n < GX_NUM_VAT) cp->vat_a[n] = value; break;
    case CP_REG_VAT_B:       if (n < GX_NUM_VAT) cp->vat_b[n] = value; break;
    case CP_REG_VAT_C:       if (n < GX_NUM_VAT) cp->vat_c[n] = value; break;
    case CP_REG_ARRAY_BASE:  cp->array_base[n]   = value; break;
    case CP_REG_ARRAY_STRIDE:cp->array_stride[n] = value; break;
    default:
        break;
    }
}

static u16 gx_guest_read16(u32 addr) { return mem_read16(addr); }

void gx_state_init(GXState *g, const GXBackend *backend)
{
    memset(g, 0, sizeof *g);
    /* After the wipe, or it would be cleared: the palette loader needs a way
     * to read guest memory and gx_state is the layer that legitimately has
     * one. */
    g->bp.read16 = gx_guest_read16;
    if (backend)
        g->backend = *backend;

    bp_reset(&g->bp);
    /* Everything is unmirrored at the start: the first draw uploads it all. */
    g->xf.mirror_dirty_pos  = ~0ull;
    g->xf.mirror_dirty_post = ~0ull;
    g->xf.mirror_dirty_nrm  = ~0u;
}

void gx_state_reset(GXState *g)
{
    GXBackend saved = g->backend;
    gx_state_init(g, &saved);
}


