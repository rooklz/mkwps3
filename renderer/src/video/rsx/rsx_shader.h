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
/* Forget what the RSX constant file holds (context reset): the next upload of
 * every row goes out even if its values look unchanged. */
void rsx_vp_constants_invalidate(void);

/* Read the fragment microcode back out of RSX memory. The GPU fetches the
 * program from there rather than being handed the words, so what matters is
 * what is actually in that memory -- not what we intended to copy into it. */
u32 rsx_fp_readback(const RsxFragProgram *fp, u32 *out, u32 max_words);

#endif
