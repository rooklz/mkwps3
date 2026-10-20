#pragma once

#include "abi_bridge.h"

#include <cstdio>
#include <cstdlib>

// Last native milestone the guest thread passed, for the control port's
// `where` (a hang inside HLE code is invisible to the guest-address sampler).
inline const char* volatile g_mkwCrumb = "";
inline void MkwCrumb(const char* what) { g_mkwCrumb = what; }
// Set while the guest scheduler has nothing runnable; read by the sampler.
inline volatile int g_mkwGuestIdle = 0;

// VI Utils
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
void VI_HLE_WaitForNextRetracePoll();
bool VI_HLE_IsAdvancingRetrace();
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
bool OS_HLE_InterruptsEnabled() noexcept;
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address. The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.

#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
