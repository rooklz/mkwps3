/* renderer_glue.c -- the RSX renderer as the runtime's GX sink.
 *
 * The runtime's GPU thread (gx_hw.cpp) parses the game's own GX command
 * stream and calls the sink below once per command, in order. This file turns
 * those calls into the renderer's GXState machine (BP/XF/CP state plus the
 * draw and EFB-copy backend in gx_render.c), owns the RSX display loop -- a
 * frame is presented when the game copies the EFB to the XFB -- and answers
 * the renderer's questions about guest memory. Everything here runs on the
 * GPU thread except dirty_mark, which DMA and cache-flush reports reach from
 * the game thread. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../common/types.h"
#include "../../common/log.h"
#define PHASE_PROF_IMPL
#include "../../common/phase_prof.h"
#include "../../core/gx/gx_state.h"
#include "../../core/mem/memmap.h"
#include "../../core/mem/dirty_pages.h"
#include "../../video/rsx/gx_render.h"
#include "../../video/rsx/rsx_video.h"

/* The runtime's sink interface (runtime/include/gx_hw.h), restated in C. */
struct GxSink {
    void *ctx;
    void (*load_cp)(void *ctx, uint8_t reg, uint32_t value);
    void (*load_xf)(void *ctx, uint16_t addr, const uint32_t *values, unsigned count);
    void (*load_bp)(void *ctx, uint32_t value);
    void (*load_index)(void *ctx, uint8_t op, uint16_t index, uint16_t xf_addr, uint8_t count);
    void (*draw)(void *ctx, uint8_t primitive, unsigned vat, uint16_t vertex_count,
                 const uint8_t *vertices, uint32_t vertex_size);
    void (*present)(void *ctx, uint32_t xfb, uint32_t width, uint32_t height, _Bool black);
    void (*idle)(void *ctx);
    void (*ram_written)(void *ctx, uint32_t addr, uint32_t size);
    void (*start)(void *ctx);
    void (*draw_run)(void *ctx, unsigned vat, const uint8_t *commands, uint32_t bytes, uint32_t prims,
                     uint32_t vertices, uint32_t indices, uint32_t vertex_size);
};
/* gx_hw.h asserts the same size: a member added on one side only is a call
 * through whatever lies past the end of the other's table. */
_Static_assert(sizeof(struct GxSink) == 11 * sizeof(void *), "GxSink differs from runtime/include/gx_hw.h");

extern uint8_t *MkwGuestArenaBase(void);
extern void spu_vtx_init(void);
extern void mkw_lv2_memory(uint64_t *free_bytes, uint64_t *total_bytes);

static void report_memory(const char *when)
{
    uint64_t free_bytes = 0, total_bytes = 0;
    mkw_lv2_memory(&free_bytes, &total_bytes);
    printf("[gfx] %s: %llu KiB free\n", when, (unsigned long long)(free_bytes >> 10));
}

u8 *g_renderer_arena;

/* ---- guest memory changes ---------------------------------------------- */

/* One stamp per 4 KiB of MEM1 and MEM2. A stamp is the sequence number of the
 * last change; dirty_since compares stamps against the sequence a texture was
 * decoded at. */
#define PAGE_SHIFT 12u
#define MEM1_PAGES (MKW_MEM1_BYTES >> PAGE_SHIFT)
#define MEM2_PAGES (MKW_MEM2_BYTES >> PAGE_SHIFT)

static u32 s_stamp[MEM1_PAGES + MEM2_PAGES];
static u32 s_seq = 1;

static int page_index(u32 fold, u32 *out)
{
    if (fold - MKW_MEM1_FOLD < MKW_MEM1_BYTES) { *out = (fold - MKW_MEM1_FOLD) >> PAGE_SHIFT; return 1; }
    if (fold - MKW_MEM2_FOLD < MKW_MEM2_BYTES) { *out = MEM1_PAGES + ((fold - MKW_MEM2_FOLD) >> PAGE_SHIFT); return 1; }
    return 0;
}

void dirty_mark(u32 ea, u32 len)
{
    u32 f, end, first, last, i;
    if (len == 0)
        return;
    f = mem_fold(ea);
    end = f + len - 1u;
    if (!page_index(f, &first) || !page_index(end, &last) || last < first)
        return;
    {
        u32 seq = __atomic_add_fetch(&s_seq, 1u, __ATOMIC_RELAXED);
        for (i = first; i <= last; i++)
            __atomic_store_n(&s_stamp[i], seq, __ATOMIC_RELAXED);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

u32 dirty_seq(void)
{
    return __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
}

int dirty_since(u32 ea, u32 len, u32 seq)
{
    u32 f, first, last, i;
    if (len == 0)
        return 0;
    f = mem_fold(ea);
    if (!page_index(f, &first) || !page_index(f + len - 1u, &last))
        return 0;
    for (i = first; i <= last; i++)
        if (__atomic_load_n(&s_stamp[i], __ATOMIC_RELAXED) > seq)
            return 1;
    return 0;
}

/* ---- the VI's view ------------------------------------------------------- */

static u32 s_vi_xfb;

u32 vi_current_xfb(void)
{
    return s_vi_xfb;
}

/* ---- frames -------------------------------------------------------------- */

u64 g_draw_frame_base;
static u64 s_frames;

/* Per-phase GPU-thread time, averaged over the last report window (60 presented
 * frames), for the control port's `prof`. Time not in any renderer phase is the
 * runtime's command parsing plus waiting for commands (gx_hw's busy counter
 * excludes the waits). */
static char s_prof_report[1024];
static u64 s_prof_window_frames;

static void prof_snapshot(void)
{
    static const char *const names[PH_COUNT] = {
        "other(parse+wait)", "shader_gen", "dd_pre", "gx_fifo", "gx_state", "draw_disp", "spu_job", "vertex",
        "WAIT spu", "texture", "shader", "rsx_cmd", "present", "WAIT vsync", "WAIT gpu", "consts", "lights",
        "-", "binds", "-", "-"};
    int i, k = 0;
    const double ms_per_tick = 1000.0 / 79800000.0;
    u64 frames = s_prof_window_frames ? s_prof_window_frames : 1;
    k += snprintf(s_prof_report + k, sizeof s_prof_report - k, "GPU thread, ms per frame over %llu frames:\n",
                  (unsigned long long)frames);
    for (i = 0; i < PH_COUNT && k < (int)sizeof s_prof_report - 64; i++) {
        if (!g_prof.acc[i])
            continue;
        k += snprintf(s_prof_report + k, sizeof s_prof_report - k, "  %-18s %7.2f  (%llu entries/frame)\n",
                      names[i], (double)g_prof.acc[i] * ms_per_tick / (double)frames,
                      (unsigned long long)(g_prof.cnt[i] / frames));
    }
    prof_window_close();
    s_prof_window_frames = 0;
}

/* The profiler costs two timebase reads per region, ~15,000 regions a race
 * frame: off until the control port's `prof` first asks, then on for good. */
static volatile int s_prof_wanted;

const char *mkw_renderer_profile(void)
{
    if (!s_prof_wanted) {
        s_prof_wanted = 1;
        return "profiling from the next frame; ask again in a few seconds\n";
    }
    return s_prof_report[0] ? s_prof_report : "no profile yet\n";
}

/* Control-port screenshot request (mkw_renderer_request_shot): the GPU thread
 * writes the frame it just finished, before flipping it. */
static volatile int s_shot_request;

void mkw_renderer_request_shot(void)
{
    s_shot_request = 1;
}

/* Control-port frame trace and copy-target dumps (gx_render.c), likewise
 * serviced on the GPU thread between frames. */
static volatile int s_cshot_request = -1;

void mkw_renderer_request_ftrace(unsigned from)
{
    extern int g_ftrace_arm;
    extern u32 g_ftrace_from;
    g_ftrace_from = from;
    g_ftrace_arm = 1;
}

static volatile int s_dinfo_pending;

void mkw_renderer_request_draw_info(unsigned index, unsigned texture)
{
    extern u32 g_draw_info_idx;
    extern u32 g_draw_info_tex;
    extern int g_draw_info_arm;
    g_draw_info_tex = texture;
    printf("[gfx] dinfo armed: draw %u texture %08x\n", index, texture);
    log_set_level(LOG_CORE, LOG_LEVEL_INFO);   /* the dump is INFO-level; frame_done lowers it again */
    s_dinfo_pending = 1;
    g_draw_info_idx = index;
    g_draw_info_arm = 1;
}

void mkw_renderer_set_xf_show(int mode)
{
    extern int g_xf_show_mode;
    extern int g_shader_flush_req;
    g_xf_show_mode = mode;
    g_shader_flush_req = 1;
}

void mkw_renderer_draw_window(unsigned min, unsigned max)
{
    extern u32 g_draw_win_min, g_draw_win_max;
    g_draw_win_min = min;
    g_draw_win_max = max;
}

void mkw_renderer_skip_draws(unsigned min, unsigned max)
{
    extern u32 g_draw_skip_min, g_draw_skip_max;
    g_draw_skip_min = min;
    g_draw_skip_max = max;
}

void mkw_renderer_set_tev_show(int mode)
{
    extern int g_tev_show_mode;
    extern int g_shader_flush_req;
    g_tev_show_mode = mode;
    g_shader_flush_req = 1;
}

void mkw_renderer_request_copy_dump(int index)
{
    s_cshot_request = index;
}

static void say_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void frame_done(void *ctx)
{
    (void)ctx;
    s_frames++;
    g_draw_frame_base = g_gx_render.draws;
    if (rsx_xmb_menu_open())
        return;   /* the XMB owns the screen while its in-game menu is up */
    if (s_shot_request) {
        s_shot_request = 0;
        rsx_wait_idle();
        rsx_video_screenshot("/dev_hdd0/tmp/mkwii-shot.ppm", 1u, (unsigned)g_rsx.current);
        printf("[gfx] screenshot written (frame %llu)\n", (unsigned long long)s_frames);
    }
    if (s_dinfo_pending) {
        extern int g_draw_info_arm;
        if (!g_draw_info_arm || ++s_dinfo_pending > 30) {
            /* Dumped (or the draw never came): stop the copy-by-copy INFO flood. */
            g_draw_info_arm = 0;
            s_dinfo_pending = 0;
            log_set_level(LOG_CORE, LOG_LEVEL_WARN);
        }
    }
    if (s_cshot_request >= 0) {
        const int idx = s_cshot_request;
        s_cshot_request = -1;
        gx_render_dump_copy((unsigned)idx, say_line);
    }
    prof_enter(PH_PRESENT);
    rsx_frame_end();      /* flip; paced by the display */
    rsx_frame_begin();
    rsx_clear(0xFF000000u);
    prof_exit();
    /* New frame: vertex arenas, constant caches and the texture fence. */
    gx_render_frame_begin();
    if (s_prof_wanted && !g_prof.enabled)
        prof_reset();       /* armed here so this thread owns it */
    if (g_prof.enabled && ++s_prof_window_frames >= 60)
        prof_snapshot();
}

u64 mkw_renderer_frames(void)
{
    return s_frames;
}

/* One line of renderer health for the control port's `gfx`. */
int mkw_renderer_stats(char *out, unsigned cap)
{
    extern u64 g_spu_jobs, g_spu_fallbacks, g_spu_too_big, g_spu_skin_copies, g_spu_skin_reuses;
    extern u64 g_run_draws, g_run_prims, g_run_splits, g_run_fallbacks, g_run_calls, g_run_singly;
    extern u64 g_spu_arena_waits, g_spu_arena_fails, g_ring_wraps;
    char line[256];
    int n;
    gx_render_stats_line(line, sizeof line);
    n = snprintf(out, cap, "spu jobs=%llu fallbacks=%llu toobig=%llu skin copies=%llu reuses=%llu\n"
                 "runs draws=%llu prims=%llu splits=%llu fallbacks=%llu calls=%llu singly=%llu dropped=%llu "
                 "prof depth=%d top=%d arena waits=%llu fails=%llu ring wraps=%llu\n",
                 (unsigned long long)g_spu_jobs, (unsigned long long)g_spu_fallbacks,
                 (unsigned long long)g_spu_too_big, (unsigned long long)g_spu_skin_copies,
                 (unsigned long long)g_spu_skin_reuses, (unsigned long long)g_run_draws,
                 (unsigned long long)g_run_prims, (unsigned long long)g_run_splits,
                 (unsigned long long)g_run_fallbacks, (unsigned long long)g_run_calls,
                 (unsigned long long)g_run_singly, (unsigned long long)gx_state()->draws_dropped,
                 g_prof.depth, g_prof.stack[g_prof.depth < PROF_STACK_MAX ? g_prof.depth : 0],
                 (unsigned long long)g_spu_arena_waits, (unsigned long long)g_spu_arena_fails,
                 (unsigned long long)g_ring_wraps);
    if (n < 0 || (unsigned)n >= cap)
        return n;
    out += n;
    cap -= (unsigned)n;
    {
        double rsx_ms, lag_ms, period_ms;
        int m;
        rsx_frame_timing(&rsx_ms, &lag_ms, &period_ms);
        (void)rsx_ms;
        m = snprintf(out, cap, "rsx finishes a frame %.1f ms after it is queued, one every %.1f ms\n",
                     lag_ms, period_ms);
        if (m < 0 || (unsigned)m >= cap)
            return n;
        out += m; cap -= (unsigned)m; n += m;
    }
    {
        /* Per-consumer SPU window, published every 256 jobs: jobs, ticks
         * working, ticks of that waiting on DMA, single-element gathers and
         * jobs that needed them. */
        extern int spu_vtx_consumer_stats(unsigned i, u32 *w8);
        unsigned i;
        u32 w[8];
        for (i = 0; i < 6 && spu_vtx_consumer_stats(i, w); i++) {
            int m = snprintf(out, cap, "spu%u jobs=%u work=%u dma=%u gathers=%u gjobs=%u total=%u\n", i, w[1], w[3],
                             w[4], w[6], w[7], w[5]);
            if (m < 0 || (unsigned)m >= cap)
                return n;
            out += m; cap -= (unsigned)m; n += m;
        }
    }
    {
        extern u64 g_sg_ticks[4], g_sg_count;
        const double us = 1.0 / 79.8;
        const double k = g_sg_count ? 1.0 / (double)g_sg_count : 0.0;
        int m = snprintf(out, cap, "shader builds=%llu per build us: evict %.1f vp %.1f fp %.1f upload %.1f\n",
                         (unsigned long long)g_sg_count, g_sg_ticks[0] * us * k, g_sg_ticks[1] * us * k,
                         g_sg_ticks[2] * us * k, g_sg_ticks[3] * us * k);
        if (m < 0 || (unsigned)m >= cap)
            return n;
        out += m; cap -= (unsigned)m; n += m;
    }
    {
        extern u64 g_split_skinned, g_split_tm_pn30, g_split_tm_eq;
        extern u32 g_split_last_pn, g_split_last_tm;
        int m = snprintf(out, cap, "splits skinned=%llu tm=pn+30:%llu tm=pn:%llu last pn=%u tm=%u\n",
                         (unsigned long long)g_split_skinned, (unsigned long long)g_split_tm_pn30,
                         (unsigned long long)g_split_tm_eq, g_split_last_pn, g_split_last_tm);
        if (m < 0 || (unsigned)m >= cap)
            return n;
        out += m; cap -= (unsigned)m; n += m;
    }
    {
        extern int rsx_debug_state(char *out, unsigned cap);
        extern const char *volatile g_rsx_init_step;
        int m = snprintf(out, cap, "rsx init: %s\n", g_rsx_init_step);
        if (m > 0 && (unsigned)m < cap)
            m += rsx_debug_state(out + m, cap - (unsigned)m);
        if (m < 0 || (unsigned)m >= cap)
            return n;
        out += m;
        cap -= (unsigned)m;
        n += m;
    }
    return n + snprintf(out, cap,
                    "%s\ndraws=%llu verts=%llu built=%llu failed=%llu hits=%llu nopos=%llu overflow=%llu "
                    "texdec=%llu undecodable=%llu admitfail=%llu xfbcopies=%llu texcopies=%llu resolved=%llu "
                    "alpha_unmapped=%llu ind=%llu\n",
                    line, (unsigned long long)g_gx_render.draws, (unsigned long long)g_gx_render.vertices,
                    (unsigned long long)g_gx_render.programs_built, (unsigned long long)g_gx_render.programs_failed,
                    (unsigned long long)g_gx_render.program_hits, (unsigned long long)g_gx_render.skipped_no_pos,
                    (unsigned long long)g_gx_render.overflow, (unsigned long long)g_gx_render.textures_decoded,
                    (unsigned long long)g_gx_render.texture_undecodable,
                    (unsigned long long)g_gx_render.texture_admit_fail,
                    (unsigned long long)g_gx_render.efb_copies_xfb, (unsigned long long)g_gx_render.efb_copies_texture,
                    (unsigned long long)g_gx_render.efb_copies_resolved,
                    (unsigned long long)g_gx_render.alpha_test_unmapped,
                    (unsigned long long)g_gx_render.indirect_unmapped);
}

/* ---- the sink ------------------------------------------------------------ */

static int s_ready;

static void sink_start(void *ctx)
{
    static GXBackend be;
    (void)ctx;
    g_renderer_arena = MkwGuestArenaBase();
    report_memory("before RSX init");
    if (rsx_video_init() != 0) {
        printf("[gfx] RSX init failed; rendering disabled\n");
        return;
    }
    report_memory("after RSX init");
    {
        /* The SPU derives each indexed attribute's index window itself; the
         * PPU-side scan is the fallback /dev_hdd0/tmp/dolphin-nospuscan.txt
         * selects. (The old emulator's main set this default.) */
        extern int g_spu_scan_on;
        FILE *f = fopen("/dev_hdd0/tmp/dolphin-nospuscan.txt", "r");
        g_spu_scan_on = 1;
        if (f) { fclose(f); g_spu_scan_on = 0; }
    }
    spu_vtx_init();
    report_memory("after SPU init");
    if (gx_render_init() != 0) {
        printf("[gfx] renderer init failed; rendering disabled\n");
        return;
    }
    gx_render_bind(&be);
    gx_state_init(gx_state(), &be);
    gx_render_set_frame_handler(frame_done, NULL);
    rsx_frame_begin();
    rsx_clear(0xFF000000u);
    gx_render_frame_begin();
    s_ready = 1;
    report_memory("after renderer init");
    printf("[gfx] RSX renderer up: %ux%u\n", (unsigned)g_rsx.width, (unsigned)g_rsx.height);
}

static void sink_load_cp(void *ctx, uint8_t reg, uint32_t value)
{
    (void)ctx;
    if (s_ready) {
        prof_enter(PH_GXSTATE);
        gx_state_load_cp(gx_state(), reg, value);
        prof_exit();
    }
}

static void sink_load_xf(void *ctx, uint16_t addr, const uint32_t *values, unsigned count)
{
    (void)ctx;
    if (s_ready) {
        prof_enter(PH_GXSTATE);
        gx_state_load_xf(gx_state(), addr, values, count);
        prof_exit();
    }
}

static void sink_load_bp(void *ctx, uint32_t word)
{
    (void)ctx;
    if (s_ready) {
        prof_enter(PH_GXSTATE);
        gx_state_load_bp(gx_state(), (u8)(word >> 24), word & 0x00FFFFFFu);
        prof_exit();
    }
}

static void sink_load_index(void *ctx, uint8_t op, uint16_t index, uint16_t xf_addr, uint8_t count)
{
    (void)ctx;
    if (s_ready)
        gx_state_load_index(gx_state(), op, index, xf_addr, count);
}

static void sink_draw(void *ctx, uint8_t primitive, unsigned vat, uint16_t count,
                      const uint8_t *vertices, uint32_t vertex_size)
{
    (void)ctx;
    if (!s_ready)
        return;
    /* Vertices are in guest memory: a display list, or the command ring the
     * runtime keeps inside the arena. Either way their guest address is the
     * arena offset in the cached mirror. */
    prof_enter(PH_DRAWDISP);
    gx_state_draw(gx_state(), (GXPrimitive)primitive, vat, count,
                  (u32)(vertices - g_renderer_arena) | 0x80000000u, vertex_size);
    prof_exit();
}

static void sink_draw_run(void *ctx, unsigned vat, const uint8_t *commands, uint32_t bytes,
                          uint32_t prims, uint32_t vertices, uint32_t indices, uint32_t vertex_size)
{
    (void)ctx;
    if (!s_ready)
        return;
    prof_enter(PH_DRAWDISP);
    gx_state_draw_run(gx_state(), vat, (u32)(commands - g_renderer_arena) | 0x80000000u, bytes,
                      prims, vertices, indices, vertex_size);
    prof_exit();
}

/* The runtime's FIFO parser, timed as its own region (gx_fifo) so the
 * profile separates parsing from the work outside it. */
void mkw_prof_parse_begin(void) { prof_enter(PH_GXFIFO); }
void mkw_prof_parse_end(void) { prof_exit(); }

static void sink_present(void *ctx, uint32_t xfb, uint32_t width, uint32_t height, _Bool black)
{
    (void)ctx; (void)width; (void)height; (void)black;
    s_vi_xfb = xfb;
}

static void sink_ram_written(void *ctx, uint32_t addr, uint32_t size)
{
    (void)ctx;
    dirty_mark(addr, size);
}

static const struct GxSink s_sink = {
    0,
    sink_load_cp,
    sink_load_xf,
    sink_load_bp,
    sink_load_index,
    sink_draw,
    sink_present,
    0,
    sink_ram_written,
    sink_start,
    sink_draw_run,
};

const struct GxSink *mkw_renderer_sink(void)
{
    return &s_sink;
}
