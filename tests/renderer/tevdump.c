/* tevdump <bpdump.bin> <draw index> -- regenerate one draw's fragment program
 * on the host from the renderer's register dump (control port `ftrace` writes
 * bpdump.bin for orthographic draws) and print it as text.
 *
 *   make -C tests/renderer && ./tests/renderer/tevdump bpdump.bin 1 */
#include "video/rsx/tev_program.h"
#include "core/gx/bp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_ops[] = {
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DP4", "DST", "MIN", "MAX", "SLT", "SGE", "SLE",
    "SGT", "SNE", "SEQ", "FRC", "FLR", "KIL", "PK4B", "UP4B", "DDX", "DDY", "TEX", "TXP", "TXD",
    "RCP", "RSQ", "EX2", "LG2", "LIT", "LRP",
};

static void src_text(const FPSrc *s, const float *lit, char *out)
{
    static const char sw[] = "xyzw";
    unsigned type = s->word & 3u;
    char reg[32];
    if (type == FP_REG_TYPE_TEMP)
        snprintf(reg, sizeof reg, "R%u", (s->word >> FP_REG_SRC_SHIFT) & 0x3Fu);
    else if (type == FP_REG_TYPE_INPUT) {
        unsigned in = (s->word >> 24) & 0x1Fu;
        if (in >= 4) snprintf(reg, sizeof reg, "TEX%u", in - 4);
        else snprintf(reg, sizeof reg, "%s", in == 0 ? "WPOS" : in == 1 ? "COL0" : in == 2 ? "COL1" : "FOGC");
    } else if (lit)
        snprintf(reg, sizeof reg, "{%g %g %g %g}", lit[0], lit[1], lit[2], lit[3]);
    else
        snprintf(reg, sizeof reg, "CONST?");
    sprintf(out, "%s%s.%c%c%c%c", (s->word & FP_REG_NEGATE) ? "-" : "", reg,
            sw[(s->word >> FP_REG_SWZ_X_SHIFT) & 3], sw[(s->word >> FP_REG_SWZ_Y_SHIFT) & 3],
            sw[(s->word >> FP_REG_SWZ_Z_SHIFT) & 3], sw[(s->word >> FP_REG_SWZ_W_SHIFT) & 3]);
}

void fp_trace(unsigned op, unsigned dst, unsigned mask, int sat, const FPSrc *const *srcs,
              unsigned nsrc, const float *lit)
{
    unsigned i, base = op & 0xFFu;
    char m[5] = "", buf[96];
    if (mask & 1) strcat(m, "x");
    if (mask & 2) strcat(m, "y");
    if (mask & 4) strcat(m, "z");
    if (mask & 8) strcat(m, "w");
    if (base == FP_OP_TEX || base == FP_OP_TXP)
        printf("  %s%s R%u.%s, unit %u,", k_ops[base], sat ? "_SAT" : "", dst, m, op >> 8);
    else
        printf("  %s%s R%u.%s,", base < 32 ? k_ops[base] : "?", sat ? "_SAT" : "", dst, m);
    for (i = 0; i < nsrc; i++) {
        if (srcs[i]->word == fp_none().word && !srcs[i]->is_const && i > 0)
            continue;
        src_text(srcs[i], srcs[i]->is_const ? lit : 0, buf);
        printf(" %s", buf);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *data;
    long size;
    unsigned magic, n, rec, i, want;
    if (argc < 3) { fprintf(stderr, "usage: tevdump <bpdump.bin> <draw>\n"); return 1; }
    want = (unsigned)strtoul(argv[2], 0, 0);
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    data = malloc((size_t)size);
    fread(data, 1, (size_t)size, f); fclose(f);
#define BE32(p) ((unsigned)(p)[0] << 24 | (unsigned)(p)[1] << 16 | (unsigned)(p)[2] << 8 | (p)[3])
    magic = BE32(data); n = BE32(data + 4); rec = BE32(data + 8);
    if (magic != 0x42504431u) { fprintf(stderr, "not a bpdump\n"); return 1; }
    for (i = 0; i < n; i++) {
        const unsigned char *r = data + 16 + (size_t)i * rec;
        unsigned reg;
        BPState bp;
        FPEmitter e;
        TevProgramInfo info;
        static u32 code[4096];
        if (BE32(r) != want) continue;
        bp_reset(&bp);
        for (reg = 0; reg < 256; reg++) {
            /* Skip the registers that trigger work rather than hold state. */
            if (reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52 || reg == 0x64 ||
                reg == 0x65 || reg == 0x66 || reg == 0x67 || reg == 0xFE)
                continue;
            bp_write(&bp, (u8)reg, BE32(r + 8 + reg * 4) & 0xFFFFFFu);
        }
        memset(&info, 0, sizeof info);
        fp_init(&e, code, 4096);
        printf("draw %u:\n", want);
        if (tev_generate(&bp, &e, &info) != 0)
            printf("tev_generate failed\n");
        return 0;
    }
    fprintf(stderr, "draw %u not in the dump\n", want);
    return 1;
}
