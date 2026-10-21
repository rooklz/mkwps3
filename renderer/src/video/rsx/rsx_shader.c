/* rsx_shader.c — see rsx_shader.h.
 *
 * The header structures (rsxVertexProgram / rsxFragmentProgram) are normally
 * produced by cgcomp and read straight out of a .vpo/.fpo file. We fill them in
 * ourselves. Only the fields the upload path actually reads are set:
 *
 *   - ucode_off: the *byte offset from the header to the microcode*. Our
 *     microcode is a separate array, so for vertex programs the pointer is
 *     passed alongside and this field is unused; for fragment programs the
 *     GPU is given an RSX offset instead.
 *   - num_insn / num_regs: how much to upload and how many temporaries the
 *     program needs. Understating num_regs on the fragment side is the classic
 *     way to get a program that renders garbage on some pixels only.
 *   - input_mask / output_mask: which vertex attributes are read and which
 *     interpolants are written. The RSX uses these to route the two stages
 *     together; a mismatch silently drops an interpolant.
 */
#include "rsx_shader.h"
#include "rsx_video.h"
#include "../../common/log.h"

#include <string.h>
#include <stdlib.h>
#include <rsx/rsx.h>

extern gcmContextData *rsx_context(void);

/* Program identities. A cache slot is reused for a different program when it
 * is evicted, so the slot's address says nothing about which program is in
 * it; the serial does. */
static u32 s_program_serial;
void (*g_rsx_fp_release)(void *ucode);

/* Fragment-program storage.
 *
 * Every program built went through rsxMemalign and every eviction through
 * rsxFree, into the same heap as every texture. After a few minutes of play
 * that heap is a long, fragmented list and an allocation walked it: measured
 * at over 500 us per program in a race, several programs a frame. Programs
 * are small (at most FP_WORDS_MAX words, 8 KiB) and fall in a few sizes, so
 * they get their own region in five size classes, each a zone of fixed slots
 * with a free list kept in host memory -- the PPE reads RSX local memory very
 * slowly, so nothing about the pool lives in it. A slot is released through
 * the same frame-fence quarantine as before (rsx_release_block). */
#define FPP_CLASSES 5
static const u32 k_fpp_slot[FPP_CLASSES]  = {512u, 1024u, 2048u, 4096u, 8192u};
static const u32 k_fpp_slots[FPP_CLASSES] = {4096u, 4096u, 2048u, 1024u, 256u};
static u8   *s_fpp_base;                 /* the whole region */
static u8   *s_fpp_zone[FPP_CLASSES];
static u32   s_fpp_offset;               /* RSX offset of s_fpp_base */
static u32   s_fpp_used[FPP_CLASSES];    /* slots ever handed out */
static u32  *s_fpp_free[FPP_CLASSES];    /* released slot indices */
static u32   s_fpp_nfree[FPP_CLASSES];
static u32   s_fpp_bytes;
static int   s_fpp_state;                /* 0 untried, 1 ready, -1 unavailable */

static int fpp_init(void)
{
    u32 total = 0, c;
    u8 *at;
    if (s_fpp_state)
        return s_fpp_state > 0;
    s_fpp_state = -1;
    for (c = 0; c < FPP_CLASSES; c++)
        total += k_fpp_slot[c] * k_fpp_slots[c];
    s_fpp_base = (u8 *)rsxMemalign(128, total);
    if (!s_fpp_base)
        return 0;
    if (rsxAddressToOffset(s_fpp_base, &s_fpp_offset) != 0) {
        rsxFree(s_fpp_base);
        s_fpp_base = NULL;
        return 0;
    }
    at = s_fpp_base;
    for (c = 0; c < FPP_CLASSES; c++) {
        s_fpp_zone[c] = at;
        at += k_fpp_slot[c] * k_fpp_slots[c];
        s_fpp_free[c] = (u32 *)malloc(k_fpp_slots[c] * sizeof(u32));
        if (!s_fpp_free[c])
            return 0;
    }
    s_fpp_bytes = total;
    s_fpp_state = 1;
    return 1;
}

static void *fpp_alloc(u32 bytes, u32 *offset)
{
    u32 c, slot;
    if (!fpp_init())
        return NULL;
    for (c = 0; c < FPP_CLASSES && k_fpp_slot[c] < bytes; c++)
        ;
    for (; c < FPP_CLASSES; c++) {
        if (s_fpp_nfree[c])
            slot = s_fpp_free[c][--s_fpp_nfree[c]];
        else if (s_fpp_used[c] < k_fpp_slots[c])
            slot = s_fpp_used[c]++;
        else
            continue;                    /* class full: try the next size up */
        {
            u8 *p = s_fpp_zone[c] + (size_t)slot * k_fpp_slot[c];
            *offset = s_fpp_offset + (u32)(p - s_fpp_base);
            return p;
        }
    }
    return NULL;
}

/* Free an RSX allocation: a pool slot back to its class, anything else to the
 * heap. The quarantine calls this once the GPU is done with the block. */
void rsx_release_block(void *ptr)
{
    u8 *p = (u8 *)ptr;
    if (!p)
        return;
    if (s_fpp_base && p >= s_fpp_base && p < s_fpp_base + s_fpp_bytes) {
        u32 c;
        for (c = FPP_CLASSES; c-- > 0;)
            if (p >= s_fpp_zone[c]) {
                s_fpp_free[c][s_fpp_nfree[c]++] = (u32)((p - s_fpp_zone[c]) / k_fpp_slot[c]);
                return;
            }
    }
    rsxFree(ptr);
}

int rsx_fp_create(RsxFragProgram *fp, const u32 *ucode, u32 words,
                  u32 num_regs, u32 texcoord_mask)
{
    memset(fp, 0, sizeof *fp);

    /* 64-byte alignment: the fragment program fetch reads whole cache lines,
     * and an unaligned program is a documented way to hang the pipeline. */
    fp->ucode = fpp_alloc(words * 4, &fp->offset);
    if (fp->ucode) {
        memcpy(fp->ucode, ucode, words * 4);
        goto placed;
    }
    fp->ucode = rsxMemalign(64, words * 4);
    if (!fp->ucode) {
        LOG_ERROR(LOG_VIDEO, "fp: %u words would not allocate", (unsigned)words);
        return -1;
    }
    memcpy(fp->ucode, ucode, words * 4);

    if (rsxAddressToOffset(fp->ucode, &fp->offset) != 0) {
        /* Freed, not abandoned. This path leaked the allocation, and the
         * caller's cache slot is left unused on failure -- so the next draw in
         * the same state came straight back here and leaked another copy, at
         * draw rate, out of video memory that nothing else ever returns. */
        LOG_ERROR(LOG_VIDEO, "fp: microcode has no RSX offset");
        rsxFree(fp->ucode);
        memset(fp, 0, sizeof *fp);
        return -1;
    }

placed:
    fp->words    = words;
    /* The RSX allocates fragment temporaries in pairs: an odd count loses the
     * highest register, which then reads as zero. A program whose top register
     * has an even index (R10, the indirect sample, in EGG's light textures)
     * computed with a zero there, and its light texture came out one flat
     * colour. */
    num_regs = (num_regs + 1u) & ~1u;
    fp->num_regs = num_regs < 2 ? 2 : num_regs;
    fp->texcoords = texcoord_mask;
    fp->serial   = ++s_program_serial;
    fp->valid    = 1;
    return 0;
}

void rsx_fp_destroy(RsxFragProgram *fp)
{
    /* Not freed on the spot: draws queued earlier in the frame fetch this
     * microcode when the GPU reaches them, and an allocation reusing the block
     * would hand them the next program instead. */
    if (fp->ucode) {
        if (g_rsx_fp_release) g_rsx_fp_release(fp->ucode);
        else rsx_release_block(fp->ucode);
    }
    memset(fp, 0, sizeof *fp);
}

void rsx_vp_create(RsxVertProgram *vp, const u32 *ucode, u32 words,
                   u32 instructions, u32 input_mask, u32 output_mask)
{
    vp->ucode        = ucode;
    vp->words        = words;
    vp->instructions = instructions;
    vp->input_mask   = input_mask;
    vp->output_mask  = output_mask;
    vp->serial       = ++s_program_serial;
    vp->valid        = 1;
}

/* Last pair actually loaded, and the frame it was loaded in.
 *
 * Binding was unconditional: every draw re-uploaded the whole vertex program
 * microcode into the command buffer, even though consecutive draws routinely
 * share a program -- a title sorts by material, so the same pair is bound over
 * and over. Loading a program is not a pointer swap; the vertex microcode is
 * copied word by word into the ring.
 *
 * The epoch is what makes skipping safe: the flip reinitialises the RSX
 * context, so a binding cannot be assumed to survive into the next frame. It
 * is bumped once per frame by rsx_shader_new_frame(). */
static u32 s_bound_vp, s_bound_fp;     /* program serials */
static u32 s_bound_epoch, s_shader_epoch = 1;

void rsx_shader_new_frame(void) { s_shader_epoch++; }

void rsx_bind_programs(const RsxVertProgram *vp, const RsxFragProgram *fp)
{
    gcmContextData *ctx = rsx_context();
    rsxVertexProgram   vph;
    rsxFragmentProgram fph;

    if (!ctx || !vp->valid || !fp->valid)
        return;

    /* By serial, not by address: an evicted cache slot is rebuilt in place,
     * and comparing addresses skipped loading the new program into it -- the
     * draw ran with whatever program the slot held before. */
    if (vp->serial == s_bound_vp && fp->serial == s_bound_fp &&
        s_bound_epoch == s_shader_epoch)
        return;                     /* already the live pair this frame */
    s_bound_vp = vp->serial; s_bound_fp = fp->serial;
    s_bound_epoch = s_shader_epoch;

    memset(&vph, 0, sizeof vph);
    vph.num_insn    = (u16)vp->instructions;
    /* The vertex program's register count decides exactly one thing in the
     * loader: whether TRANSFORM_TIMEOUT is programmed for a program using more
     * than 32 temporaries. NV4x has 32, the generated programs use nine at
     * their longest (a lit channel), and understating the count therefore
     * selects the same configuration a truthful one would. Checked rather than
     * assumed, because "the register count is a lie" is an alarming thing to
     * read next to a generator that now uses nine of them. */
    vph.num_regs    = 2;
    vph.input_mask  = vp->input_mask;
    vph.output_mask = vp->output_mask;
    vph.const_start = 0;
    vph.insn_start  = 0;
    rsxLoadVertexProgram(ctx, &vph, vp->ucode);

    memset(&fph, 0, sizeof fph);
    fph.num_insn = (u16)(fp->words / 4);
    fph.num_regs = (u16)fp->num_regs;
    /* Left at zero on purpose. The loader builds FP_CONTROL itself:
     *     fp_control | (num_regs << TEMP_COUNT_SHIFT) | (1 << 10)
     * so setting the temporary count here too ORs it in twice and corrupts the
     * field. num_regs alone is what the count should come from. */
    /* 0x40: the colour result is R0, in FP32.
     *
     * cgcomp sets this only when the parsed program writes an OUTPUT-typed
     * register (emit_dst, compilerfp.cpp); Cg-generated shaders always do, so
     * every working homebrew carries it. Assembly that writes plain R0 -- ours
     * and cgcomp's own `-a` output alike -- parses it as a temp, and the flag
     * is silently absent. Without it the ROP reads the output as FP16 halves
     * of R0: every constant renders as a saturate/zero pattern of its float's
     * halfwords, interpolants collapse to (0,1,0,1), and WPOS leaks its
     * half-pixel fraction into one channel. Matched, bit for bit, on all of
     * them. */
    fph.fp_control = 0x40;
    /* Which texture-coordinate interpolants this program reads. The loader
     * emits a TEX_COORD_CONTROL per set bit; with none set the hardware leaves
     * the coordinate in its default 2D configuration and only the first two
     * components carry a varying value -- which showed up as a gradient in red
     * with green and blue frozen. */
    fph.texcoords  = (u16)fp->texcoords;
    fph.texcoord2D = 0;
    fph.texcoord3D = 0;
    rsxLoadFragmentProgramLocation(ctx, &fph, fp->offset, GCM_LOCATION_RSX);
}

u32 rsx_fp_readback(const RsxFragProgram *fp, u32 *out, u32 max_words)
{
    u32 n, i;
    const volatile u32 *p = (const volatile u32 *)fp->ucode;
    if (!fp->valid || !fp->ucode) return 0;
    n = fp->words < max_words ? fp->words : max_words;
    for (i = 0; i < n; i++) out[i] = p[i];
    return n;
}

/* A CPU copy of the RSX vertex constant file, so a draw only emits the rows
 * that differ from what the GPU already holds. The renderer re-derives its
 * matrices, lights and materials for every draw, but most of them repeat from
 * one draw to the next; sending them anyway was most of the per-draw command
 * stream. The constant file persists across program binds (our programs carry
 * no internal constants -- rsx_bind_programs leaves num_const at 0), and
 * uploads are ordered in the command stream with the draws, so skipping an
 * identical row is exact. */
#define VP_CONST_ROWS 468
/* Shared with the vector-register path in rsx_shader.h (rsx_vpc_row). */
f32 g_vpc_shadow[VP_CONST_ROWS * 4] __attribute__((aligned(16)));
u8  g_vpc_known[VP_CONST_ROWS];
#define s_vpc_shadow g_vpc_shadow
#define s_vpc_known  g_vpc_known

void rsx_vp_constants_invalidate(void)
{
    memset(s_vpc_known, 0, sizeof s_vpc_known);
}

void rsx_vp_constants(u32 base, u32 rows, const f32 *values)
{
    gcmContextData *ctx = rsx_context();
    u32 r = 0;
    if (!ctx) return;
    /* The block form writes raw constant rows by index, which is what a
     * generated program needs: xf_program.h fixes the slot numbers (projection
     * at 0, texture matrices after it) precisely so the upload does not depend
     * on which program is loaded. Runs of changed rows go out as one block. */
    if (base + rows > VP_CONST_ROWS) {
        rsxLoadVertexProgramParameterBlock(ctx, base, rows, values);
        return;
    }
    while (r < rows) {
        u32 run;
        while (r < rows && s_vpc_known[base + r] &&
               memcmp(&s_vpc_shadow[(base + r) * 4], values + r * 4, 16) == 0)
            r++;
        if (r == rows)
            break;
        run = r;
        while (run < rows &&
               !(s_vpc_known[base + run] &&
                 memcmp(&s_vpc_shadow[(base + run) * 4], values + run * 4, 16) == 0)) {
            memcpy(&s_vpc_shadow[(base + run) * 4], values + run * 4, 16);
            s_vpc_known[base + run] = 1;
            run++;
        }
        rsxLoadVertexProgramParameterBlock(ctx, base + r, run - r, values + r * 4);
        r = run;
    }
}
