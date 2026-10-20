/* rsx_shader.h — hand our generated microcode to the RSX.
 *
 * fp_emitter.h and vp_emitter.h produce raw NV40 microcode, verified word for
 * word against Sony's own compiler (`cgcomp`) by tools/fp_verify.sh and
 * tools/vp_verify.sh. What they do not produce is the small *container* the
 * driver expects: cgcomp normally emits a header describing the program's
 * register and attribute usage, and PSL1GHT's upload functions take that
 * header rather than bare instructions.
 *
 * This module builds that header around our own microcode, so the verified
 * encoders can be used directly with no offline compilation step -- which is
 * the whole point, since an emulator generates its shaders at run time from
 * the guest's TEV and XF state and cannot precompile anything.
 */
#ifndef DOLPHIN_VIDEO_RSX_SHADER_H
#define DOLPHIN_VIDEO_RSX_SHADER_H

#include "../../common/types.h"

/* A fragment program must live in RSX-visible memory, because the GPU fetches
 * it by offset rather than being handed the words. This owns that allocation. */
typedef struct {
    void *ucode;        /* RSX-visible copy of the microcode   */
    u32   offset;       /* its RSX offset                      */
    u32   words;
    u32   num_regs;     /* temporaries used; drives FP_CONTROL */
    u32   texcoords;    /* which TEX interpolants the program reads */
    u32   serial;       /* unique per creation: what "already bound" compares */
    int   valid;
} RsxFragProgram;

typedef struct {
    const u32 *ucode;   /* stays in main memory; uploaded by command */
    u32   words;
    u32   instructions;
    u32   input_mask;
    u32   output_mask;
    u32   serial;       /* unique per creation: what "already bound" compares */
    int   valid;
} RsxVertProgram;

/* Copy fragment microcode into RSX memory and record what the header needs.
 * `num_regs` is the count of temporary registers the program uses (minimum 1).
 * Returns 0 on success. */
int  rsx_fp_create(RsxFragProgram *fp, const u32 *ucode, u32 words,
                   u32 num_regs, u32 texcoord_mask);
void rsx_fp_destroy(RsxFragProgram *fp);
/* Where rsx_fp_destroy sends the microcode: draws already queued may still
 * fetch it, so the renderer holds it until the frame fence passes. NULL frees
 * at once (only safe with the GPU idle). */
extern void (*g_rsx_fp_release)(void *ucode);

/* Vertex microcode is uploaded through the command buffer, so it can stay in
 * main memory; this just records the shape. */
void rsx_vp_create(RsxVertProgram *vp, const u32 *ucode, u32 words,
                   u32 instructions, u32 input_mask, u32 output_mask);

/* Bind for drawing. Both must be called before a draw; the RSX keeps them
 * until replaced. */
void rsx_bind_programs(const RsxVertProgram *vp, const RsxFragProgram *fp);

/* Upload vertex-program constants (4 floats per row, `rows` rows starting at
 * `base`). Matrices live here. */
void rsx_vp_constants(u32 base, u32 rows, const f32 *values);

#if defined(__ALTIVEC__) && defined(__PS3__)
#include <altivec.h>
#include <stdint.h>
#include <rsx/rsx.h>
extern gcmContextData *rsx_context(void);
/* Vertex-program constant rows from vector registers.
 *
 * rsx_vp_constants takes an array, which on the PPE means the caller stores
 * the rows it just computed and the upload loads them straight back -- a
 * load-hit-store each, ~60 cycles, several times a draw. This keeps the rows
 * in registers: the shadow compare is a vector compare against memory written
 * long ago, and a changed run is written into the command buffer with one
 * vector store per row (NOP words in front align the data). */
#define RSX_VPC_ROWS 468
extern f32 g_vpc_shadow[RSX_VPC_ROWS * 4];
extern u8  g_vpc_known[RSX_VPC_ROWS];
u32 *rsx_cmd_reserve(u32 words);

static inline int rsx_vpc_same(u32 row, vector float v)
{
    return row < RSX_VPC_ROWS && g_vpc_known[row] &&
           vec_all_eq(v, ((const vector float *)g_vpc_shadow)[row]);
}

/* Upload rows [base, base + n) of r (n <= 4) that differ from the shadow, as
 * one block covering the first through the last changed row. */
static inline void rsx_vpc_rows(u32 base, unsigned n, vector float r0, vector float r1,
                                vector float r2, vector float r3)
{
    unsigned first = 4, last = 0, k, pad;
    u32 *p;
    if (base + n > RSX_VPC_ROWS) return;
    if (n > 0 && !rsx_vpc_same(base + 0, r0)) { first = 0; last = 0; }
    if (n > 1 && !rsx_vpc_same(base + 1, r1)) { if (first > 1) first = 1; last = 1; }
    if (n > 2 && !rsx_vpc_same(base + 2, r2)) { if (first > 2) first = 2; last = 2; }
    if (n > 3 && !rsx_vpc_same(base + 3, r3)) { if (first > 3) first = 3; last = 3; }
    if (first > 3)
        return;
    k = last - first + 1;
    /* Header and base index, then the rows on a 16-byte boundary. */
    p = rsx_cmd_reserve(3 + 2 + 4 * k);
    if (!p) return;
    pad = (unsigned)(((16u - (((uintptr_t)(p + 2)) & 15u)) & 15u) >> 2);
    {
        unsigned i;
        for (i = 0; i < pad; i++) p[i] = 0;        /* NV NOPs */
        p[pad] = ((4u * k + 1u) << 18) | 0x1EFCu;  /* VP_UPLOAD_CONST_ID */
        p[pad + 1] = base + first;
        {
            vector float *d = (vector float *)(p + pad + 2);
            vector float *sh = (vector float *)g_vpc_shadow;
            for (i = first; i <= last; i++) {
                vector float v = i == 0 ? r0 : i == 1 ? r1 : i == 2 ? r2 : r3;
                d[i - first] = v;
                sh[base + i] = v;
                g_vpc_known[base + i] = 1;
            }
        }
        /* Give back the NOP words the alignment did not use. */
        {
            gcmContextData *c = rsx_context();
            c->current -= (3u - pad);
        }
    }
}
#endif
/* Forget what the RSX constant file holds (context reset): the next upload of
 * every row goes out even if its values look unchanged. */
void rsx_vp_constants_invalidate(void);

/* Read the fragment microcode back out of RSX memory. The GPU fetches the
 * program from there rather than being handed the words, so what matters is
 * what is actually in that memory -- not what we intended to copy into it. */
u32 rsx_fp_readback(const RsxFragProgram *fp, u32 *out, u32 max_words);

#endif
