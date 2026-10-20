// Leaving the game: the Wii's menu, restart and power calls. There is no
// system menu to return to, so returning to it or shutting down quits the app
// and a restart relaunches it.
//
// Also the disc cover check those paths make first. __DVDGetCoverStatus asks
// the drive for its cover register through DVDLowPrepareCoverRegister and
// spins until the reply arrives; nothing answers that request here, so it
// never returned.

#include "hle_stubs.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef __PS3__
extern "C" void mkw_platform_quit(int restart);
#else
static void mkw_platform_quit(int)
{
    std::exit(0);
}
#endif

namespace {

void Leave(const char* what, int restart)
{
    std::printf("[os] %s: %s\n", what, restart ? "relaunching" : "quitting");
    std::fflush(stdout);
    mkw_platform_quit(restart);
}

void OSShutdownSystem_HLE(CpuContext*)
{
    Leave("OSShutdownSystem", 0);
}

void OSRestart_HLE(CpuContext*)
{
    Leave("OSRestart", 1);
}

void OSReturnToMenu_HLE(CpuContext*)
{
    Leave("OSReturnToMenu", 0);
}

void OSReturnToMenuForError_HLE(CpuContext*)
{
    Leave("__OSReturnToMenuForError", 0);
}

// 2 is "cover closed": 2 - (cover register & 1) in the original.
uint32_t DVDGetCoverStatus_HLE()
{
    return 2;
}

} // namespace

PPC_NATIVE_OVERRIDE_VOID(801A856C, OSShutdownSystem_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(801A8688, OSRestart_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(801A8858, OSReturnToMenu_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(801A8954, OSReturnToMenuForError_HLE, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE(8016322C, DVDGetCoverStatus_HLE, uint32_t, (), ());
