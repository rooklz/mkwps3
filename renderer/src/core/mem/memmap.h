/* memmap.h -- guest memory as the renderer sees it.
 *
 * The renderer was written against the dolphin-ps3 memory map; in this port
 * guest memory is the runtime's 1 GiB arena (guest_flat_memory.h), where an
 * address folds with `& 0x3FFFFFFF`. Only the regions the GPU can read are
 * answered here: MEM1, MEM2 and the GX command ring the runtime keeps in the
 * arena (so FIFO vertices have guest addresses like display-list ones). */
#ifndef MKW_RENDERER_MEMMAP_H
#define MKW_RENDERER_MEMMAP_H

#include "../../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u8 *g_renderer_arena;   /* set by renderer_glue before any draw */

#define MKW_MEM1_FOLD   0x00000000u
#define MKW_MEM1_BYTES  0x01800000u
#define MKW_MEM2_FOLD   0x10000000u
#define MKW_MEM2_BYTES  0x04000000u
#define MKW_RING_FOLD   0x30000000u
#define MKW_RING_BYTES  0x00300000u

DOL_INLINE u32 mem_fold(u32 ea) { return ea & 0x3FFFFFFFu; }

/* Bytes readable from `ea` before its region ends; 0 when unbacked. */
DOL_INLINE u32 mem_valid_span(u32 ea)
{
    u32 f = mem_fold(ea);
    if (f - MKW_MEM1_FOLD < MKW_MEM1_BYTES) return MKW_MEM1_FOLD + MKW_MEM1_BYTES - f;
    if (f - MKW_MEM2_FOLD < MKW_MEM2_BYTES) return MKW_MEM2_FOLD + MKW_MEM2_BYTES - f;
    if (f - MKW_RING_FOLD < MKW_RING_BYTES) return MKW_RING_FOLD + MKW_RING_BYTES - f;
    return 0;
}

DOL_INLINE void *mem_ptr(u32 ea)
{
    return mem_valid_span(ea) ? g_renderer_arena + mem_fold(ea) : (void *)0;
}

DOL_INLINE u8 mem_read8(u32 ea)
{
    return mem_valid_span(ea) ? g_renderer_arena[mem_fold(ea)] : 0;
}

DOL_INLINE u16 mem_read16(u32 ea)
{
    return mem_valid_span(ea) >= 2 ? dol_be16(g_renderer_arena + mem_fold(ea)) : 0;
}

DOL_INLINE u32 mem_read32(u32 ea)
{
    return mem_valid_span(ea) >= 4 ? dol_be32(g_renderer_arena + mem_fold(ea)) : 0;
}

#ifdef __cplusplus
}
#endif

#endif
