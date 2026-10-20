/* dirty_pages.h -- "has this guest memory changed since sequence N?"
 *
 * The texture cache keys decoded textures on it. Guest RAM changes the GPU
 * can see are reported by the runtime: data-cache flushes and stores (the CPU
 * must flush before the GPU reads, as on hardware), DVD and other DMA, and the
 * renderer's own EFB copies. */
#ifndef MKW_RENDERER_DIRTY_PAGES_H
#define MKW_RENDERER_DIRTY_PAGES_H

#include "../../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

void dirty_mark(u32 ea, u32 len);
u32  dirty_seq(void);
int  dirty_since(u32 ea, u32 len, u32 seq);

#ifdef __cplusplus
}
#endif

#endif
