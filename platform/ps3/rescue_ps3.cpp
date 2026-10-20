// Development control port (TCP 4001), compatible with the earlier
// dolphin-ps3 tooling: lets the Mac relaunch the app after replacing its
// EBOOT, push files and read logs without anyone at the console.
//
//   ping                  -> "alive"
//   relaunch              -> exit and spawn RELOAD.SELF (else EBOOT.BIN)
//   put <path> <size>     -> "GO", then <size> raw bytes; "OK" when stored
//   cat <path>            -> the file's bytes, then the connection closes
//   ctx <guest|gpu>       -> that thread's registers (PEX debug syscalls),
//                            for symbolising a fault against mkwii.elf
//   pcs <secs>            -> sample the guest's current function and LR for
//                            <secs> seconds into /dev_hdd0/tmp/pcsamples.bin
//   where                 -> the guest's current function, LR and stack
//
// The listener runs above the game threads' priority so it answers even when
// guest code spins.

#include <cstdio>
#include <cstring>
#include <net/net.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <cstdlib>
#include <sys/dbg.h>
#include <sys/file.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <unistd.h>

namespace {
extern char g_faultReport[2048];
}

namespace {

constexpr int kPort = 4001;
const char* const kSpawnTargets[2] = {
    "/dev_hdd0/game/DOLPHIN01/USRDIR/RELOAD.SELF",
    "/dev_hdd0/game/DOLPHIN01/USRDIR/EBOOT.BIN",
};

void Spawn()
{
    const char* self = kSpawnTargets[1];
    struct stat st;
    for (const char* t : kSpawnTargets) {
        if (stat(t, &st) == 0) {
            self = t;
            break;
        }
    }
    std::printf("[rescue] relaunching %s\n", self);
    std::fflush(stdout);
    const char* argv[2] = {self, nullptr};
    sysProcessExitSpawn2(self, argv, nullptr, nullptr, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
    std::printf("[rescue] spawn failed\n");
}

void SendAll(int c, const char* p, size_t n)
{
    while (n) {
        const int w = netSend(c, p, static_cast<int>(n), 0);
        if (w <= 0)
            return;
        p += w;
        n -= static_cast<size_t>(w);
    }
}

void SendText(int c, const char* s)
{
    SendAll(c, s, std::strlen(s));
}

void Put(int c, const char* args)
{
    char path[128];
    unsigned long want = 0;
    if (std::sscanf(args, "%127s %lu", path, &want) != 2 || want == 0 || want > (256ul << 20)) {
        SendText(c, "EARGS\n");
        return;
    }
    char part[140];
    ::snprintf(part, sizeof part, "%s.part", path);
    FILE* f = std::fopen(part, "wb");
    if (!f) {
        SendText(c, "EOPEN\n");
        return;
    }
    static char buf[32768];
    SendText(c, "GO\n");
    unsigned long got = 0;
    while (got < want) {
        unsigned long chunk = want - got;
        if (chunk > sizeof buf)
            chunk = sizeof buf;
        const int r = netRecv(c, buf, static_cast<int>(chunk), 0);
        if (r < 0) {
            usleep(2000);
            continue;
        }
        if (r == 0)
            break;
        std::fwrite(buf, 1, static_cast<size_t>(r), f);
        got += static_cast<unsigned long>(r);
    }
    std::fclose(f);
    if (got == want) {
        std::remove(path);
        std::rename(part, path);
        SendText(c, "OK\n");
    } else {
        std::remove(part);
        SendText(c, "SHORT\n");
    }
}

void Cat(int c, const char* path)
{
    char p[128];
    if (std::sscanf(path, "%127s", p) != 1)
        return;
    FILE* f = std::fopen(p, "rb");
    if (!f) {
        SendText(c, "ENOENT\n");
        return;
    }
    static char buf[32768];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        SendAll(c, buf, n);
    std::fclose(f);
}

// Thread ids the ctx command can read, registered by the runtime.
volatile sys_ppu_thread_t g_guestTid = 0;
volatile sys_ppu_thread_t g_gpuTid = 0;

struct CtxRequest {
    volatile int done;
    volatile s32 rc;
    sys_ppu_thread_t target;
    sys_dbg_ppu_thread_context_t ctx;
};

// The read blocks while the target is running, so it happens on a throwaway
// thread: a listener stuck in it would lose the console.
void CtxReader(void* arg)
{
    CtxRequest* q = static_cast<CtxRequest*>(arg);
    q->rc = sysDbgReadPPUThreadContext(q->target, &q->ctx);
    q->done = 1;
    sysThreadExit(0);
}

void Ctx(int c, const char* which)
{
    const sys_ppu_thread_t tid = !std::strncmp(which, "gpu", 3) ? g_gpuTid : g_guestTid;
    char out[1024];
    if (!tid) {
        SendText(c, "thread not registered\n");
        return;
    }
    CtxRequest* q = static_cast<CtxRequest*>(std::calloc(1, sizeof(CtxRequest)));
    if (!q)
        return;
    q->target = tid;
    sys_ppu_thread_t w;
    if (sysThreadCreate(&w, CtxReader, q, 1000, 0x8000, 0, const_cast<char*>("ctxrd")) != 0) {
        std::free(q);
        SendText(c, "could not start reader\n");
        return;
    }
    for (int i = 0; i < 200 && !q->done; ++i)
        usleep(10000);
    if (!q->done) {
        SendText(c, "thread is running (not stopped); request leaked\n");
        return;
    }
    if (q->rc != 0) {
        ::snprintf(out, sizeof out, "read failed rc=%d\n", static_cast<int>(q->rc));
        SendText(c, out);
        std::free(q);
        return;
    }
    const sys_dbg_ppu_thread_context_t& x = q->ctx;
    int k = ::snprintf(out, sizeof out, "pc=%016llx lr=%016llx ctr=%016llx cr=%08x\n",
                       static_cast<unsigned long long>(x.pc), static_cast<unsigned long long>(x.lr),
                       static_cast<unsigned long long>(x.ctr), static_cast<unsigned>(x.cr));
    for (int g = 0; g < 32 && k < static_cast<int>(sizeof out) - 64; g += 4)
        k += ::snprintf(out + k, sizeof out - k, "r%-2d %016llx %016llx %016llx %016llx\n", g,
                        static_cast<unsigned long long>(x.gpr[g]), static_cast<unsigned long long>(x.gpr[g + 1]),
                        static_cast<unsigned long long>(x.gpr[g + 2]), static_cast<unsigned long long>(x.gpr[g + 3]));
    SendText(c, out);
    std::free(q);
}

// ---- guest sampler ------------------------------------------------------
//
// PEX's sys_dbg_stop_threads refuses to stop threads of the calling process,
// so this samples what the runtime publishes instead: the translated function
// it last dispatched (guest address) and the live guest register file's LR.
// Reading them needs no stop; a sample is a few loads.

extern "C" int mkw_guest_sample(uint32_t* func, uint32_t* lr, uint32_t* sp);
extern "C" int mkw_guest_stack(uint32_t* out, unsigned cap);
extern "C" int mkw_bench(char* out, unsigned cap);
extern "C" int mkw_where(char* out, unsigned cap);
extern "C" void mkw_renderer_request_shot(void);
extern "C" int mkw_renderer_stats(char* out, unsigned cap);
extern "C" const char* mkw_renderer_profile(void);
extern "C" void mkw_pad_inject(uint32_t buttons, int reads, int stickX, int stickY);

constexpr uint32_t kPcsSlots = 1u << 16;
struct PcSlot {
    uint32_t pc, n;
};
PcSlot* g_pcs[2];
uint32_t g_pcsTotal[2], g_pcsDrop[2];
unsigned g_pcsSecs = 10;
volatile int g_pcsBusy = 0;

inline uint64_t Timebase()
{
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

void PcsAdd(int t, uint32_t pc)
{
    uint32_t i = (pc * 2654435761u) >> 16;
    for (unsigned k = 0; k < 32; ++k, i = (i + 1) & (kPcsSlots - 1)) {
        PcSlot& e = g_pcs[t][i];
        if (e.pc == pc) {
            ++e.n;
            return;
        }
        if (!e.n) {
            e.pc = pc;
            e.n = 1;
            return;
        }
    }
    ++g_pcsDrop[t];
}

// Raw stack samples follow the hashed tables: {count, words...} each.
constexpr unsigned kStackWords = 18;
constexpr uint32_t kStackBufWords = 600000;
uint32_t* g_pcsStacks;
uint32_t g_pcsStackUsed;

void PcsThread(void*)
{
    const uint64_t end = Timebase() + static_cast<uint64_t>(g_pcsSecs) * 79800000ull;
    while (Timebase() < end) {
        uint32_t st[kStackWords];
        const int n = mkw_guest_stack(st, kStackWords);
        if (n >= 2) {
            PcsAdd(0, st[0]);
            PcsAdd(1, st[1]);
            ++g_pcsTotal[0];
            ++g_pcsTotal[1];
            if (g_pcsStacks && g_pcsStackUsed + 1 + n <= kStackBufWords) {
                g_pcsStacks[g_pcsStackUsed++] = static_cast<uint32_t>(n);
                std::memcpy(g_pcsStacks + g_pcsStackUsed, st, n * 4u);
                g_pcsStackUsed += n;
            }
        }
        usleep(200);
    }
    if (FILE* f = std::fopen("/dev_hdd0/tmp/pcsamples.bin", "wb")) {
        const uint32_t hdr[8] = {0x50435334u, 0, g_pcsTotal[0], g_pcsTotal[1], 0, 0, g_pcsDrop[0], g_pcsDrop[1]};
        std::fwrite(hdr, sizeof hdr, 1, f);
        for (int t = 0; t < 2; ++t) {
            uint32_t n = 0;
            for (uint32_t i = 0; i < kPcsSlots; ++i)
                n += g_pcs[t][i].n ? 1 : 0;
            std::fwrite(&n, 4, 1, f);
            for (uint32_t i = 0; i < kPcsSlots; ++i)
                if (g_pcs[t][i].n)
                    std::fwrite(&g_pcs[t][i], sizeof(PcSlot), 1, f);
        }
        std::fwrite(&g_pcsStackUsed, 4, 1, f);
        if (g_pcsStacks)
            std::fwrite(g_pcsStacks, 4, g_pcsStackUsed, f);
        std::fclose(f);
    }
    std::free(g_pcsStacks);
    g_pcsStacks = nullptr;
    g_pcsBusy = 0;
    sysThreadExit(0);
}

void PcsStart(int c, const char* args)
{
    if (g_pcsBusy) {
        SendText(c, "sampler already running\n");
        return;
    }
    unsigned secs = 10;
    std::sscanf(args, "%u", &secs);
    for (int t = 0; t < 2; ++t) {
        if (!g_pcs[t])
            g_pcs[t] = static_cast<PcSlot*>(std::malloc(kPcsSlots * sizeof(PcSlot)));
        if (!g_pcs[t]) {
            SendText(c, "out of memory\n");
            return;
        }
        std::memset(g_pcs[t], 0, kPcsSlots * sizeof(PcSlot));
        g_pcsTotal[t] = g_pcsDrop[t] = 0;
    }
    g_pcsSecs = secs ? secs : 10;
    g_pcsStackUsed = 0;
    g_pcsStacks = static_cast<uint32_t*>(std::malloc(kStackBufWords * 4u));
    g_pcsBusy = 1;
    sys_ppu_thread_t t;
    if (sysThreadCreate(&t, PcsThread, nullptr, 400, 0x8000, 0, const_cast<char*>("pcsample")) != 0) {
        g_pcsBusy = 0;
        SendText(c, "could not start sampler\n");
        return;
    }
    SendText(c, "sampling\n");
}

void Where(int c)
{
    char out[512];
    mkw_where(out, sizeof out);
    SendText(c, out);
}

void Listener(void*)
{
    sysModuleLoad(SYSMODULE_NET);
    netInitialize();
    const int ls = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) {
        sysThreadExit(0);
        return;
    }
    int one = 1;
    netSetSockOpt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (netBind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || netListen(ls, 2) < 0) {
        netClose(ls);
        sysThreadExit(0);
        return;
    }
    std::printf("[rescue] listening on port %d\n", kPort);
    for (;;) {
        const int c = netAccept(ls, nullptr, nullptr);
        if (c < 0) {
            usleep(200000);
            continue;
        }
        char cmd[192];
        const int n = netRecv(c, cmd, static_cast<int>(sizeof cmd) - 1, 0);
        if (n > 0) {
            cmd[n] = 0;
            if (!std::strncmp(cmd, "relaunch", 8)) {
                SendText(c, "spawning\n");
                netClose(c);
                Spawn();
                continue;
            }
            if (!std::strncmp(cmd, "ping", 4))
                SendText(c, "alive\n");
            else if (!std::strncmp(cmd, "put ", 4))
                Put(c, cmd + 4);
            else if (!std::strncmp(cmd, "cat ", 4))
                Cat(c, cmd + 4);
            else if (!std::strncmp(cmd, "ctx", 3))
                Ctx(c, cmd[3] ? cmd + 4 : "guest");
            else if (!std::strncmp(cmd, "pcs", 3))
                PcsStart(c, cmd[3] ? cmd + 4 : "");
            else if (!std::strncmp(cmd, "where", 5))
                Where(c);
            else if (!std::strncmp(cmd, "pad ", 4)) {
                unsigned buttons = 0;
                int reads = 6, sx = 0, sy = 0;
                std::sscanf(cmd + 4, "%x %d %d %d", &buttons, &reads, &sx, &sy);
                mkw_pad_inject(buttons, reads, sx, sy);
                SendText(c, "ok\n");
            } else if (!std::strncmp(cmd, "prof", 4)) {
                SendText(c, mkw_renderer_profile());
            } else if (!std::strncmp(cmd, "gfx", 3)) {
                char out[2048];
                mkw_renderer_stats(out, sizeof out);
                SendText(c, out);
            } else if (!std::strncmp(cmd, "bench", 5)) {
                char out[512];
                mkw_bench(out, sizeof out);
                SendText(c, out);
            } else if (!std::strncmp(cmd, "shot", 4)) {
                mkw_renderer_request_shot();
                SendText(c, "requested\n");
            } else if (!std::strncmp(cmd, "fault", 5))
                SendText(c, g_faultReport[0] ? g_faultReport : "no fault\n");
            else
                SendText(c, "commands: ping, relaunch, put <path> <size>, cat <path>, ctx <guest|gpu>, pcs <secs>, where, fault, shot, gfx, prof, pad <hex> <reads> [x y], bench\n");
        }
        netClose(c);
    }
}

} // namespace

// The guest asked to leave (os_power.cpp): back to the XMB, or relaunched.
extern "C" void mkw_platform_quit(int restart)
{
    if (restart)
        Spawn();
    std::exit(0);
}

extern "C" void mkw_rescue_register_thread(int gpu)
{
    sys_ppu_thread_t self = 0;
    sysThreadGetId(&self);
    if (gpu)
        g_gpuTid = self;
    else
        g_guestTid = self;
}

extern "C" void mkw_rescue_start(void)
{
    sys_ppu_thread_t t;
    sysThreadCreate(&t, Listener, nullptr, 500, 0x8000, 0, const_cast<char*>("rescue"));
}

// ---- XMB events ------------------------------------------------------------
//
// "Quit Game" must end the process, and while the in-game XMB menu is open the
// renderer stops flipping so the system can draw its overlay.

#include <sysutil/sysutil.h>

namespace {
volatile int g_xmbMenuOpen = 0;
volatile int g_exitRequested = 0;

void SysutilCallback(u64 status, u64, void*)
{
    switch (status) {
    case SYSUTIL_EXIT_GAME: g_exitRequested = 1; break;
    case SYSUTIL_MENU_OPEN: g_xmbMenuOpen = 1; break;
    case SYSUTIL_MENU_CLOSE: g_xmbMenuOpen = 0; break;
    default: break;
    }
}
} // namespace

extern "C" void mkw_sysutil_start(void)
{
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, SysutilCallback, nullptr);
}

// Services queued system events; returns non-zero once the user quit.
extern "C" int mkw_sysutil_poll(void)
{
    sysUtilCheckCallback();
    return g_exitRequested;
}

extern "C" int rsx_xmb_menu_open(void)
{
    return g_xmbMenuOpen;
}

// ---- fault reports ---------------------------------------------------------
//
// lv2 leaves a faulting PPU thread re-executing the faulting access, which
// looks exactly like a hang. PEX firmware keeps the debug syscalls, and a
// process may handle its own threads' exceptions: the faulting thread stops,
// this handler runs on lv2's handler thread, reads its registers and logs
// them (symbolise pc/lr against mkwii.elf). The other threads -- the control
// port above all -- keep running.

namespace {
char g_faultReport[2048];

void FaultHandler(u64 cause, sys_ppu_thread_t tid, u64 dar)
{
    sys_dbg_ppu_thread_context_t ctx;
    char name[32] = {0};
    sysDbgGetPPUThreadName(tid, name);
    int k = ::snprintf(g_faultReport, sizeof g_faultReport,
                       "[fault] thread %llu (%s) cause=%016llx dar=%016llx\n",
                       static_cast<unsigned long long>(tid), name, static_cast<unsigned long long>(cause),
                       static_cast<unsigned long long>(dar));
    if (sysDbgReadPPUThreadContext(tid, &ctx) == 0) {
        k += ::snprintf(g_faultReport + k, sizeof g_faultReport - k,
                        "[fault] pc=%016llx lr=%016llx ctr=%016llx cr=%08x\n",
                        static_cast<unsigned long long>(ctx.pc), static_cast<unsigned long long>(ctx.lr),
                        static_cast<unsigned long long>(ctx.ctr), static_cast<unsigned>(ctx.cr));
        for (int g = 0; g < 32 && k < static_cast<int>(sizeof g_faultReport) - 96; g += 4)
            k += ::snprintf(g_faultReport + k, sizeof g_faultReport - k,
                            "[fault] r%-2d %016llx %016llx %016llx %016llx\n", g,
                            static_cast<unsigned long long>(ctx.gpr[g]), static_cast<unsigned long long>(ctx.gpr[g + 1]),
                            static_cast<unsigned long long>(ctx.gpr[g + 2]),
                            static_cast<unsigned long long>(ctx.gpr[g + 3]));
    }
    char where[256];
    mkw_where(where, sizeof where);
    k += ::snprintf(g_faultReport + k, sizeof g_faultReport - k, "[fault] guest %s", where);
    std::fputs(g_faultReport, stdout);
    std::fflush(stdout);
}
} // namespace

void FaultSetupThread(void*)
{
    // Flags: keep the other PPU threads and the SPU groups running.
    std::printf("[fault] registering exception handler\n");
    const s32 init = sysDbgInitializePPUExceptionHandler(400);
    std::printf("[fault] initialize -> %d\n", static_cast<int>(init));
    const s32 reg = init == 0 ? sysDbgRegisterPPUExceptionHandler(FaultHandler, 3) : -1;
    std::printf("[fault] register -> %d\n", static_cast<int>(reg));
    sysThreadExit(0);
}

extern "C" void mkw_fault_handler_start(void)
{
    // Registration has been seen to block; it gets its own thread so the app
    // starts regardless. /dev_hdd0/tmp/mkwii-nofault.txt skips it.
    if (FILE* f = std::fopen("/dev_hdd0/tmp/mkwii-nofault.txt", "r")) {
        std::fclose(f);
        return;
    }
    sys_ppu_thread_t t;
    sysThreadCreate(&t, FaultSetupThread, nullptr, 450, 0x10000, 0, const_cast<char*>("faultsetup"));
}

extern "C" const char* mkw_last_fault(void)
{
    return g_faultReport;
}
