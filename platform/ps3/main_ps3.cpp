// PS3 entry point: replaces upstream's main.cpp (Windows + aurora).
//
// Boot order follows upstream's RuntimeMain: guest memory and data sections,
// static constructors, the translated-function registry, then the guest entry
// point on the fiber scheduler. The GX front end comes up before the guest
// runs so GXInit finds its registers. The guest runs on its own PPU thread
// with a large stack: translated code nests one host frame per guest call.

#include "abi_bridge.h"
#include "fiber_manager.h"
#include "gx_hw.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "system_bridge.h"
#include "hle_stubs.h"
#include "guest_flat_memory.h"
#include "recomp_mod_loader.h"
#include "audio_backend.h"
#include "runtime_config.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <string_view>

extern "C" int mkw_lv2_thread_create(uint64_t* id, void (*entry)(void*), void* arg, int priority,
                                     uint64_t stack, const char* name);
extern "C" int mkw_lv2_thread_join(uint64_t id);
extern "C" void mkw_lv2_thread_exit(void);
extern "C" void mkw_log_streams_set(FILE* out, FILE* err);
extern "C" void mkw_lv2_memory(uint64_t* freeBytes, uint64_t* totalBytes);
extern "C" void mkw_lv2_thread_name(char* out, unsigned cap);
extern "C" void mkw_lv2_usleep(uint32_t us);
extern "C" void mkw_rescue_start(void);
extern "C" void mkw_fault_handler_start(void);
extern "C" void mkw_rescue_register_thread(int gpu);
extern "C" void mkw_hprof_attach(int which);
extern "C" void mkw_sysutil_start(void);
extern "C" int mkw_sysutil_poll(void);
extern "C" const GxSink* mkw_renderer_sink(void);
extern "C" unsigned long long mkw_renderer_frames(void);
extern bool g_fixedRandomSeeds;
extern uint32_t g_fixedSeedReads;

namespace {

extern volatile uint32_t* g_guestExecAddr;
extern volatile uint32_t* g_guestEntries;
extern volatile uint64_t* g_guestInstructions;
extern "C" void mkw_mix_ticks(uint64_t* work, uint64_t* join, uint64_t* voices, uint64_t* spu);
extern "C" uint64_t g_audioPollTicks, g_reverbTicks;
extern "C" uint32_t g_reverbCalls, g_reverbFallbacks;
extern "C" void mkw_mix_spu_waits(uint64_t* pb, uint64_t* samples);
extern "C" void mkw_rescue_set_app_dir(const char* dir);
std::atomic<bool> g_fatalReported{false};
std::atomic<int> g_exitCode{0};
std::atomic<bool> g_guestDone{false};

constexpr const char* kLogPath = "/dev_hdd0/tmp/mkwii-ps3.log";
constexpr const char* kErrPath = "/dev_hdd0/tmp/mkwii-ps3.err";

void OpenLog()
{
    // Two files: lv2 ignores O_APPEND, so two streams sharing one file write
    // over each other. Unbuffered, so a fault loses nothing.
    if (std::freopen(kLogPath, "w", stdout))
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (std::freopen(kErrPath, "w", stderr))
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    mkw_log_streams_set(stdout, stderr);
}

bool FlagFile(const char* path)
{
    FILE* f = std::fopen(path, "r");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

// /dev_hdd0/tmp/mkwii-norender.txt: parse the GX stream but draw nothing.
bool NoRendererFlag()
{
    return FlagFile("/dev_hdd0/tmp/mkwii-norender.txt");
}

void GuestMain(void*)
{
    mkw_rescue_register_thread(0);
    mkw_hprof_attach(1);
    g_guestExecAddr = &RecompMod::g_currentTranslatedExecutionAddress;
    g_guestEntries = &RecompMod::g_translatedEntries;
    g_guestInstructions = &RecompMod::g_guestInstructions;
    try {
        SystemBridge::Initialize();
        TranslatedFunctionRegistry::Finalize();
        {
            uint64_t freeBytes = 0, totalBytes = 0;
            mkw_lv2_memory(&freeBytes, &totalBytes);
            std::printf("[main] after guest memory and data init: %llu KiB free\n",
                        static_cast<unsigned long long>(freeBytes >> 10));
        }
        // /dev_hdd0/tmp/mkwii-fixedseed.txt: the same random seeds every run, so
        // tools/bench-gp.sh races the same race (ppc_helpers.cpp).
        g_fixedRandomSeeds = FlagFile("/dev_hdd0/tmp/mkwii-fixedseed.txt");
        if (!NoRendererFlag())
            GxHw::SetSink(mkw_renderer_sink());
        GxHw::Initialize();

        const TranslatedFunctionInfo* entry = TranslatedFunctionRegistry::FindByAddressPtr(kDefaultEntryAddress);
        if (!entry)
            throw std::runtime_error("no translated function at the guest entry point");

        InitializePersistentCpuContext();
        CpuContext& cpu = GetPersistentCpuContext();
        cpu.gpr[1] = 0x81700000u;
        Fiber::GuestFiberManager::Initialize();
        CpuContextScope scope(&cpu);

        std::printf("[main] entering guest at %08x\n", entry->address);
        std::fflush(stdout);
        InvokeIndirectCpu(entry->address, &cpu);
        std::printf("[main] guest returned r3=%08x\n", cpu.gpr[3]);
        Fiber::GuestFiberManager::Shutdown();
    } catch (const Memory::AccessViolation& ex) {
        std::printf("[main] guest memory access violation: %s\n", ex.what());
        SystemBridge::DumpCpuState(TryGetCpuContext());
        g_exitCode = 1;
    } catch (const std::exception& ex) {
        std::printf("[main] runtime exception: %s\n", ex.what());
        SystemBridge::DumpCpuState(TryGetCpuContext());
        g_exitCode = 1;
    }
    std::fflush(stdout);
    g_guestDone = true;
    mkw_lv2_thread_exit();
}

void ReportStats()
{
    static GxHw::Stats last{};
    static unsigned long long lastFrames = 0;
    const GxHw::Stats s = GxHw::GetStats();
    // Without a renderer nothing presents; the game's two draw-done finishes a
    // frame stand in for the count.
    static unsigned long long proxyFrames = 0;
    proxyFrames += (s.finishes - last.finishes) / 2;
    const unsigned long long frames = mkw_renderer_frames() ? mkw_renderer_frames() : proxyFrames;
    const double fps = static_cast<double>(frames - lastFrames) / 5.0;
    const double idleMs = static_cast<double>(s.guestIdleNanos - last.guestIdleNanos) / 1e6;
    // The window is 5 s. A 60 Hz game has 16.7 ms a frame; busy% of 100
    // means the game thread never waited for the next retrace.
    std::printf("[stats] %.1f fps, game thread busy %.0f%% (%.1f ms per 60 Hz frame); ", fps,
                100.0 * (5000.0 - idleMs) / 5000.0, (5000.0 - idleMs) / 300.0);
    {
        // Translated function entries on the guest thread, per presented frame.
        static uint32_t lastEntries = 0;
        const uint32_t entries = g_guestEntries ? *g_guestEntries : 0;
        const unsigned long long nf = frames - lastFrames;
        // Zero unless the translated code counts guest instructions (MKW_COUNT_INSNS).
        static uint64_t lastInstructions = 0;
        const uint64_t instructions = g_guestInstructions ? *g_guestInstructions : 0;
        if (instructions != lastInstructions && frames != lastFrames)
            std::printf("insns %llu/frame; ", static_cast<unsigned long long>(
                (instructions - lastInstructions) / (frames - lastFrames)));
        lastInstructions = instructions;
        // Zero unless the translated code counts entries (MKW_COUNT_CALLS).
        if (entries != lastEntries) {
            std::printf("calls %llu/frame; ", nf ? static_cast<unsigned long long>(entries - lastEntries) / nf : 0ull);
            std::printf("%.3f us/call; ", (5000.0 - idleMs) * 1000.0 / static_cast<double>(entries - lastEntries));
        }
        lastEntries = entries;
    }
    lastFrames = frames;
    {
        // AX mix time on the PPU (whichever thread mixes), the guest thread's waits for a
        // mix worker or the SPU, the SPU's own time, and voices mixed, per frame.
        static uint64_t lastWork = 0, lastJoin = 0, lastVoices = 0, lastSpu = 0, lastPb = 0, lastSmp = 0;
        static unsigned long long lastMixFrames = 0;
        uint64_t work = 0, join = 0, voices = 0, spu = 0, pb = 0, smp = 0;
        mkw_mix_ticks(&work, &join, &voices, &spu);
        mkw_mix_spu_waits(&pb, &smp);
        if (frames != lastMixFrames) {
            const double n = static_cast<double>(frames - lastMixFrames);
            auto ms = [n](uint64_t ticks) { return static_cast<double>(ticks) / 79800.0 / n; };
            std::printf("mix %.2f ms (wait %.2f, spu %.2f: pb %.2f samples %.2f), %.0f voices/frame; ",
                        ms(work - lastWork), ms(join - lastJoin), ms(spu - lastSpu), ms(pb - lastPb),
                        ms(smp - lastSmp), static_cast<double>(voices - lastVoices) / n);
            // The audio pump runs in the scheduler's idle loop, so the busy figure above
            // leaves it out; the reverb is part of it.
            static uint64_t lastPoll = 0, lastReverb = 0;
            static uint32_t lastReverbCalls = 0, lastFallbacks = 0;
            std::printf("audio pump %.2f ms (reverb %.2f in %.1f calls, %u translated); ",
                        ms(g_audioPollTicks - lastPoll), ms(g_reverbTicks - lastReverb),
                        static_cast<double>(g_reverbCalls - lastReverbCalls) / n,
                        g_reverbFallbacks - lastFallbacks);
            lastPoll = g_audioPollTicks;
            lastReverb = g_reverbTicks;
            lastReverbCalls = g_reverbCalls;
            lastFallbacks = g_reverbFallbacks;
        }
        lastWork = work;
        lastJoin = join;
        lastVoices = voices;
        lastSpu = spu;
        lastPb = pb;
        lastSmp = smp;
        lastMixFrames = frames;
    }
    {
        uint64_t freeBytes = 0, totalBytes = 0;
        mkw_lv2_memory(&freeBytes, &totalBytes);
        std::printf("mem free %llu KiB; ", static_cast<unsigned long long>(freeBytes >> 10));
    }
    std::printf("fifo %llu KiB parsed %llu KiB (lists %llu KiB, %llu calls) draws %llu verts %llu "
                "finish %llu tokens %llu stalls %llu unknown %llu gpu %llu ms\n",
                static_cast<unsigned long long>((s.bytesWritten - last.bytesWritten) >> 10),
                static_cast<unsigned long long>((s.bytesParsed - last.bytesParsed) >> 10),
                static_cast<unsigned long long>((s.listBytesParsed - last.listBytesParsed) >> 10),
                static_cast<unsigned long long>(s.listCalls - last.listCalls),
                static_cast<unsigned long long>(s.draws - last.draws),
                static_cast<unsigned long long>(s.vertices - last.vertices),
                static_cast<unsigned long long>(s.finishes - last.finishes),
                static_cast<unsigned long long>(s.tokens - last.tokens),
                static_cast<unsigned long long>(s.producerStalls - last.producerStalls),
                static_cast<unsigned long long>(s.unknownOpcodes),
                static_cast<unsigned long long>((s.gpuBusyNanos - last.gpuBusyNanos) / 1000000ull));
    last = s;
}

} // namespace

// ---- guest sampling for the control port -----------------------------------
//
// The translated-execution address is thread_local; the guest thread publishes
// where its copy lives so the sampler can read it from another thread.

namespace {
volatile uint32_t* g_guestExecAddr = nullptr;
volatile uint32_t* g_guestEntries = nullptr;
volatile uint64_t* g_guestInstructions = nullptr;
}

extern "C" int mkw_guest_sample(uint32_t* func, uint32_t* lr, uint32_t* sp)
{
    const CpuContext* cpu = TryGetCpuContext();
    if (!g_guestExecAddr)
        return 0;
    *func = *g_guestExecAddr;
    *lr = cpu ? cpu->lr : 0;
    *sp = cpu ? cpu->gpr[1] : 0;
    return 1;
}

// Cumulative counters for tools/bench-gp.sh, which reads them at race frames and
// divides: the race frame (Raceinfo+0x20, which Raceinfo::Update 0x805331B4
// advances once a frame from the start of the countdown; the race itself starts
// at frame 240), the time, the game thread's idle time and the GPU thread's busy
// time in ns, how many random seeds were fixed, the race stage (Raceinfo+0x28:
// 0 intro, 1 countdown, 2 racing) and whether a race exists at all (the Raceinfo
// instance pointer at 0x809BD730, stored by Raceinfo::CreateInstance).
extern "C" int mkw_mark(char* out, unsigned cap)
{
    uint32_t info = 0, frame = 0, stage = 0;
    const bool race = Memory::TryRead32(0x809BD730u, info) && info >= 0x80000000u && info < 0x81800000u;
    if (race) {
        Memory::TryRead32(info + 0x20u, frame);
        Memory::TryRead32(info + 0x28u, stage);
    }
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    const GxHw::Stats s = GxHw::GetStats();
    return snprintf(out, cap, "frame %u ns %llu idle %llu gpu %llu seeds %u stage %u race %d\n", frame,
                    static_cast<unsigned long long>(tb * 10000ull / 798ull),
                    static_cast<unsigned long long>(s.guestIdleNanos),
                    static_cast<unsigned long long>(s.gpuBusyNanos), g_fixedSeedReads, stage,
                    race ? 1 : 0);
}

// ---- slow-path census (the control port's `slow`) ---------------------------
//
// Counts, per guest function and kind (slow_path_census.h), the accesses that took
// a memory slow path: a store to the GX pipe or a quantized paired-single the
// translator couldn't resolve shows up here with its function. Off until `slow on`.

extern "C" {
volatile int g_mkwSlowCensus = 0;
}

namespace {
struct CensusSlot {
    uint32_t fn;
    uint32_t kind;
    uint32_t count;
};
constexpr unsigned kCensusSlots = 8192;
CensusSlot g_census[kCensusSlots];
}

extern "C" void mkw_slow_census_note(uint32_t kind)
{
    const uint32_t fn = RecompMod::g_currentTranslatedExecutionAddress;
    unsigned slot = ((fn >> 2) * 2654435761u + kind * 40503u) & (kCensusSlots - 1);
    for (unsigned probe = 0; probe < 16; ++probe, slot = (slot + 1) & (kCensusSlots - 1)) {
        CensusSlot& entry = g_census[slot];
        if (entry.count != 0 && (entry.fn != fn || entry.kind != kind))
            continue;
        entry.fn = fn;
        entry.kind = kind;
        ++entry.count;
        return;
    }
}

extern "C" int mkw_slow_census(const char* arg, char* out, unsigned cap)
{
    static const char* const kKinds[] = {"read mmio", "read ram",  "write fifo", "write mmio",
                                         "write ram", "psq load", "psq store",  "range fallback"};
    if (arg && !std::strncmp(arg, "on", 2)) {
        std::memset(g_census, 0, sizeof g_census);
        g_mkwSlowCensus = 1;
        return snprintf(out, cap, "census on\n");
    }
    if (arg && !std::strncmp(arg, "off", 3)) {
        g_mkwSlowCensus = 0;
        return snprintf(out, cap, "census off\n");
    }
    // The 40 busiest, then a fresh count.
    CensusSlot top[40] = {};
    for (const CensusSlot& entry : g_census) {
        if (entry.count <= top[39].count)
            continue;
        unsigned at = 39;
        while (at > 0 && top[at - 1].count < entry.count) {
            top[at] = top[at - 1];
            --at;
        }
        top[at] = entry;
    }
    std::memset(g_census, 0, sizeof g_census);
    int used = 0;
    for (const CensusSlot& entry : top) {
        if (entry.count == 0 || used >= static_cast<int>(cap) - 64)
            break;
        used += snprintf(out + used, cap - used, "%08x %-14s %u\n", entry.fn,
                         kKinds[entry.kind & 7u], entry.count);
    }
    return used;
}

// One stack sample: [current function or 1 when idle, LR, return addresses
// walked up the guest back chain...]. The guest stack lives in guest memory and
// the translated code keeps it as the original did, so the chain is readable
// from here; r1 in the context is as of the last call, close enough for a
// profile. Reads race the guest and are range-checked, never trusted.
extern "C" int mkw_guest_stack(uint32_t* out, unsigned cap)
{
    const CpuContext* cpu = TryGetCpuContext();
    if (!g_guestExecAddr || !cpu || cap < 2)
        return 0;
    const uint8_t* base = MkwGuestArenaBase();
    auto read = [base](uint32_t a, uint32_t& v) {
        if ((a & 3u) || !((a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u)))
            return false;
        v = *reinterpret_cast<const volatile uint32_t*>(base + (a & 0x3FFFFFFFu));
        return true;
    };
    unsigned n = 0;
    out[n++] = g_mkwGuestIdle ? 1u : *g_guestExecAddr;
    out[n++] = cpu->lr;
    uint32_t sp = cpu->gpr[1];
    while (n < cap) {
        uint32_t next = 0, ret = 0;
        if (!read(sp, next) || next <= sp || next - sp > 0x10000u || !read(next + 4, ret))
            break;
        out[n++] = ret;
        sp = next;
    }
    return static_cast<int>(n);
}

extern "C" int mkw_where(char* out, unsigned cap)
{
    uint32_t func = 0, lr = 0, sp = 0;
    if (!mkw_guest_sample(&func, &lr, &sp))
        return std::snprintf(out, cap, "guest not started crumb=%s\n", g_mkwCrumb);
    const CpuContext* cpu = TryGetCpuContext();
    return std::snprintf(out, cap, "func=%08x lr=%08x sp=%08x pc=%08x r3=%08x r4=%08x r31=%08x crumb=%s\n", func,
                         lr, sp, cpu ? cpu->pc : 0u, cpu ? cpu->gpr[3] : 0u, cpu ? cpu->gpr[4] : 0u,
                         cpu ? cpu->gpr[31] : 0u, g_mkwCrumb);
}

// ---- the crash-reporting seam upstream's main.cpp provides -----------------

void MarkFatalErrorReported()
{
    g_fatalReported = true;
}

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept
{
    std::printf("[fatal] %.*s\n%.*s\n", static_cast<int>(category.size()), category.data(),
                static_cast<int>(details.size()), details.data());
    std::fflush(stdout);
}

void WriteFatalLog(std::string_view reason)
{
    std::printf("[fatal] %.*s\n", static_cast<int>(reason.size()), reason.data());
    std::fflush(stdout);
}

void SetRuntimeExitCode(int code)
{
    g_exitCode = code;
}

extern "C" void DumpHostStackTraceForRuntimeHelper()
{
}

void DumpHostStackTrace()
{
}

namespace RuntimeCrash {

void WriteCrashArtifacts(std::string_view reason, std::string_view extraDetails,
                         const uint32_t* missingGuestTarget) noexcept
{
    std::printf("[crash] %.*s %.*s", static_cast<int>(reason.size()), reason.data(),
                static_cast<int>(extraDetails.size()), extraDetails.data());
    if (missingGuestTarget)
        std::printf(" target=%08x", *missingGuestTarget);
    std::printf("\n");
    std::fflush(stdout);
}

[[noreturn]] void FatalMissingGuestTarget(uint32_t target, CpuContext* cpu) noexcept
{
    std::printf("[crash] call to untranslated guest address %08x (lr %08x)\n", target, cpu ? cpu->lr : 0u);
    SystemBridge::DumpCpuState(cpu);
    std::fflush(stdout);
    std::exit(EXIT_FAILURE);
}

} // namespace RuntimeCrash

extern "C" bool g_dynamicAspectRatioEnabled = false;

// ---------------------------------------------------------------------------


int main(int argc, char** argv)
{
    // User state (Config.toml, NAND) and the extracted disc live beside the
    // app, in its USRDIR.
    if (argc > 0 && argv[0]) {
        const std::string self(argv[0]);
        const size_t slash = self.rfind('/');
        if (slash != std::string::npos) {
            std::filesystem::current_path(self.substr(0, slash));
            mkw_rescue_set_app_dir(self.substr(0, slash).c_str());
        }
    }
    OpenLog();
    std::printf("[main] mkwii-ps3 starting in %s\n", std::filesystem::current_path().string().c_str());
    {
        uint64_t freeBytes = 0, totalBytes = 0;
        mkw_lv2_memory(&freeBytes, &totalBytes);
        std::printf("[main] user memory: %llu of %llu KiB free\n", static_cast<unsigned long long>(freeBytes >> 10),
                    static_cast<unsigned long long>(totalBytes >> 10));
    }
    // Name what escaped: an uncaught exception otherwise ends in a bare abort().
    std::set_terminate([] {
        char name[32];
        mkw_lv2_thread_name(name, sizeof name);
        const char* what = "unknown";
        try {
            if (auto e = std::current_exception())
                std::rethrow_exception(e);
            what = "terminate without an exception";
        } catch (const std::exception& ex) {
            std::printf("[main] uncaught exception on thread '%s': %s\n", name, ex.what());
            what = nullptr;
        } catch (...) {
        }
        if (what)
            std::printf("[main] std::terminate on thread '%s': %s\n", name, what);
        abort();
    });
    mkw_rescue_start();
    mkw_fault_handler_start();
    // Config.toml is a function-local static the guest, audio and GPU threads
    // all reach early; read it here, once, before any of them exists.
    (void)RuntimeConfigFile::Get();

    uint64_t guestThread = 0;
    // Translated code keeps one host frame per guest call; 8 MiB covers the
    // deepest chains with room to spare.
    if (mkw_lv2_thread_create(&guestThread, GuestMain, nullptr, 1000, 8u << 20, "mkw_guest") != 0) {
        std::printf("[main] guest thread creation failed\n");
        return 1;
    }

    mkw_sysutil_start();
    unsigned ticks = 0;
    // Runs until the user quits from the XMB or the control port relaunches
    // the app: a guest that stopped (or faulted) leaves the process up so the
    // log can be read and a new build started remotely.
    bool reportedDone = false;
    for (;;) {
        if (g_guestDone.load() && !reportedDone) {
            reportedDone = true;
            std::printf("[main] guest stopped (exit code %d); waiting for relaunch or quit\n", g_exitCode.load());
        }
        mkw_lv2_usleep(100000);
        if (mkw_sysutil_poll()) {
            // Quit Game from the XMB: lv2 kills the process shortly after
            // this event regardless, so leave while the log is consistent.
            std::printf("[main] quit requested from the XMB\n");
            std::fflush(stdout);
            AudioBackend::Instance().Shutdown();
            std::exit(0);
        }
        if (++ticks % 50 == 0 && !g_guestDone.load())
            ReportStats();
    }
}

// Process exits from the runtime's fatal paths (std::exit / std::abort) would
// take the control port down with them. Failures park the calling thread
// instead; a clean exit(0) still ends the process.
extern "C" void __real_exit(int status) __attribute__((noreturn));

extern "C" void __wrap_exit(int status)
{
    std::fflush(stdout);
    if (status == 0) {
        AudioBackend::Instance().Shutdown();
        __real_exit(0);
    }
    std::printf("[main] exit(%d) requested; parking the thread\n", status);
    g_exitCode = status;
    g_guestDone = true;
    for (;;)
        mkw_lv2_usleep(1000000);
}

extern "C" void __wrap_abort(void)
{
    char name[32];
    mkw_lv2_thread_name(name, sizeof name);
    std::printf("[main] abort() on thread '%s'; parking it\n", name);
    g_exitCode = 134;
    g_guestDone = true;
    for (;;)
        mkw_lv2_usleep(1000000);
}
