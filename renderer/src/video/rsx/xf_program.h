/* xf_program.h — XF configuration to an RSX vertex program.
 *
 * The vertex-side counterpart to tev_program.h. Gekko's transform unit takes a
 * position, transforms it by a 3x4 position matrix and then a 4x4 projection,
 * generates texture coordinates, and optionally lights the vertex. RSX does all
 * of that in a vertex program, so the same generate-and-cache approach applies.
 *
 * Two things make this simpler than the fragment side and one makes it harder.
 *
 * Simpler: the vertex unit has no register-pressure cliff, so the generated
 * code can be written for clarity rather than squeezed. And a vertex program is
 * far shorter than a sixteen-stage TEV chain.
 *
 * Harder: the *matrices* are not part of the program. They live in XF memory
 * and change per draw -- sometimes per object -- so they are uploaded as
 * program constants and the generated code refers to them by index. The
 * program therefore depends on the *shape* of the XF configuration (how many
 * texture coordinates, whether they are generated or passed through) but not on
 * the matrix values, which is exactly what makes caching worthwhile: a title
 * with one vertex format and a thousand objects compiles one program.
 *
 * Verified the same way TEV is: each instruction is a verified encoding
 * (vp_emitter.h against cgcomp), and each generated program is executed by a
 * small interpreter and compared against a direct implementation of the
 * transform (tests/test_xf.c).
 */
#ifndef DOLPHIN_VIDEO_RSX_XF_PROGRAM_H
#define DOLPHIN_VIDEO_RSX_XF_PROGRAM_H

#include "vp_emitter.h"
#include "../../core/gx/gx_state.h"

/* Where the generated program expects its constants.
 *
 * Fixed rather than allocated: the backend uploads matrices to these slots
 * before each draw, and a fixed layout means the upload does not have to be
 * regenerated alongside the program. */
#define XF_CONST_PROJ       0               /* 4 rows: projection x position */
#define XF_CONST_TEXMTX(n)  (4 + (n) * 4)   /* 4 rows each, up to 8 coords   */
/* The model-view matrix on its own. Lighting happens in *view* space, before
 * the projection, so the combined matrix above cannot serve: it is the one
 * place in this file where the two have to be separate. Three rows; the fourth
 * is the implicit {0,0,0,1}. */
#define XF_CONST_POSMTX     36              /* 3 rows                        */
/* The inverse transpose, for normals. Using the position matrix instead is
 * correct only while the model matrix has no non-uniform scale, which is the
 * kind of assumption that holds until a title squashes a wheel. */
#define XF_CONST_NRMMTX     39              /* 3 rows                        */
/* Dual-transform ("post") matrices, one per texgen, 3 rows each. Indexed by
 * texgen rather than by the hardware's 64-row matrix memory: the index a
 * texgen selects is part of the program's shape, so the backend uploads the
 * selected rows into a fixed slot and the code does not have to be
 * regenerated when a different matrix is selected. */
#define XF_CONST_POSTMTX(n) (42 + (n) * 3)  /* 8 x 3 rows -> 42..65          */

/* Lights. Five vectors each, laid out to match the hardware's light memory so
 * the upload is a copy rather than a shuffle. */
#define XF_CONST_LIGHT(i)   (66 + (i) * 5)  /* 8 x 5 -> 66..105              */
#define XF_LIGHT_COLOR      0
#define XF_LIGHT_COSATT     1
#define XF_LIGHT_DISTATT    2
#define XF_LIGHT_POS        3
#define XF_LIGHT_DIR        4

/* Per-channel material and ambient colours, for the channels whose source is
 * a register rather than the vertex. */
#define XF_CONST_MATERIAL(c) (106 + (c))    /* 106..107                      */
#define XF_CONST_AMBIENT(c)  (108 + (c))    /* 108..109                      */

/* A literal (0, 1, 0.5, 2) the generated code reaches for whenever it needs a
 * plain number. A vertex instruction can name exactly one constant *register*,
 * so having the small numbers in one register rather than as immediates is what
 * lets an instruction take a light colour and a 1.0 at the same time; and it
 * avoids the trick of comparing a register against itself, which produces zero
 * rather than one when the register happens to hold a NaN. */
#define XF_CONST_ONE        110

/* (-2^-b_shift, B, A, -C) -- everything the fog coordinate needs that is not
 * already in the projection. */
#define XF_CONST_FOG        111

#define XF_CONST_COUNT      112

/* THE XF MIRROR (g_xf_mirror).
 *
 * In the layout above the backend re-derives and uploads the selected
 * matrices for every draw, which is most of the per-draw command stream and
 * all of the per-draw matrix maths on the PPE. The mirror instead keeps the
 * transform unit's matrix memories themselves in constant registers -- the
 * rows are uploaded when the title writes XF memory, not when it draws -- and
 * the program picks its matrices out of them by the indices the draw supplies,
 * through an address register. That is what XF does in silicon: the matrix
 * index selects rows of a memory, it does not copy them.
 *
 * Rows sit above XF_CONST_COUNT so the fixed layout keeps working beside them
 * (lights, material, ambient, the literal and fog are shared). 285 of the
 * RSX's 468 vertex constants. */
#define XFM_POS     112   /* 64 rows: XF 0x000-0x0FF, position + texture matrices */
#define XFM_NRM     176   /* 34 rows: XF 0x400-0x45F as rows of three; the two
                           * spare rows absorb a 3-row read at index 30/31 */
#define XFM_POST    210   /* 64 rows: XF 0x500-0x5FF, dual-transform matrices    */
#define XFM_IDENT   274   /* 4 rows: identity, for pre-transformed (skinned) draws */
#define XFM_PROJ    278   /* 4 rows: the projection, depth range folded in        */
#define XFM_IDX     282   /* 3 rows: row offsets the draw selects --
                           *   +0 (pos, nrm, tex0, tex1)  -> A0
                           *   +1 (tex2, tex3, tex4, tex5) -> A1
                           *   +2 (tex6, tex7, -, -)       -> A0, reloaded     */
#define XFM_END     285

/* Offsets that land a relative read on XFM_IDENT. */
#define XFM_IDENT_POS (XFM_IDENT - XFM_POS)
#define XFM_IDENT_NRM (XFM_IDENT - XFM_NRM)

/* Non-zero: generate mirror-addressed programs (part of the program key). */
extern int g_xf_mirror;

/* The interpolant the fog coordinate travels in. Must agree with
 * tev_program.c's TEV_FOG_COORD; they are two ends of one wire. */
#define XF_FOG_TEXCOORD     7

typedef struct {
    u32 instructions;
    u32 words;
    u32 input_mask;
    u32 output_mask;
    int truncated;
    /* What the generated program needs from the vertex and from the state, so
     * the backend can decide what to upload without re-deriving it. */
    unsigned uses_normal;
    unsigned uses_posmtx;       /* lighting: the model-view and normal matrices */
    /* The program evaluates a colour channel, so it reads the material and
     * ambient constants -- whether or not any light is on. An unlit channel
     * whose material source is the register reads XF_CONST_MATERIAL and
     * nothing else, and gating that upload on "a light is in use" would hand
     * it whatever the constant file happened to hold. */
    unsigned uses_channels;
    unsigned texgens;
    unsigned lights_used;       /* bit per light slot referenced         */
    unsigned post_used;         /* bit per texgen with a post-transform  */
    unsigned fog;
} XFProgramInfo;

/* The constant *values* the generated program reads, unpacked from XF and BP
 * memory. They live here rather than in the backend for one reason: the
 * backend cannot be linked off the console, and every one of these is a
 * byte-order or layout decision that is silent when wrong. A light colour
 * packed ABGR read as RGBA is a light of the wrong hue at the right
 * brightness; a normal matrix read as rows of four instead of rows of three
 * tilts every normal. Both are checkable here and nowhere else.
 *
 * `out` sizes: 20 floats for a light (five vectors), 8 for the two material or
 * ambient colours, 4 for the fog vector, 12 for the normal matrix. */
void xf_light_constants(const XFState *xf, unsigned light, f32 out[20]);
void xf_material_constants(const XFState *xf, f32 material[8], f32 ambient[8]);
void xf_normal_matrix(const XFState *xf, unsigned posmtx_idx, f32 out[12]);
void xf_fog_constants(const BPState *bp, f32 out[4]);

/* Generate a vertex program for the current transform configuration. Returns 0
 * on success. */
int xf_generate(const GXState *g, VPEmitter *e, XFProgramInfo *info);

/* The cache key: every part of the XF and vertex configuration the generated
 * program depends on, and nothing else. Matrix *values* are deliberately
 * excluded -- they are constants the program reads, not part of its shape, and
 * including them would recompile every time an object moved. */
u64 xf_state_hash(const GXState *g);

#endif /* DOLPHIN_VIDEO_RSX_XF_PROGRAM_H */
