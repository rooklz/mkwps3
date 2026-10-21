/* vpdump <dinfo-state.bin> -- rebuild one draw's vertex program on the host from
 * the state the control port's `dinfo` writes, and print it as text.
 *
 *   make -C tests/renderer vpdump && ./tests/renderer/vpdump dinfo-state.bin */
#include "core/gx/gx_state.h"
#include "video/rsx/xf_program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern unsigned g_gx_state_mask;
extern int g_xf_mirror;
unsigned char *g_renderer_arena;   /* gx_state.c reads indexed loads through it; unused here */

static const char *const k_vec[] = {
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT", "SGE",
    "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", "ARR", "ARA", "TXL",
};
static const char *const k_sca[] = {
    "NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL", "CLI", "RET",
    "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP",
};

typedef struct { int scalar; unsigned op, dst, mask; int to_output, sat; VPSrc s[3]; unsigned n; } Insn;
static Insn s_prog[1024];
static unsigned s_nprog;

void vp_trace(int scalar, unsigned op, unsigned dst, int to_output, unsigned mask, int sat,
              const VPSrc *srcs, unsigned nsrc)
{
    if (s_nprog < 1024) {
        Insn *in = &s_prog[s_nprog++];
        unsigned k;
        in->scalar = scalar; in->op = op; in->dst = dst; in->mask = mask;
        in->to_output = to_output; in->sat = sat; in->n = nsrc;
        for (k = 0; k < nsrc; k++) in->s[k] = srcs[k];
    }
    static const char sw[] = "xyzw";
    char m[5] = "";
    unsigned i;
    const char *name = scalar ? (op < 21 ? k_sca[op] : "?") : (op < 26 ? k_vec[op] : "?");
    if (mask & 8) strcat(m, "x");
    if (mask & 4) strcat(m, "y");
    if (mask & 2) strcat(m, "z");
    if (mask & 1) strcat(m, "w");
    printf("  %s%s %s%u.%s,", name, sat ? "_SAT" : "", to_output ? "o" : "R", dst, m);
    for (i = 0; i < nsrc; i++) {
        const VPSrc *s = &srcs[i];
        unsigned w = s->word, type = w & 3u, neg = (w >> 16) & 1u;
        if (type == VP_SRC_REG_TYPE_INPUT && !s->is_input)
            continue;   /* vp_none() */
        if (s->is_input) printf(" %sv%u", neg ? "-" : "", s->input_index);
        else if (s->is_const) printf(" %sc[%s%u]", neg ? "-" : "", s->rel ? "A0+" : "", s->const_index);
        else printf(" %sR%u", neg ? "-" : "", (w >> 2) & 0x3Fu);
        printf(".%c%c%c%c", sw[(w >> 14) & 3], sw[(w >> 12) & 3], sw[(w >> 10) & 3], sw[(w >> 8) & 3]);
    }
    printf("\n");
}


/* ---- a small interpreter for the ops the generator emits ---------------- */
static float C[512][4], V[16][4], R[32][4], O[16][4];

static void fetch(const VPSrc *s, float out[4])
{
    unsigned w = s->word, k;
    const float *src;
    float tmp[4];
    if (s->is_input) src = V[s->input_index];
    else if (s->is_const) src = C[s->const_index];
    else src = R[(w >> 2) & 0x3Fu];
    for (k = 0; k < 4; k++) tmp[k] = src[k];
    out[0] = tmp[(w >> 14) & 3]; out[1] = tmp[(w >> 12) & 3];
    out[2] = tmp[(w >> 10) & 3]; out[3] = tmp[(w >> 8) & 3];
    if ((w >> 16) & 1u) for (k = 0; k < 4; k++) out[k] = -out[k];
}

static float sat1(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void run(void)
{
    unsigned i, k;
    for (i = 0; i < s_nprog; i++) {
        const Insn *in = &s_prog[i];
        float a[4], b[4], c[4], r[4];
        float *dst = in->to_output ? O[in->dst] : R[in->dst];
        unsigned m = in->mask;
        fetch(&in->s[0], a);
        if (in->n > 1) fetch(&in->s[1], b);
        if (in->n > 2) fetch(&in->s[2], c);
        if (in->scalar) {
            float x = a[0];
            float v = in->op == 2 ? 1.0f / x : in->op == 4 ? 1.0f / sqrtf(x) : in->op == 1 ? x : 0.0f;
            for (k = 0; k < 4; k++) r[k] = v;
        } else {
            for (k = 0; k < 4; k++) {
                switch (in->op) {
                case 1: r[k] = a[k]; break;
                case 2: r[k] = a[k] * b[k]; break;
                case 3: r[k] = a[k] + b[k]; break;
                case 4: r[k] = a[k] * b[k] + c[k]; break;
                case 5: r[k] = a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; break;
                case 7: r[k] = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]; break;
                case 10: r[k] = a[k] > b[k] ? a[k] : b[k]; break;
                case 9: r[k] = a[k] < b[k] ? a[k] : b[k]; break;
                case 16: r[k] = a[k] == b[k] ? 1.0f : 0.0f; break;
                case 14: r[k] = a[k] - floorf(a[k]); break;
                default: r[k] = 0; break;
                }
            }
        }
        if (in->sat) for (k = 0; k < 4; k++) r[k] = sat1(r[k]);
        if (m & 8) dst[0] = r[0];
        if (m & 4) dst[1] = r[1];
        if (m & 2) dst[2] = r[2];
        if (m & 1) dst[3] = r[3];
    }
}

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(int argc, char **argv)
{
    static GXState g;
    static u32 code[8192];
    FILE *f;
    unsigned char *d;
    long size;
    unsigned off, i, xfn;
    VPEmitter e;
    XFProgramInfo info;
    if (argc < 2) { fprintf(stderr, "usage: vpdump <dinfo-state.bin>\n"); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc((size_t)size); fread(d, 1, (size_t)size, f); fclose(f);
    if (be32(d) != 0x58465331u) { fprintf(stderr, "not a dinfo state dump\n"); return 1; }
    xfn = be32(d + 4);
    g_gx_state_mask = be32(d + 8);
    g_xf_mirror = (int)be32(d + 12);
    off = 24;
    for (i = 0; i < xfn && i < XF_MEM_SIZE; i++) {
        u32 v = be32(d + off + i * 4);
        gx_state_load_xf(&g, (u16)i, &v, 1);
    }
    off += xfn * 4;
    for (i = 0; i < 256; i++) {
        if (i == 0x45 || i == 0x47 || i == 0x48 || i == 0x52 || i == 0x64 || i == 0x65 ||
            i == 0x66 || i == 0x67 || i == 0xFE)
            continue;
        bp_write(&g.bp, (u8)i, be32(d + off + i * 4) & 0xFFFFFFu);
    }
    off += 256 * 4;
    {
        u32 *cpw = (u32 *)&g.parser.cp;
        for (i = 0; i < sizeof g.parser.cp / 4; i++)
            cpw[i] = be32(d + off + i * 4);
        off += (unsigned)sizeof g.parser.cp;
    }
    printf("mirror=%d mask=%08x vcd=%08x/%08x\n", g_xf_mirror, g_gx_state_mask,
           g.parser.cp.vcd_lo, g.parser.cp.vcd_hi);
    vp_init(&e, code, 8192);
    if (xf_generate(&g, &e, &info) != 0)
        printf("xf_generate failed\n");
    printf("inputs %08x outputs %08x regs %u\n", e.input_mask, e.output_mask, e.num_regs);

    /* Execute it once for a representative skinned vertex (view space already). */
    {
        unsigned li, r;
        float L[20], MC[8], AC[8];
        for (r = 0; r < 4; r++) { C[0 + r][r] = 1; C[36 + r][r] = 1; C[39 + r][r] = 1; }
        for (li = 0; li < 8; li++) {
            xf_light_constants(&g.xf, li, L);
            for (r = 0; r < 5; r++) memcpy(C[66 + li * 5 + r], &L[r * 4], 16);
        }
        xf_material_constants(&g.xf, MC, AC);
        memcpy(C[106], MC, 16); memcpy(C[107], MC + 4, 16);
        memcpy(C[108], AC, 16); memcpy(C[109], AC + 4, 16);
        C[110][0] = 0; C[110][1] = 1; C[110][2] = 0.5f; C[110][3] = 2;
        V[0][0] = -603.575f; V[0][1] = -42.9292f; V[0][2] = -152.177f; V[0][3] = 1;
        V[2][0] = 0.912296f; V[2][1] = 0.408733f; V[2][2] = -0.0255131f; V[2][3] = 0;
        V[3][0] = 0.90f; V[3][1] = 0.905f; V[3][2] = 0.90f; V[3][3] = 1;
        run();
        printf("light colour constant c[%u] = {%g %g %g %g}\n", 66 + 2 * 5, C[76][0], C[76][1], C[76][2], C[76][3]);
        printf("o1 (COL0) = {%g %g %g %g}   R8 = {%g %g %g %g}   R2 = {%g %g %g %g}\n",
               O[1][0], O[1][1], O[1][2], O[1][3], R[8][0], R[8][1], R[8][2], R[8][3],
               R[2][0], R[2][1], R[2][2], R[2][3]);
    }
    return 0;
}
