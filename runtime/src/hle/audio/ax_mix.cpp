#include "hle_stubs.h"
#include "ax_dsp.h"

#include "ax_internal.h"
#include "ax_voice.h"

#include "abi_bridge.h"
#include "audio_backend.h"
#include "ax_mix_kernels.h"
#include "isa/big_endian.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

// Timebase ticks the mix worker spent mixing and the guest thread spent waiting for it, for the
// control port's stats.
static std::atomic<uint64_t> g_mixWorkTicks{0};
static std::atomic<uint64_t> g_mixJoinTicks{0};
static std::atomic<uint64_t> g_mixVoices{0};
// SPU decrementer ticks (the same 79.8 MHz) the SPU mixer spent on the voices.
static std::atomic<uint64_t> g_mixSpuTicks{0};
static std::atomic<uint64_t> g_mixSpuWaitPb{0};
static std::atomic<uint64_t> g_mixSpuWaitSmp{0};

static inline uint64_t MixTicks()
{
#if defined(__PPU__) || defined(__PS3__)
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
#else
    return 0;
#endif
}

#if defined(__PPU__) || defined(__PS3__)
#include "spu/ax_spu_job.h"
extern "C" int mkw_lv2_thread_set_priority(int priority);
// platform/ps3/ax_spu_ps3.cpp
extern "C" AxSpuJob* mkw_ax_spu_job(void);
extern "C" void mkw_ax_spu_kick(void);
extern "C" const AxSpuResult* mkw_ax_spu_wait(void);
#define MKW_AX_SPU 1
#else
#define MKW_AX_SPU 0
#endif

extern "C" void mkw_mix_ticks(uint64_t* work, uint64_t* join, uint64_t* voices, uint64_t* spu)
{
    *work = g_mixWorkTicks.load(std::memory_order_relaxed);
    *join = g_mixJoinTicks.load(std::memory_order_relaxed);
    *voices = g_mixVoices.load(std::memory_order_relaxed);
    *spu = g_mixSpuTicks.load(std::memory_order_relaxed);
}

// Of the SPU mixer's time, how much went on waiting for PB and for sample reads.
extern "C" void mkw_mix_spu_waits(uint64_t* pb, uint64_t* samples)
{
    *pb = g_mixSpuWaitPb.load(std::memory_order_relaxed);
    *samples = g_mixSpuWaitSmp.load(std::memory_order_relaxed);
}

namespace AxDspHle {
namespace {

std::filesystem::path FindDspCoefficientRom() {
    if (const auto executableDirectory = RuntimeConfigFile::ExecutableDirectory()) {
        const auto adjacent = *executableDirectory / "dsp_coef.bin";
        if (std::filesystem::is_regular_file(adjacent)) {
            return adjacent;
        }
    }

    for (auto base = std::filesystem::current_path(); !base.empty();) {
        const auto sourceTreeAsset = base / "runtime" / "assets" / "dsp" / "dsp_coef.bin";
        if (std::filesystem::is_regular_file(sourceTreeAsset)) {
            return sourceTreeAsset;
        }
        const auto parent = base.parent_path();
        if (parent == base) {
            break;
        }
        base = parent;
    }

    throw std::runtime_error("Missing bundled Wii DSP coefficient ROM (dsp_coef.bin)");
}

class AXWii {
public:
    AXWii() {
        m_lastMainVolume = 0x8000;
        m_lastAuxVolumes.fill(0x8000);
    }

    ~AXWii() { ShutdownMixWorker(); }

    void Initialize() {
        // Every entry point that resets or reconfigures mix state joins the
        // worker first: the mix owns those members outright while it runs.
        MkwCrumb("ax: Initialize JoinMix");
        JoinMix();
        MkwCrumb("ax: Initialize memory map");
        RefreshMixMemoryMap();
        MkwCrumb("ax: Initialize lock");
        std::lock_guard<std::mutex> lock(m_mutex);
        MkwCrumb("ax: Initialize config");
        m_mixWorkerEnabled = RuntimeConfigFile::AudioMixWorkerEnabled(false);
        MkwCrumb("ax: Initialize state");
        MarkDspInitialized();
        ResetDspTaskGlobals();
        m_mailState = MailState::WaitingForCmdListSize;
        m_cmdListSize = 0;
        m_toDspBusy = false;
        m_cpuMails.clear();
        m_compressorPos = 0;
        m_deferredResumeCallbacks = 0;
        // Both layouts are re-derived from the ucode CRC by ConfigureFromTask;
        // until one arrives, the PB layout is detected per parameter block and
        // the command layout is probed per command list.
        m_pbLayout = PBLayout::Unknown;
        m_commandLayout = AXCommandLayout::Auto;
        m_ucodeCrc = 0;
        m_auxOutCount = 0;
        m_auxInCount = 0;
        m_deferredAux = false;
        LoadResamplingCoefficients();
        PushMail(kDspInit);
    }

    void Stop() {
        JoinMix();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mailState = MailState::WaitingForCmdListSize;
        m_cmdListSize = 0;
        m_toDspBusy = false;
        m_cpuMails.clear();
        m_lastCpuMail = 0;
        m_compressorPos = 0;
        m_deferredResumeCallbacks = 0;
        m_ucodeCrc = 0;
    }

    void ConfigureFromTask(uint32_t taskPtr) {
        // ConfigureFromTaskLocked rewrites the ucode CRC and the PB/command
        // layouts, all of which the mix reads.
        JoinMix();
        std::lock_guard<std::mutex> lock(m_mutex);
        ConfigureFromTaskLocked(taskPtr);
    }

    void JoinMix() {
        if (m_mixThread.joinable()) {
            const uint64_t start = MixTicks();
            std::unique_lock<std::mutex> lock(m_mixMutex);
            m_mixIdle.wait(lock, [this] { return !m_mixPending && !m_mixBusy; });
            g_mixJoinTicks.fetch_add(MixTicks() - start, std::memory_order_relaxed);
        }
        FinishSpuMix();
        PublishAuxOutputs();
    }

    void ShutdownMixWorker() {
        if (!m_mixThread.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mixMutex);
            m_mixStop = true;
        }
        m_mixWake.notify_all();
        m_mixThread.join();
    }

    void SetMixWorkerEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mixWorkerEnabled = enabled;
    }

    void SendMail(uint32_t mail) {
        std::lock_guard<std::mutex> lock(m_mutex);
        HandleMail(mail);
        m_toDspBusy = false;
    }

    void QueueResumeCallback() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_deferredResumeCallbacks;
    }

    void ServiceDeferredCallbacks() {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_deferredResumeCallbacks == 0) {
                    return;
                }
                --m_deferredResumeCallbacks;
            }
            InvokeAxTaskCallback(0x2cu, false);
        }
    }

    uint32_t CheckMailToDSP() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_toDspBusy ? 1u : 0u;
    }

    uint32_t CheckMailFromDSP() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cpuMails.empty() ? 0u : 1u;
    }

    uint32_t ReadMailFromDSP() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cpuMails.empty()) {
            return m_lastCpuMail & 0x7fffffffu;
        }
        m_lastCpuMail = m_cpuMails.front();
        m_cpuMails.pop_front();
        return m_lastCpuMail;
    }

private:
    // What DispatchCommandList has to know about a command list before it is
    // allowed to leave the guest thread.
    struct CommandListScan {
        uint32_t outputCommands = 0;
        uint32_t auxInputCount = 0;
        // Three blocks per MIX_AUX* (x3) plus four per UPL_AUX* (x2) = 17.
        std::array<uint32_t, 24> auxInputAddrs{};
    };

    struct AuxBlock {
        uint32_t addr = 0;
        std::array<int32_t, kAxSamplesPerFrame> data{};
    };

    enum Cmd : uint16_t {
        CMD_SETUP = 0x00,
        CMD_ADD_TO_LR = 0x01,
        CMD_SUB_TO_LR = 0x02,
        CMD_ADD_SUB_TO_LR = 0x03,
        CMD_PROCESS = 0x04,
        CMD_MIX_AUXA = 0x05,
        CMD_MIX_AUXB = 0x06,
        CMD_MIX_AUXC = 0x07,
        CMD_UPL_AUXA_MIX_LRSC = 0x08,
        CMD_UPL_AUXB_MIX_LRSC = 0x09,
        CMD_COMPRESSOR = 0x0a,
        CMD_OUTPUT = 0x0b,
        CMD_OUTPUT_DPL2 = 0x0c,
        CMD_WM_OUTPUT = 0x0d,
        CMD_END = 0x0e,
    };


    static constexpr uint16_t kCmdSetPbAddressOld = 0x04;

    struct DecodedCommand {
        Cmd cmd = CMD_SETUP;
        bool setsPbAddress = false;
    };

    static DecodedCommand DecodeCommand(uint16_t raw, AXCommandLayout layout) {
        if (layout != AXCommandLayout::Old) {
            return {static_cast<Cmd>(raw), false};
        }
        if (raw == kCmdSetPbAddressOld) {
            return {CMD_SETUP, true};
        }
        return {static_cast<Cmd>(raw > kCmdSetPbAddressOld ? raw - 1u : raw), false};
    }

    // The old layout runs the PB chain the previous SET_PB_ADDRESS stashed, and
    // ProcessPBList applies the old per-millisecond PB update walk for it.
    static bool UsesStashedPbAddress(AXCommandLayout layout) {
        return layout == AXCommandLayout::Old;
    }

    // Only the plain New encoding puts a volume halfword in front of OUTPUT's
    // two addresses; Old and NewNoOutputVolume both imply 0x8000.
    static bool OutputCarriesVolume(AXCommandLayout layout) {
        return layout != AXCommandLayout::Old && layout != AXCommandLayout::NewNoOutputVolume;
    }

    void PushMail(uint32_t mail) {
        m_cpuMails.push_back(mail);
    }

    void HandleMail(uint32_t mail) {
        switch (m_mailState) {
        case MailState::WaitingForCmdListSize:
            if ((mail & kMailCmdListMask) == kMailCmdList) {
                m_cmdListSize = mail & 0xffffu;
                m_mailState = MailState::WaitingForCmdListAddress;
            } else if ((mail & 0xffff0000u) == kTaskMailToDsp) {
                HandleTaskMail(mail);
            }
            break;
        case MailState::WaitingForCmdListAddress:
            // The previous mix owns m_cmdList, the mix buses and the PB chain
            // until it finishes. In practice this join is free
            JoinMix();
            CopyCmdList(mail, m_cmdListSize);
            DispatchCommandList();
            m_cmdListSize = 0;
            //  yield mail and the deferred resume callback are still raised synchronously at mail time,
            PushMail(kDspYield);
            ++m_deferredResumeCallbacks;
            m_mailState = MailState::WaitingForNextTask;
            break;
        case MailState::WaitingForNextTask:
            if ((mail & kMailCmdListMask) == kMailCmdList) {
                m_cmdListSize = mail & 0xffffu;
                m_mailState = MailState::WaitingForCmdListAddress;
            } else {
                HandleTaskMail(mail);
            }
            break;
        }
    }

    void HandleTaskMail(uint32_t mail) {
        switch (mail) {
        case kMailResume:
            PushMail(kDspResume);
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        case kMailNewUCode:
        case kMailContinue:
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        case kMailReset:
            PushMail(kDspDone);
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        default:
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        }
    }

    void CopyCmdList(uint32_t addr, uint32_t sizeWords) {
        if (sizeWords > m_cmdList.size()) {
            // Truncating a command list drops whole commands
            if (!m_loggedCmdListTruncated.exchange(true, std::memory_order_relaxed)) {
                RT_LOGF(RT_TAG_AUDIO,
                             "command list of %u words exceeds the %zu-word buffer; truncating\n",
                             sizeWords, m_cmdList.size());
                std::fflush(stderr);
            }
            sizeWords = static_cast<uint32_t>(m_cmdList.size());
        }
        for (uint32_t i = 0; i < sizeWords; ++i) {
            m_cmdList[i] = Memory::Read16(addr + i * 2);
        }
    }

    // Everything that must be observed at mail time happens here, before the mix leaves the
    // guest thread: the DSP sync mails OUTPUT commands raise (so the mail sequence doesn't
    // depend on where the mix runs)
    void DispatchCommandList() {
        AXCommandLayout layout = m_commandLayout;
        if (layout == AXCommandLayout::Auto) {
            const bool validNew = ScanCommandList(AXCommandLayout::New, nullptr);
            const bool validNewNoOutputVolume = ScanCommandList(AXCommandLayout::NewNoOutputVolume, nullptr);
            const bool validOld = ScanCommandList(AXCommandLayout::Old, nullptr);
            layout = validNew ? AXCommandLayout::New :
                     (validNewNoOutputVolume ? AXCommandLayout::NewNoOutputVolume :
                      (validOld ? AXCommandLayout::Old : AXCommandLayout::New));
        }

        CommandListScan scan{};
        ScanCommandList(layout, &scan);
        for (uint32_t i = 0; i < scan.outputCommands; ++i) {
            PushMail(kDspSync);
        }

        // With the SPU mixer the commands after the voices run at the join, so they see the
        // aux buffers the same way the worker does.
        const bool spu = !m_mixWorkerEnabled && SpuMixAvailable();
        const bool deferred = spu || (m_mixWorkerEnabled && EnsureMixWorker());
        m_deferredAux = deferred;
        m_auxInCount = 0;
        if (deferred) {
            for (uint32_t i = 0; i < scan.auxInputCount && m_auxInCount < m_auxIn.size(); ++i) {
                AuxBlock& block = m_auxIn[m_auxInCount++];
                block.addr = scan.auxInputAddrs[i];
                ReadGuestS32Buffer(block.addr, block.data.data(), kAxSamplesPerFrame);
            }
        }

        m_mixLayout = layout;
        m_mixCmdListSize = m_cmdListSize;
        if (deferred && !spu) {
            {
                std::lock_guard<std::mutex> lock(m_mixMutex);
                m_mixPending = true;
            }
            m_mixWake.notify_one();
            return;
        }

        const uint64_t start = MixTicks();
        if (spu) {
            uint32_t idx = 0;
            uint32_t oldPbAddr = 0;
            m_auxOutCount = 0;
            if (RunCommands(m_mixLayout, m_mixCmdListSize, idx, oldPbAddr, true)) {
                m_spuPending = true;
                m_spuResumeIdx = idx;
                m_spuOldPbAddr = oldPbAddr;
            }
        } else {
            ExecuteCommandList(m_mixLayout, m_mixCmdListSize);
        }
        g_mixWorkTicks.fetch_add(MixTicks() - start, std::memory_order_relaxed);
    }

    bool CanReadWords(uint32_t idx, uint32_t count) const {
        return idx <= m_cmdListSize && count <= m_cmdListSize - idx;
    }

    // Walks the command list for a candidate layout
    bool ScanCommandList(AXCommandLayout layout, CommandListScan* scan) const {
        uint32_t idx = 0;
        bool end = false;
        auto addAuxInput = [&](uint32_t addr, uint32_t blocks) {
            if (!scan) {
                return;
            }
            for (uint32_t block = 0; block < blocks; ++block) {
                if (scan->auxInputCount >= scan->auxInputAddrs.size()) {
                    return;
                }
                scan->auxInputAddrs[scan->auxInputCount++] =
                    addr + block * kAxSamplesPerFrame * static_cast<uint32_t>(sizeof(int32_t));
            }
        };
        auto wordAddress = [&](uint32_t base) {
            return Hilo(m_cmdList[base], m_cmdList[base + 1]);
        };

        while (!end) {
            if (!CanReadWords(idx, 1)) {
                return false;
            }
            const uint16_t cmd = m_cmdList[idx++];
            const uint32_t operandBase = idx;
            auto skip = [&](uint32_t words) {
                if (!CanReadWords(idx, words)) {
                    return false;
                }
                idx += words;
                return true;
            };

            const DecodedCommand decoded = DecodeCommand(cmd, layout);
            if (decoded.setsPbAddress) {
                if (!skip(2)) return false;
                continue;
            }

            switch (decoded.cmd) {
            case CMD_SETUP:
            case CMD_ADD_TO_LR:
            case CMD_SUB_TO_LR:
            case CMD_ADD_SUB_TO_LR:
                if (!skip(2)) return false;
                break;
            case CMD_PROCESS:
                if (!UsesStashedPbAddress(layout) && !skip(2)) return false;
                break;
            case CMD_MIX_AUXA:
            case CMD_MIX_AUXB:
            case CMD_MIX_AUXC:
                if (!skip(5)) return false;
                // volume, write address, read address; the mix reads three
                // consecutive 96-word blocks from the read address.
                addAuxInput(wordAddress(operandBase + 3), 3);
                break;
            case CMD_UPL_AUXA_MIX_LRSC:
            case CMD_UPL_AUXB_MIX_LRSC:
                if (!skip(13)) return false;
                // volume then six addresses; [2..5] are read back.
                for (uint32_t slot = 2; slot < 6; ++slot) {
                    addAuxInput(wordAddress(operandBase + 1 + slot * 2), 1);
                }
                break;
            case CMD_COMPRESSOR:
                if (!skip(4)) return false;
                break;
            case CMD_OUTPUT:
            case CMD_OUTPUT_DPL2:
                if (!skip(OutputCarriesVolume(layout) ? 5 : 4)) return false;
                if (scan) ++scan->outputCommands;
                break;
            case CMD_WM_OUTPUT:
                if (!skip(8)) return false;
                break;
            case CMD_END:
                end = true;
                break;
            default:
                return false;
            }
        }
        return idx <= m_cmdListSize;
    }

    void ExecuteCommandList(AXCommandLayout layout, uint32_t cmdListSize) {
        uint32_t idx = 0;
        uint32_t oldPbAddr = 0;
        m_auxOutCount = 0;
        RunCommands(layout, cmdListSize, idx, oldPbAddr, false);
    }

    // Runs the command list from `idx`. With `toSpu` the first PB list goes to the SPU
    // mixer if it can take it, and this returns true straight after handing it over, with
    // `idx` just past that command for FinishSpuMix to carry on from.
    bool RunCommands(AXCommandLayout layout, uint32_t cmdListSize, uint32_t& idx, uint32_t& oldPbAddr,
                     bool toSpu) {
        const bool newFilter = m_ucodeCrc != 0 ? UCodeUsesNewFilter(m_ucodeCrc) : layout != AXCommandLayout::Old;
        bool end = false;
        while (!end && idx < cmdListSize) {
            const uint16_t cmd = m_cmdList[idx++];
            uint16_t volume = 0;
            uint16_t frames = 0;
            uint32_t addr = 0;
            uint32_t addr2 = 0;
            const DecodedCommand decoded = DecodeCommand(cmd, layout);
            if (decoded.setsPbAddress) {
                oldPbAddr = ReadAddress(idx);
                continue;
            }

            switch (decoded.cmd) {
            case CMD_SETUP:
                addr = ReadAddress(idx);
                SetupProcessing(addr);
                break;
            case CMD_ADD_TO_LR:
            case CMD_SUB_TO_LR:
                addr = ReadAddress(idx);
                AddToLR(addr, decoded.cmd == CMD_SUB_TO_LR);
                break;
            case CMD_ADD_SUB_TO_LR:
                addr = ReadAddress(idx);
                AddSubToLR(addr);
                break;
            case CMD_PROCESS:
                if (UsesStashedPbAddress(layout)) {
                    ProcessPBList(oldPbAddr, newFilter, true);
                } else {
                    addr = ReadAddress(idx);
                    if (toSpu && StartSpuMix(addr, newFilter)) {
                        return true;
                    }
                    ProcessPBList(addr, newFilter, false);
                }
                break;
            case CMD_MIX_AUXA:
            case CMD_MIX_AUXB:
            case CMD_MIX_AUXC:
                volume = m_cmdList[idx++];
                addr = ReadAddress(idx);
                addr2 = ReadAddress(idx);
                MixAUXSamples(decoded.cmd - CMD_MIX_AUXA, addr, addr2, volume);
                break;
            case CMD_UPL_AUXA_MIX_LRSC:
            case CMD_UPL_AUXB_MIX_LRSC: {
                volume = m_cmdList[idx++];
                std::array<uint32_t, 6> addresses{};
                for (uint32_t& out : addresses) {
                    out = ReadAddress(idx);
                }
                UploadAUXMixLRSC(decoded.cmd == CMD_UPL_AUXB_MIX_LRSC ? 1 : 0, addresses.data(), volume);
                break;
            }
            case CMD_COMPRESSOR:
                volume = m_cmdList[idx++];
                frames = m_cmdList[idx++];
                addr = ReadAddress(idx);
                RunCompressor(volume, frames, addr);
                break;
            case CMD_OUTPUT:
            case CMD_OUTPUT_DPL2:
                volume = OutputCarriesVolume(layout) ? m_cmdList[idx++] : 0x8000u;
                addr = ReadAddress(idx);
                addr2 = ReadAddress(idx);
                OutputSamples(addr2, addr, volume, decoded.cmd == CMD_OUTPUT_DPL2);
                break;
            case CMD_WM_OUTPUT: {
                std::array<uint32_t, 4> addresses{};
                for (uint32_t& out : addresses) {
                    out = ReadAddress(idx);
                }
                OutputWMSamples(addresses.data());
                break;
            }
            case CMD_END:
                end = true;
                break;
            default:
                ReportUnknownCommand(cmd, idx - 1);
                end = true;
                break;
            }
        }
        return false;
    }

    bool SpuMixAvailable() {
#if MKW_AX_SPU
        return g_mixMem1.load(std::memory_order_relaxed) != nullptr && mkw_ax_spu_job() != nullptr;
#else
        return false;
#endif
    }

    // Hands the voices of PB list `pbAddr` to the SPU mixer. It accumulates into m_bus and
    // m_wmBus and writes the PBs back, so nothing here touches either until FinishSpuMix.
    bool StartSpuMix(uint32_t pbAddr, bool newFilter) {
#if MKW_AX_SPU
        // The PB layout is settled by the ucode CRC, or by the PPU's first walk detecting it.
        if (m_pbLayout == PBLayout::Unknown) {
            return false;
        }
        AxSpuJob* job = mkw_ax_spu_job();
        if (!job) {
            return false;
        }
        std::array<PBSegment, 3> segments{};
        const size_t count = PBSegmentsForLayout(m_pbLayout, segments);
        job->mem1_ea = reinterpret_cast<uintptr_t>(g_mixMem1.load(std::memory_order_relaxed));
        job->mem2_ea = reinterpret_cast<uintptr_t>(g_mixMem2.load(std::memory_order_relaxed));
        job->bus_ea = reinterpret_cast<uintptr_t>(m_bus.data());
        job->wmbus_ea = reinterpret_cast<uintptr_t>(m_wmBus.data());
        job->coef_ea = m_hasCoeffs ? reinterpret_cast<uintptr_t>(m_coeffs.data()) : 0;
        job->pb_addr = pbAddr;
        job->flags = (newFilter ? AX_SPU_NEW_FILTER : 0u) | (m_coeffsFit32 ? AX_SPU_COEFFS_FIT32 : 0u);
        job->nseg = static_cast<uint32_t>(count);
        for (size_t i = 0; i < count; ++i) {
            job->seg[i][0] = static_cast<uint16_t>(segments[i].guestOffset);
            job->seg[i][1] = static_cast<uint16_t>(segments[i].structOffset);
            job->seg[i][2] = static_cast<uint16_t>(segments[i].byteCount);
        }
        mkw_ax_spu_kick();
        return true;
#else
        (void)pbAddr;
        (void)newFilter;
        return false;
#endif
    }

    // Waits for the SPU's voices, then runs the rest of the command list (aux returns,
    // uploads, output) on the guest thread.
    void FinishSpuMix() {
#if MKW_AX_SPU
        if (!m_spuPending) {
            return;
        }
        m_spuPending = false;
        const uint64_t start = MixTicks();
        if (const AxSpuResult* result = mkw_ax_spu_wait()) {
            g_mixVoices.fetch_add(result->voices, std::memory_order_relaxed);
            g_mixSpuTicks.fetch_add(result->ticks, std::memory_order_relaxed);
            g_mixSpuWaitPb.fetch_add(result->wait_pb, std::memory_order_relaxed);
            g_mixSpuWaitSmp.fetch_add(result->wait_smp, std::memory_order_relaxed);
            if (result->bad != 0 && !m_loggedSpuBadAccess.exchange(true, std::memory_order_relaxed)) {
                RT_LOGF(RT_TAG_AUDIO, "SPU mixer skipped %u guest accesses outside MEM1/MEM2\n", result->bad);
            }
        }
        const uint64_t resumed = MixTicks();
        g_mixJoinTicks.fetch_add(resumed - start, std::memory_order_relaxed);
        uint32_t idx = m_spuResumeIdx;
        uint32_t oldPbAddr = m_spuOldPbAddr;
        RunCommands(m_mixLayout, m_mixCmdListSize, idx, oldPbAddr, false);
        g_mixWorkTicks.fetch_add(MixTicks() - resumed, std::memory_order_relaxed);
#endif
    }

    void ReportUnknownCommand(uint16_t cmd, uint32_t word) {
        if (m_loggedUnknownCommand.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        RT_LOGF(RT_TAG_AUDIO,
                     "unknown AX command 0x%04X at word %u; abandoning the command list\n",
                     cmd, word);
        std::fflush(stderr);
    }

    uint32_t ReadAddress(uint32_t& idx) const {
        const uint16_t hi = m_cmdList[idx++];
        const uint16_t lo = m_cmdList[idx++];
        return Hilo(hi, lo);
    }

    // Head-of-frame pointer for every bus, in AxBus order. This is the only
    // place the 20 buses are enumerated.
    AXBuffers MixBuffers() {
        AXBuffers buffers{};
        for (size_t i = 0; i < kAxRegularBusCount; ++i) {
            buffers[i] = m_bus[i].data();
        }
        for (size_t i = 0; i < kAxWiimoteBusCount; ++i) {
            buffers[kAxRegularBusCount + i] = m_wmBus[i].data();
        }
        return buffers;
    }

    void SetupProcessing(uint32_t initAddr) {
        const AXBuffers buffers = MixBuffers();
        for (size_t i = 0; i < buffers.size(); ++i) {
            const uint32_t value = (MixRead16(initAddr + static_cast<uint32_t>(i * 6)) << 16) |
                                   MixRead16(initAddr + static_cast<uint32_t>(i * 6 + 2));
            const int16_t delta = static_cast<int16_t>(MixRead16(initAddr + static_cast<uint32_t>(i * 6 + 4)));
            const int count = (i < kAxRegularBusCount ? 32 : 6) * 3;
            if (value == 0) {
                std::fill(buffers[i], buffers[i] + count, 0);
            } else {
                const int32_t start = static_cast<int32_t>(value);
                for (int j = 0; j < count; ++j) {
                    buffers[i][j] = start + j * delta;
                }
            }
        }
    }

    void AddToLR(uint32_t addr, bool neg) {
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            int32_t value = static_cast<int32_t>(MixRead32(addr + i * 4));
            if (neg) {
                value = -value;
            }
            m_bus[kBusMainL][i] += value;
            m_bus[kBusMainR][i] += value;
        }
    }

    void AddSubToLR(uint32_t addr) {
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            m_bus[kBusMainL][i] += static_cast<int32_t>(MixRead32(addr + i * 4));
        }
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            m_bus[kBusMainR][i] -= static_cast<int32_t>(MixRead32(addr + (kAxSamplesPerFrame + i) * 4));
        }
    }

    static void GenerateVolumeRamp(uint16_t* output, uint16_t from, uint16_t to, size_t count) {
        float current = static_cast<float>(from);
        const float delta = (static_cast<float>(to) - static_cast<float>(from)) / static_cast<float>(count);
        for (size_t i = 0; i < count; ++i) {
            current += delta;
            output[i] = static_cast<uint16_t>(current);
        }
    }

    void ProcessPBList(uint32_t pbAddr, bool newFilter, bool oldAxLayout) {
        uint32_t guard = 0;
        while (pbAddr && guard++ < 256) {
            // Most of the chain is voices that are not playing, which ProcessVoice leaves
            // untouched: the full read and write-back of those PBs was a third of the mix. The
            // old ucode's per-millisecond updates can start a voice, so it keeps the full walk.
            if (!oldAxLayout && m_pbLayout != PBLayout::Unknown &&
                MixRead16(pbAddr + static_cast<uint32_t>(offsetof(AXPBWii, running))) != 1) {
                pbAddr = Hilo(MixRead16(pbAddr), MixRead16(pbAddr + 2));
                continue;
            }
            AXPBWii pb{};
            ReadPB(pbAddr, pb);
            AXBuffers buffers = MixBuffers();
            const AXMixControl control = ConvertMixerControl(Hilo(pb.mixer_control_hi, pb.mixer_control_lo));
            if (oldAxLayout && (pb.updates.num_updates[0] | pb.updates.num_updates[1] | pb.updates.num_updates[2]) != 0) {
                const auto updates = LoadPBUpdates(pb);
                for (uint32_t ms = 0; ms < 3; ++ms) {
                    ApplyPBUpdatesForMs(pb, updates, ms);
                    if (ProcessVoice(pb, buffers, 32, control, BaseCoefficients(), newFilter, m_coeffsFit32)) {
                        g_mixVoices.fetch_add(1, std::memory_order_relaxed);
                    }
                    AdvanceBuffers(buffers, 32, 6);
                }
            } else {
                if (ProcessVoice(pb, buffers, kAxSamplesPerFrame, control, BaseCoefficients(), newFilter,
                                 m_coeffsFit32)) {
                    g_mixVoices.fetch_add(1, std::memory_order_relaxed);
                }
            }
            WritePB(pbAddr, pb);
            pbAddr = Hilo(pb.next_pb_hi, pb.next_pb_lo);
        }
    }

    // Four consecutive 0x200-halfword polyphase banks; only the voice resample selects a
    // bank (VoiceCoefficients). Wiimote downsample always uses bank 0 at its fixed ratio.
    const int16_t* BaseCoefficients() const {
        return m_hasCoeffs ? m_coeffs.data() : nullptr;
    }

    void LoadResamplingCoefficients() {
        if (m_triedLoadCoeffs) {
            return;
        }
        m_triedLoadCoeffs = true;

        const auto path = FindDspCoefficientRom();
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream || stream.tellg() != static_cast<std::streamoff>(m_coeffs.size() * 2)) {
            throw std::runtime_error("Bundled Wii DSP coefficient ROM has an invalid size: " + path.string());
        }
        stream.seekg(0);
        std::array<uint8_t, kResamplingCoefficientCount * 2> bytes{};
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
            throw std::runtime_error("Failed to read bundled Wii DSP coefficient ROM: " + path.string());
        }
        for (size_t i = 0; i < m_coeffs.size(); ++i) {
            const uint16_t word = static_cast<uint16_t>(bytes[i * 2]) << 8 |
                                  static_cast<uint16_t>(bytes[i * 2 + 1]);
            m_coeffs[i] = static_cast<int16_t>(word);
        }
        m_hasCoeffs = true;
        // Four int16 taps times int16 samples sum within int32 when no row's absolute
        // coefficients add up past 0xffff, which lets the resampler skip 64-bit multiplies.
        m_coeffsFit32 = true;
        for (size_t row = 0; row < m_coeffs.size(); row += 4) {
            int32_t sum = 0;
            for (size_t k = 0; k < 4; ++k) {
                sum += std::abs(static_cast<int32_t>(m_coeffs[row + k]));
            }
            if (sum > 0xffff) {
                m_coeffsFit32 = false;
            }
        }
    }

    static std::array<PBUpdate, 32> LoadPBUpdates(const AXPBWii& pb) {
        std::array<PBUpdate, 32> updates{};
        const uint32_t addr = Hilo(pb.updates.data_hi, pb.updates.data_lo);
        if (addr == 0) {
            return updates;
        }
        for (size_t i = 0; i < updates.size(); ++i) {
            updates[i].pb_offset = MixRead16(addr + static_cast<uint32_t>(i * sizeof(PBUpdate)));
            updates[i].new_value = MixRead16(addr + static_cast<uint32_t>(i * sizeof(PBUpdate) + sizeof(uint16_t)));
        }
        return updates;
    }

    void ApplyPBUpdatesForMs(AXPBWii& pb, const std::array<PBUpdate, 32>& updates, uint32_t ms) {
        uint32_t start = 0;
        for (uint32_t i = 0; i < ms; ++i) {
            start += pb.updates.num_updates[i];
        }
        const uint32_t count = pb.updates.num_updates[ms];
        uint16_t* words = reinterpret_cast<uint16_t*>(&pb);
        constexpr uint32_t wordCount = sizeof(AXPBWii) / sizeof(uint16_t);
        for (uint32_t i = start; i < start + count && i < updates.size(); ++i) {
            if (updates[i].pb_offset >= wordCount) {
                continue;
            }
            words[updates[i].pb_offset] = updates[i].new_value;
        }
    }

    static void AdvanceBuffers(AXBuffers& buffers, uint32_t regular, uint32_t wiimote) {
        for (size_t i = 0; i < kAxRegularBusCount; ++i) {
            buffers[i] += regular;
        }
        for (size_t i = kAxRegularBusCount; i < buffers.size(); ++i) {
            buffers[i] += wiimote;
        }
    }

    static bool LooksLikeMainRamPointer(uint32_t addr) {
        return addr == 0 || MixContains(addr, sizeof(uint16_t) * 8);
    }

    static bool LooksLikeAxSampleFormat(uint16_t format) {
        return format == 0 || format == 0x19 || format == 0x0a;
    }

    static int ScorePBLayout(uint32_t addr, const AXPBWii& pb) {
        int score = 0;
        const uint32_t next = Hilo(pb.next_pb_hi, pb.next_pb_lo);
        const uint32_t self = Hilo(pb.this_pb_hi, pb.this_pb_lo);
        const uint32_t ratio = Hilo(pb.src.ratio_hi, pb.src.ratio_lo);
        const uint32_t loop = Hilo(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo);
        const uint32_t end = Hilo(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo);
        const uint32_t current = Hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo);

        score += LooksLikeMainRamPointer(next) ? 8 : -16;
        score += self == addr ? 12 : (LooksLikeMainRamPointer(self) ? 2 : -8);
        score += pb.running <= 1 ? 8 : -16;
        score += pb.running == 1 ? 16 : 0;
        score += pb.is_stream <= 1 ? 3 : -4;
        score += pb.src_type <= 2 ? 8 : -10;
        score += LooksLikeAxSampleFormat(pb.audio_addr.sample_format) ? 14 : -12;
        score += ratio > 0 && ratio <= 0x00040000u ? 12 : (pb.running == 1 ? -12 : 0);
        score += (loop & 0xfc000000u) == 0 ? 5 : -5;
        score += (end & 0xfc000000u) == 0 ? 5 : -5;
        score += (current & 0xc0000000u) == 0 || (current & 0xc0000000u) == 0x80000000u ? 5 : -5;
        score += end >= loop ? 4 : -4;
        return score;
    }

    PBLayout DetectPBLayout(uint32_t addr) {
        struct Candidate {
            PBLayout layout;
            AXPBWii pb;
            int score;
        };
        std::array<Candidate, 3> candidates{{
            {PBLayout::SkipUpdatesOnly, {}, 0},
            {PBLayout::SkipUpdatesAndBiquadGap, {}, 0},
            {PBLayout::SkipBiquadGapOnly, {}, 0},
        }};
        Candidate* best = nullptr;
        for (Candidate& candidate : candidates) {
            ReadPBWithLayout(addr, candidate.pb, candidate.layout);
            candidate.score = ScorePBLayout(addr, candidate.pb);
            if (candidate.pb.running == 1 && (!best || candidate.score > best->score)) {
                best = &candidate;
            }
        }
        if (!best || best->score < 40) {
            return PBLayout::Unknown;
        }
        return best->layout;
    }

    void ReadPB(uint32_t addr, AXPBWii& pb) {
        PBLayout layout = m_pbLayout;
        if (layout == PBLayout::Unknown) {
            layout = DetectPBLayout(addr);
            if (layout != PBLayout::Unknown) {
                m_pbLayout = layout;
            } else {
                layout = PBLayout::SkipUpdatesOnly;
            }
        }
        ReadPBWithLayout(addr, pb, layout);
    }

    void ConfigureFromTaskLocked(uint32_t taskPtr) {
        if (taskPtr == 0) {
            return;
        }

        const uint32_t iramAddr = ReadGuestU32OrZero(taskPtr + 0x0cu);
        const uint32_t iramLength = ReadGuestU32OrZero(taskPtr + 0x10u);
        const uint32_t crc = HashEctorGuest(iramAddr, iramLength);
        if (crc == 0) {
            return;
        }

        m_ucodeCrc = crc;
        const PBLayout derivedPBLayout = PBLayoutForUCode(crc);
        if (derivedPBLayout != PBLayout::Unknown) {
            m_pbLayout = derivedPBLayout;
        }
        m_commandLayout = CommandLayoutForUCode(crc);
    }

    // PB write-back goes straight into guest memory even from the worker, deliberately:
    // the CPU's only PB chain access is __AXServiceVPB, reached from __AXSyncPBs before
    // the mail that starts a mix (and after the previous mix was joined), so guest and mix
    // never touch a PB concurrently. Shadowing it would only risk a stale-snapshot clobber.
    void WritePB(uint32_t addr, const AXPBWii& pb) const {
        if (m_pbLayout == PBLayout::Unknown) {
            return;
        }
        WritePBWithLayout(addr, pb, m_pbLayout);
    }

    void RunCompressor(uint16_t threshold, uint16_t releaseFrames, uint32_t tableAddr) {
        bool triggered = false;
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            const int64_t left = m_bus[kBusMainL][i];
            const int64_t right = m_bus[kBusMainR][i];
            if ((left < 0 ? -left : left) > threshold || (right < 0 ? -right : right) > threshold) {
                triggered = true;
                break;
            }
        }

        constexpr uint32_t kAttackEntryCount = 11;
        const uint32_t frameByteSize = kAxSamplesPerFrame * sizeof(uint16_t);
        uint32_t tableOffset = 0;
        if (triggered) {
            tableOffset = m_compressorPos * frameByteSize;
            m_compressorPos = releaseFrames;
        } else if (m_compressorPos != 0) {
            --m_compressorPos;
            tableOffset = (kAttackEntryCount + m_compressorPos) * frameByteSize;
        } else {
            return;
        }

        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            const uint16_t coef = MixRead16(tableAddr + tableOffset + i * sizeof(uint16_t));
            m_bus[kBusMainL][i] =
                static_cast<int>((static_cast<int64_t>(m_bus[kBusMainL][i]) * coef) >> 15);
            m_bus[kBusMainR][i] =
                static_cast<int>((static_cast<int64_t>(m_bus[kBusMainR][i]) * coef) >> 15);
        }
    }

    void MixAUXSamples(uint32_t auxId, uint32_t writeAddr, uint32_t readAddr, uint16_t volume) {
        std::array<uint16_t, kAxSamplesPerFrame> ramp{};
        GenerateVolumeRamp(ramp.data(), m_lastAuxVolumes[auxId], volume, ramp.size());
        m_lastAuxVolumes[auxId] = volume;
        std::array<int*, 3> src{};
        if (auxId == 0) src = {m_bus[kBusAuxAL].data(), m_bus[kBusAuxAR].data(), m_bus[kBusAuxAS].data()};
        if (auxId == 1) src = {m_bus[kBusAuxBL].data(), m_bus[kBusAuxBR].data(), m_bus[kBusAuxBS].data()};
        if (auxId == 2) src = {m_bus[kBusAuxCL].data(), m_bus[kBusAuxCR].data(), m_bus[kBusAuxCS].data()};
        if (writeAddr) {
            for (int* buffer : src) {
                WriteAuxOutBuffer(writeAddr, buffer, kAxSamplesPerFrame);
                writeAddr += kAxSamplesPerFrame * sizeof(int32_t);
            }
        }
        std::array<int*, 3> dst = {m_bus[kBusMainL].data(), m_bus[kBusMainR].data(), m_bus[kBusMainS].data()};
        std::array<int, kAxSamplesPerFrame> aux{};
        for (int* buffer : dst) {
            ReadAuxInBuffer(readAddr, aux.data(), kAxSamplesPerFrame);
            readAddr += kAxSamplesPerFrame * sizeof(int32_t);
            AxMixKernels::MixAccumRamp32(buffer, aux.data(), ramp.data(), kAxSamplesPerFrame);
        }
    }

    void UploadAUXMixLRSC(int auxId, uint32_t* addresses, uint16_t volume) {
        int* auxLeft = auxId ? m_bus[kBusAuxBL].data() : m_bus[kBusAuxAL].data();
        int* auxRight = auxId ? m_bus[kBusAuxBR].data() : m_bus[kBusAuxAR].data();
        int* auxSurround = auxId ? m_bus[kBusAuxBS].data() : m_bus[kBusAuxAS].data();
        int* auxCBuffer = auxId ? m_bus[kBusAuxCS].data() : m_bus[kBusAuxCR].data();
        WriteAuxOutBuffer(addresses[0], auxLeft, kAxSamplesPerFrame);
        WriteAuxOutBuffer(addresses[0] + kAxSamplesPerFrame * sizeof(int32_t), auxRight, kAxSamplesPerFrame);
        WriteAuxOutBuffer(addresses[0] + 2 * kAxSamplesPerFrame * sizeof(int32_t), auxSurround, kAxSamplesPerFrame);
        WriteAuxOutBuffer(addresses[1], auxCBuffer, kAxSamplesPerFrame);
        std::array<uint16_t, kAxSamplesPerFrame> ramp{};
        GenerateVolumeRamp(ramp.data(), m_lastAuxVolumes[auxId], volume, ramp.size());
        m_lastAuxVolumes[auxId] = volume;
        std::array<int*, 4> mixDest = {m_bus[kBusMainL].data(), m_bus[kBusMainR].data(),
                                       m_bus[kBusMainS].data(), m_bus[kBusAuxCL].data()};
        std::array<int, kAxSamplesPerFrame> temp{};
        for (uint32_t i = 0; i < mixDest.size(); ++i) {
            ReadAuxInBuffer(addresses[2 + i], temp.data(), kAxSamplesPerFrame);
            AxMixKernels::MixAccumRamp32(mixDest[i], temp.data(), ramp.data(),
                                         kAxSamplesPerFrame);
        }
    }

    void OutputSamples(uint32_t lrAddr, uint32_t surroundAddr, uint16_t volume, bool uploadAuxC) {
        std::array<uint16_t, kAxSamplesPerFrame> ramp{};
        GenerateVolumeRamp(ramp.data(), m_lastMainVolume, volume, ramp.size());
        m_lastMainVolume = volume;
        WriteGuestS32Buffer(surroundAddr, m_bus[kBusMainS].data(), kAxSamplesPerFrame);
        if (uploadAuxC) {
            WriteGuestS32Buffer(surroundAddr + kAxSamplesPerFrame * sizeof(int32_t),
                                m_bus[kBusAuxCL].data(), kAxSamplesPerFrame);
        }
        std::array<int16_t, kAxSamplesPerFrame * 2> pcm{};
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            const int16_t left = ClampS16((static_cast<int64_t>(m_bus[kBusMainL][i]) * ramp[i]) >> 15);
            const int16_t right = ClampS16((static_cast<int64_t>(m_bus[kBusMainR][i]) * ramp[i]) >> 15);
            // OUTPUT is destructive on the DSP: the main buses are left holding the ramped,
            // clamped result (not the wide accumulator) so any mixing after it sees that value.
            m_bus[kBusMainL][i] = left;
            m_bus[kBusMainR][i] = right;
            pcm[i * 2] = right;
            pcm[i * 2 + 1] = left;
        }
        // The AI frame (contiguous right/left interleaved) lands in guest memory directly
        // even from the worker: its only reader, PushAudioBlock, runs after Audio_HLE_Tick
        // has joined the worker, so the guest never observes a half-written frame.
        if (uint8_t* host = MixResolveRange(lrAddr, pcm.size() * sizeof(int16_t))) {
            for (size_t i = 0; i < pcm.size(); ++i) {
                BigEndian::Write16(host + i * sizeof(uint16_t), static_cast<uint16_t>(pcm[i]));
            }
        } else {
            for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
                MixWrite16(lrAddr + i * 4, static_cast<uint16_t>(pcm[i * 2]));
                MixWrite16(lrAddr + i * 4 + 2, static_cast<uint16_t>(pcm[i * 2 + 1]));
            }
        }
    }

    void OutputWMSamples(uint32_t* addresses) {
        const AXBuffers all = MixBuffers();
        const std::array<int*, 4> buffers = {all[kBusWm0Main], all[kBusWm1Main], all[kBusWm2Main],
                                             all[kBusWm3Main]};
        for (uint32_t i = 0; i < buffers.size(); ++i) {
            for (uint32_t j = 0; j < kAxWiimoteSamplesPerFrame; ++j) {
                MixWrite16(addresses[i] + j * 2, static_cast<uint16_t>(ClampS16(buffers[i][j])));
            }
        }
    }

    // Aux OUT/IN stay off guest memory during a deferred mix, since __AXProcessAux reads a mix
    // result inside the same __AXOutNewFrame that produced it: OUT shadows to a host buffer
    // JoinMix publishes one AX frame later (a 3 ms older reverb tap, not a dry-signal change),
    // and IN snapshots at mail time to match a synchronous mix. Reads check this frame's shadow
    // first, so a same-buffer write/read still behaves like a synchronous read-back.
    void WriteAuxOutBuffer(uint32_t addr, const int* src, uint32_t count) {
        if (!m_deferredAux) {
            WriteGuestS32Buffer(addr, src, count);
            return;
        }
        if (addr == 0) {
            return;
        }
        if (m_auxOutCount >= m_auxOut.size()) {
            ReportAuxShadowOverflow();
            return;
        }
        AuxBlock& block = m_auxOut[m_auxOutCount++];
        block.addr = addr;
        std::memcpy(block.data.data(), src, count * sizeof(int32_t));
    }

    void ReadAuxInBuffer(uint32_t addr, int* dst, uint32_t count) {
        if (m_deferredAux) {
            for (uint32_t i = m_auxOutCount; i-- > 0;) {
                if (m_auxOut[i].addr == addr) {
                    std::memcpy(dst, m_auxOut[i].data.data(), count * sizeof(int32_t));
                    return;
                }
            }
            for (uint32_t i = 0; i < m_auxInCount; ++i) {
                if (m_auxIn[i].addr == addr) {
                    std::memcpy(dst, m_auxIn[i].data.data(), count * sizeof(int32_t));
                    return;
                }
            }
        }
        ReadGuestS32Buffer(addr, dst, count);
    }

    // Guest thread only, with the worker joined.
    void PublishAuxOutputs() {
        for (uint32_t i = 0; i < m_auxOutCount; ++i) {
            WriteGuestS32Buffer(m_auxOut[i].addr, m_auxOut[i].data.data(), kAxSamplesPerFrame);
        }
        // Publish exactly once: __AXProcessAux may process the aux buffer in
        // place, so replaying the shadow at the next join would overwrite the
        // reverb result the guest just computed.
        m_auxOutCount = 0;
    }

    void ReportAuxShadowOverflow() {
        if (m_loggedAuxShadowOverflow.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        RT_LOGF(RT_TAG_AUDIO, "aux-out shadow overflow; dropped an aux upload\n");
        std::fflush(stderr);
    }

    bool EnsureMixWorker() {
        if (m_mixThread.joinable()) {
            return true;
        }
        if (g_mixMem1.load(std::memory_order_relaxed) == nullptr) {
            return false;
        }
        RT_LOGF(RT_TAG_AUDIO, "starting the AX/DSP mix worker thread\n");
        std::fflush(stderr);
        m_mixStop = false;
        m_mixPending = false;
        m_mixBusy = false;
        try {
            m_mixThread = std::thread([this] { MixWorkerMain(); });
        } catch (const std::system_error&) {
            RT_LOGF(RT_TAG_AUDIO, "failed to start the mix worker; mixing inline\n");
            std::fflush(stderr);
            return false;
        }
        return true;
    }

    void MixWorkerMain() {
        t_onMixWorker = true;
#if defined(__PPU__) || defined(__PS3__)
        // Above the guest and GPU threads (1000): the guest blocks on the mix every audio
        // frame, so the worker must get the other hardware thread as soon as it is woken.
        mkw_lv2_thread_set_priority(990);
#endif
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_mixMutex);
                m_mixWake.wait(lock, [this] { return m_mixStop || m_mixPending; });
                if (!m_mixPending) {
                    return;
                }
                m_mixPending = false;
                m_mixBusy = true;
            }

            const uint64_t start = MixTicks();
            try {
                ExecuteCommandList(m_mixLayout, m_mixCmdListSize);
            } catch (const std::exception& error) {
                ReportMixWorkerFailure(error.what());
            } catch (...) {
                ReportMixWorkerFailure("unknown exception");
            }

            g_mixWorkTicks.fetch_add(MixTicks() - start, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(m_mixMutex);
                m_mixBusy = false;
            }
            m_mixIdle.notify_all();
        }
    }

    void ReportMixWorkerFailure(const char* what) {
        if (m_loggedMixWorkerFailure.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        RT_LOGF(RT_TAG_AUDIO, "worker aborted a command list: %s\n", what);
        std::fflush(stderr);
    }

    mutable std::mutex m_mutex;
    MailState m_mailState = MailState::WaitingForCmdListSize;
    bool m_toDspBusy = false;
    uint32_t m_cmdListSize = 0;
    std::array<uint16_t, 512> m_cmdList{};
    std::deque<uint32_t> m_cpuMails;
    uint32_t m_lastCpuMail = 0;
    uint32_t m_compressorPos = 0;
    uint32_t m_deferredResumeCallbacks = 0;
    uint32_t m_ucodeCrc = 0;
    PBLayout m_pbLayout = PBLayout::Unknown;
    AXCommandLayout m_commandLayout = AXCommandLayout::Auto;
    bool m_triedLoadCoeffs = false;
    bool m_hasCoeffs = false;
    bool m_coeffsFit32 = false;
    // 128-aligned for the SPU mixer's DMA.
    alignas(128) std::array<int16_t, 0x800> m_coeffs{};
    // Indexed by AxBus. The wiimote buses live in their own array because they
    // are 18 samples per frame rather than 96.
    alignas(128) std::array<std::array<int, kAxSamplesPerFrame>, kAxRegularBusCount> m_bus{};
    alignas(128) std::array<std::array<int, kAxWiimoteSamplesPerFrame>, kAxWiimoteBusCount> m_wmBus{};
    uint16_t m_lastMainVolume = 0x8000;
    std::array<uint16_t, 3> m_lastAuxVolumes{};

    // Mix worker handshake.
    std::thread m_mixThread;
    mutable std::mutex m_mixMutex;
    std::condition_variable m_mixWake;  // guest -> worker
    std::condition_variable m_mixIdle;  // worker -> guest
    bool m_mixStop = false;
    bool m_mixPending = false;
    bool m_mixBusy = false;
    bool m_mixWorkerEnabled = false;
    AXCommandLayout m_mixLayout = AXCommandLayout::New;
    uint32_t m_mixCmdListSize = 0;
    std::atomic<bool> m_loggedMixWorkerFailure{false};
    std::atomic<bool> m_loggedAuxShadowOverflow{false};
    std::atomic<bool> m_loggedUnknownCommand{false};
    std::atomic<bool> m_loggedCmdListTruncated{false};
    std::atomic<bool> m_loggedSpuBadAccess{false};

    // An SPU mix in flight (DispatchCommandList to FinishSpuMix), and where the command
    // list carries on after it.
    bool m_spuPending = false;
    uint32_t m_spuResumeIdx = 0;
    uint32_t m_spuOldPbAddr = 0;

    // Aux marshalling. m_deferredAux is written by the guest thread before the
    // worker is signalled and read by whichever thread runs the mix.
    bool m_deferredAux = false;
    uint32_t m_auxOutCount = 0;
    uint32_t m_auxInCount = 0;
    std::array<AuxBlock, 24> m_auxOut{};
    std::array<AuxBlock, 24> m_auxIn{};
};

AXWii& Instance() {
    static AXWii ax;
    return ax;
}

} // namespace

void Init() {
    InitAram();
    Instance().Initialize();
}

void InitForAXOut(CpuContext* ctx) {
    Init();
    g_axTaskPtr = kAxDspTaskAddr;
    Memory::Write32(g_axTaskPtr + 0x00u, 1);
    Memory::Write32(g_axTaskPtr + 0x04u, 0);
    Memory::Write32(g_axTaskPtr + 0x0cu, kAxIramMmemAddr);
    Memory::Write32(g_axTaskPtr + 0x14u, 0);
    Memory::Write32(g_axTaskPtr + 0x18u, kAxDramMmemAddr);
    Memory::Write32(g_axTaskPtr + 0x1cu, kAxDramLength);
    Memory::Write32(g_axTaskPtr + 0x20u, kAxDramDspAddr);
    Memory::Write32(g_axTaskPtr + 0x28u, kAxInitCallback);
    Memory::Write32(g_axTaskPtr + 0x2cu, kAxResumeCallback);
    Memory::Write32(g_axTaskPtr + 0x30u, kAxDoneCallback);
    Memory::Write32(g_axTaskPtr + 0x34u, kAxRequestCallback);
    if (ctx) {
        const uint32_t r13 = ctx->gpr[13];
        Memory::Write32(g_axTaskPtr + 0x10u, Memory::Read16(r13 - 0x73fcu));
        Memory::Write16(g_axTaskPtr + 0x24u, Memory::Read16(r13 - 0x7400u));
        Memory::Write16(g_axTaskPtr + 0x26u, Memory::Read16(r13 - 0x73feu));
    }
    Instance().ConfigureFromTask(g_axTaskPtr);
    LinkSingleDspTask(g_axTaskPtr);
    if (ctx) {
        const uint32_t r13 = ctx->gpr[13];
        Memory::Write32(r13 - 0x66d8u, 1);
        Memory::Write32(r13 - 0x66dcu, 0);
    }
}

void Stop() {
    Instance().Stop();
}

uint32_t CheckInit() {
    return ReadGuestU32OrZero(kDspInitializedAddr);
}

uint32_t AddTask(uint32_t taskPtr) {
    if (taskPtr == 0) {
        return 0;
    }

    MarkDspInitialized();
    Instance().ConfigureFromTask(taskPtr);
    const uint32_t firstTask = ReadGuestU32OrZero(kDspFirstTaskAddr);
    if (firstTask == 0) {
        LinkSingleDspTask(taskPtr);
    } else {
        const uint32_t oldCurrent = ReadGuestU32OrZero(kDspCurrentTaskAddr);
        Memory::TryWrite32(oldCurrent + 0x38u, taskPtr);
        Memory::TryWrite32(taskPtr + 0x38u, 0);
        Memory::TryWrite32(taskPtr + 0x3cu, oldCurrent);
        Memory::TryWrite32(kDspCurrentTaskAddr, taskPtr);
    }
    Memory::TryWrite32(taskPtr + 0x00u, 0);
    Memory::TryWrite32(taskPtr + 0x08u, 1);
    if (taskPtr == ReadGuestU32OrZero(kDspFirstTaskAddr)) {
        AssertTask(taskPtr);
    }
    return taskPtr;
}

void SendMailToDSP(uint32_t mail) {
    Instance().SendMail(mail);
}

uint32_t CheckMailToDSP() {
    return Instance().CheckMailToDSP();
}

uint32_t CheckMailFromDSP() {
    return Instance().CheckMailFromDSP();
}

uint32_t ReadMailFromDSP() {
    return Instance().ReadMailFromDSP();
}

uint32_t AssertTask(uint32_t taskPtr) {
    if (taskPtr != 0) {
        g_axTaskPtr = taskPtr;
        Memory::TryWrite32(kDspAssertPendingAddr, 1);
        Memory::TryWrite32(kDspAssertTaskAddr, taskPtr);
        Memory::TryWrite32(kDspRunningTaskAddr, taskPtr);
        Memory::Write32(taskPtr, 1);
        Instance().QueueResumeCallback();
    }
    return taskPtr;
}

void ServiceDeferredCallbacks() {
    Instance().ServiceDeferredCallbacks();
}

void InitAram() {
    // Retire cached host sample windows: the guest memory map may have been rebuilt since
    // the last AX session, so the worker (which caches a window thread_local) must be idle
    // across the generation bump and region-base refresh.
    Instance().JoinMix();
    RefreshMixMemoryMap();
    g_aramWindowGeneration.fetch_add(1, std::memory_order_relaxed);
}

void JoinMixWorker() {
    Instance().JoinMix();
}

void ShutdownMixWorker() {
    Instance().ShutdownMixWorker();
}

void SetMixWorkerEnabled(bool enabled) {
    Instance().SetMixWorkerEnabled(enabled);
}

} // namespace AxDspHle
