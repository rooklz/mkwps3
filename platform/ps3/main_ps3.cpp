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
extern "C" void mkw_sysutil_start(void);
extern "C" int mkw_sysutil_poll(void);
extern "C" const GxSink* mkw_renderer_sink(void);
extern "C" unsigned long long mkw_renderer_frames(void);

namespace {

extern volatile uint32_t* g_guestExecAddr;
extern volatile uint32_t* g_guestEntries;
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

// /dev_hdd0/tmp/mkwii-norender.txt: parse the GX stream but draw nothing.
bool NoRendererFlag()
{
    FILE* f = std::fopen("/dev_hdd0/tmp/mkwii-norender.txt", "r");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

void GuestMain(void*)
{
    mkw_rescue_register_thread(0);
    g_guestExecAddr = &RecompMod::g_currentTranslatedExecutionAddress;
    g_guestEntries = &RecompMod::g_translatedEntries;
    try {
        SystemBridge::Initialize();
        TranslatedFunctionRegistry::Finalize();
        {
            uint64_t freeBytes = 0, totalBytes = 0;
            mkw_lv2_memory(&freeBytes, &totalBytes);
            std::printf("[main] after guest memory and data init: %llu KiB free\n",
                        static_cast<unsigned long long>(freeBytes >> 10));
        }
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
        // Zero unless the translated code counts entries (MKW_COUNT_CALLS).
        if (entries != lastEntries) {
            std::printf("calls %llu/frame; ", nf ? static_cast<unsigned long long>(entries - lastEntries) / nf : 0ull);
            std::printf("%.3f us/call; ", (5000.0 - idleMs) * 1000.0 / static_cast<double>(entries - lastEntries));
        }
        lastEntries = entries;
    }
    lastFrames = frames;
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
        if (slash != std::string::npos)
            std::filesystem::current_path(self.substr(0, slash));
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
