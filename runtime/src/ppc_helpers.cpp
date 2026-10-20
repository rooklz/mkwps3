#include "ppc_runtime.h"
// ppc_runtime.h re-exports the rest of the ISA package; the CR tier is pulled in
// by abi_bridge.h for generated code, so this TU asks for it directly.
#include "isa/ppc_isa_cr.h"
#include "memory.h"
#include "runtime_log.h"
#include "timebase_contract.h"

#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <limits>


extern "C" void PPC_TrapWord(uint32_t trapOptions, uint32_t lhs, uint32_t rhs)
{
    const bool trap =
        ((trapOptions & 0x10u) != 0u && static_cast<int32_t>(lhs) < static_cast<int32_t>(rhs)) ||
        ((trapOptions & 0x08u) != 0u && static_cast<int32_t>(lhs) > static_cast<int32_t>(rhs)) ||
        ((trapOptions & 0x04u) != 0u && lhs == rhs) ||
        ((trapOptions & 0x02u) != 0u && lhs < rhs) ||
        ((trapOptions & 0x01u) != 0u && lhs > rhs);
    if (trap)
    {
        throw std::runtime_error("PowerPC trap condition fired");
    }
}

namespace {

constexpr uint32_t kBroadwayPvr = 0x00087200u; // Known PVR value for Wii's Broadway CPU.

// One warning per (SPR, direction). A bitset rather than bool arrays: an
// unhandled SPR is a real translation gap worth reporting once per SPR, but the
// dedup state has no business costing 4 KiB of .bss.
std::bitset<2048> g_warnedSprRead{};
std::bitset<2048> g_warnedSprWrite{};
std::array<uint32_t, 2048> g_sprShadow{};
uint32_t g_reservationAddr = 0;
bool g_hasReservation = false;
// The Wii timebase runs at 60.75 MHz; the Cell's at 79.8 MHz. Reading the
// host register and rescaling (60.75/79.8 = 405/532) costs a few cycles,
// where steady_clock here is gettimeofday, a system call.
inline uint64_t HostTimeBase()
{
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

const uint64_t g_timeBaseStart = HostTimeBase();

uint64_t GetTimeBase() {
    const uint64_t elapsed = HostTimeBase() - g_timeBaseStart;
    return (elapsed / 532u) * 405u + ((elapsed % 532u) * 405u) / 532u;
}

constexpr uint32_t kFpscrFx = 1u << 31;
constexpr uint32_t kFpscrFex = 1u << 30;
constexpr uint32_t kFpscrVx = 1u << 29;
constexpr uint32_t kFpscrOx = 1u << 28;
constexpr uint32_t kFpscrUx = 1u << 27;
constexpr uint32_t kFpscrZx = 1u << 26;
constexpr uint32_t kFpscrXx = 1u << 25;
constexpr uint32_t kFpscrVxSnan = 1u << 24;
constexpr uint32_t kFpscrVxIsi = 1u << 23;
constexpr uint32_t kFpscrVxIdi = 1u << 22;
constexpr uint32_t kFpscrVxZdz = 1u << 21;
constexpr uint32_t kFpscrVxImz = 1u << 20;
constexpr uint32_t kFpscrVxVc = 1u << 19;
constexpr uint32_t kFpscrVxSoft = 1u << 10;
constexpr uint32_t kFpscrVxSqrt = 1u << 9;
constexpr uint32_t kFpscrVxCvi = 1u << 8;
constexpr uint32_t kFpscrVe = 1u << 7;
constexpr uint32_t kFpscrOe = 1u << 6;
constexpr uint32_t kFpscrUe = 1u << 5;
constexpr uint32_t kFpscrZe = 1u << 4;
constexpr uint32_t kFpscrXe = 1u << 3;
constexpr uint32_t kFpscrVxAny =
    kFpscrVxSnan | kFpscrVxIsi | kFpscrVxIdi | kFpscrVxZdz | kFpscrVxImz |
    kFpscrVxVc | kFpscrVxSoft | kFpscrVxSqrt | kFpscrVxCvi;
constexpr uint32_t kFpscrAnyX = kFpscrOx | kFpscrUx | kFpscrZx | kFpscrXx | kFpscrVxAny;
constexpr uint32_t kFpscrAnyE = kFpscrVe | kFpscrOe | kFpscrUe | kFpscrZe | kFpscrXe;

template <size_t N>
inline void WarnOnce(std::bitset<N>& warned, uint32_t spr, const char* op)
{
    if (spr < N)
    {
        if (warned.test(spr))
        {
            return;
        }
        warned.set(spr);
    }
    RT_LOG(RT_TAG_RUNTIME) << "PPC_" << op << " unhandled SPR " << spr << " (defaulting to 0)" << std::endl;
}



inline void UpdateFpscrSummary(CpuContext* cpu)
{
    if (!cpu)
    {
        return;
    }

    if ((cpu->fpscr & kFpscrVxAny) != 0)
    {
        cpu->fpscr |= kFpscrVx;
    }
    else
    {
        cpu->fpscr &= ~kFpscrVx;
    }

    if (((cpu->fpscr >> 22) & (cpu->fpscr & kFpscrAnyE)) != 0)
    {
        cpu->fpscr |= kFpscrFex;
    }
    else
    {
        cpu->fpscr &= ~kFpscrFex;
    }
}

} // namespace

extern "C" uint32_t PPC_UpdateCarryAdd(uint32_t lhs, uint32_t rhs, uint32_t carryIn)
{
    const uint64_t sum = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs) + static_cast<uint64_t>(carryIn & 1u);
    const uint32_t carryOut = static_cast<uint32_t>((sum >> 32) & 1u);
    CpuContext* cpu = TryGetCpuContext();
    if (cpu)
    {
        cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        return cpu->xer;
    }

    return carryOut << 29;
}

extern "C" uint32_t PPC_UpdateCarrySub(uint32_t lhs, uint32_t rhs)
{
    // Carry flag is set when no borrow occurs (i.e., lhs >= rhs).
    const uint32_t carryOut = lhs >= rhs ? 1u : 0u;
    CpuContext* cpu = TryGetCpuContext();
    if (cpu)
    {
        cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        return cpu->xer;
    }

    return carryOut << 29;
}

extern "C" uint32_t PPC_UpdateCarryShiftRight(uint32_t value, uint32_t shift)
{
    uint32_t carryOut = 0;

    // Per Broadway docs: CA is set if value is NEGATIVE and any 1 bits shifted out
    const bool isNegative = (value & 0x80000000u) != 0;

    if ((shift & 0x20u) != 0)
    {
        carryOut = isNegative ? 1u : 0u;
    }
    else
    {
        const uint32_t sh = shift & 31u;
        if (sh != 0 && isNegative)
        {
            const uint32_t mask = (1u << sh) - 1u;
            carryOut = (value & mask) ? 1u : 0u;
        }
    }

    CpuContext* cpu = TryGetCpuContext();
    if (cpu)
    {
        cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        return cpu->xer;
    }

    return carryOut << 29;
}

extern "C" uint32_t PPC_GetCarry()
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }
    return (cpu->xer >> 29) & 1u;
}

static void UpdateOverflow(uint32_t result, bool overflow)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return;
    }

    if (overflow)
    {
        cpu->xer |= 0xC0000000u; // SO and OV.
    }
    else
    {
        cpu->xer &= ~0x40000000u; // Clear OV, preserve SO.
    }
    (void)result;
}

static uint32_t AddWithXer(uint32_t lhs, uint32_t rhs, uint32_t carryIn, bool updateCarry)
{
    const uint64_t wide = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs) + static_cast<uint64_t>(carryIn & 1u);
    const uint32_t result = static_cast<uint32_t>(wide);
    const bool overflow = ((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) != 0;
    if (updateCarry)
    {
        PPC_UpdateCarryAdd(lhs, rhs, carryIn);
    }
    UpdateOverflow(result, overflow);
    return result;
}

static uint32_t SubfWithXer(uint32_t subtrahend, uint32_t minuend, uint32_t carryIn, bool extended, bool updateCarry)
{
    const uint32_t rhs = ~subtrahend;
    const uint64_t wide = static_cast<uint64_t>(rhs) + static_cast<uint64_t>(minuend) + static_cast<uint64_t>(extended ? (carryIn & 1u) : 1u);
    const uint32_t result = static_cast<uint32_t>(wide);
    const bool overflow = (((minuend ^ subtrahend) & (minuend ^ result)) & 0x80000000u) != 0;
    if (updateCarry)
    {
        CpuContext* cpu = TryGetCpuContext();
        const uint32_t carryOut = static_cast<uint32_t>((wide >> 32) & 1u);
        if (cpu)
        {
            cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        }
    }
    UpdateOverflow(result, overflow);
    return result;
}

extern "C" uint32_t PPC_Addo(uint32_t lhs, uint32_t rhs) { return AddWithXer(lhs, rhs, 0, false); }
extern "C" uint32_t PPC_Addco(uint32_t lhs, uint32_t rhs) { return AddWithXer(lhs, rhs, 0, true); }
extern "C" uint32_t PPC_Addeo(uint32_t lhs, uint32_t rhs) { return AddWithXer(lhs, rhs, PPC_GetCarry(), true); }
extern "C" uint32_t PPC_Addmeo(uint32_t value) { return AddWithXer(value, 0xFFFFFFFFu, PPC_GetCarry(), true); }
extern "C" uint32_t PPC_Addzeo(uint32_t value) { return AddWithXer(value, 0, PPC_GetCarry(), true); }
extern "C" uint32_t PPC_Subfo(uint32_t subtrahend, uint32_t minuend) { return SubfWithXer(subtrahend, minuend, 0, false, false); }
extern "C" uint32_t PPC_Subfco(uint32_t subtrahend, uint32_t minuend) { return SubfWithXer(subtrahend, minuend, 0, false, true); }
extern "C" uint32_t PPC_Subfeo(uint32_t subtrahend, uint32_t minuend) { return SubfWithXer(subtrahend, minuend, PPC_GetCarry(), true, true); }
extern "C" uint32_t PPC_Subfmeo(uint32_t value) { return SubfWithXer(value, 0xFFFFFFFFu, PPC_GetCarry(), true, true); }
extern "C" uint32_t PPC_Subfzeo(uint32_t value) { return SubfWithXer(value, 0, PPC_GetCarry(), true, true); }
extern "C" uint32_t PPC_Nego(uint32_t value) { return SubfWithXer(value, 0, 0, false, false); }

extern "C" uint32_t PPC_Mullwo(uint32_t lhs, uint32_t rhs)
{
    const int64_t product = static_cast<int64_t>(static_cast<int32_t>(lhs)) * static_cast<int64_t>(static_cast<int32_t>(rhs));
    const uint32_t result = static_cast<uint32_t>(product);
    UpdateOverflow(result, product < std::numeric_limits<int32_t>::min() || product > std::numeric_limits<int32_t>::max());
    return result;
}

extern "C" uint32_t PPC_Divwo(uint32_t lhs, uint32_t rhs)
{
    const int32_t dividend = static_cast<int32_t>(lhs);
    const int32_t divisor = static_cast<int32_t>(rhs);
    if (divisor == 0 || (dividend == std::numeric_limits<int32_t>::min() && divisor == -1))
    {
        const uint32_t result = dividend < 0 ? 0xFFFFFFFFu : 0u;
        UpdateOverflow(result, true);
        return result;
    }
    const uint32_t result = static_cast<uint32_t>(dividend / divisor);
    UpdateOverflow(result, false);
    return result;
}

extern "C" uint32_t PPC_Divwuo(uint32_t lhs, uint32_t rhs)
{
    if (rhs == 0)
    {
        UpdateOverflow(0, true);
        return 0;
    }
    const uint32_t result = lhs / rhs;
    UpdateOverflow(result, false);
    return result;
}

static void LoadString(uint32_t rD, uint32_t addr, uint32_t byteCount)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return;
    }

    uint32_t reg = rD & 31u;
    uint32_t current = 0;
    for (uint32_t i = 0; i < byteCount; ++i)
    {
        current = (current << 8) | Memory::Read8(addr + i);
        if ((i & 3u) == 3u)
        {
            cpu->gpr[reg] = current;
            reg = (reg + 1u) & 31u;
            current = 0;
        }
    }

    const uint32_t partial = byteCount & 3u;
    if (partial != 0)
    {
        cpu->gpr[reg] = current << ((4u - partial) * 8u);
    }
}

static void StoreString(uint32_t rS, uint32_t addr, uint32_t byteCount)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return;
    }

    uint32_t reg = rS & 31u;
    for (uint32_t i = 0; i < byteCount; ++i)
    {
        const uint32_t shift = 24u - ((i & 3u) * 8u);
        Memory::Write8(addr + i, static_cast<uint8_t>(cpu->gpr[reg] >> shift));
        if ((i & 3u) == 3u)
        {
            reg = (reg + 1u) & 31u;
        }
    }
}

extern "C" void PPC_Lswi(uint32_t rD, uint32_t addr, uint32_t byteCount) { LoadString(rD, addr, byteCount == 0 ? 32u : byteCount); }
extern "C" void PPC_Lswx(uint32_t rD, uint32_t addr)
{
    CpuContext* cpu = TryGetCpuContext();
    LoadString(rD, addr, cpu ? (cpu->xer & 0x7Fu) : 0u);
}
extern "C" void PPC_Stswi(uint32_t rS, uint32_t addr, uint32_t byteCount) { StoreString(rS, addr, byteCount == 0 ? 32u : byteCount); }
extern "C" void PPC_Stswx(uint32_t rS, uint32_t addr)
{
    CpuContext* cpu = TryGetCpuContext();
    StoreString(rS, addr, cpu ? (cpu->xer & 0x7Fu) : 0u);
}

extern "C" uint32_t PPC_Lwarx(uint32_t addr)
{
    g_reservationAddr = addr;
    g_hasReservation = true;
    return Memory::Read32(addr);
}

extern "C" uint32_t PPC_Stwcx(uint32_t addr, uint32_t value)
{
    CpuContext* cpu = TryGetCpuContext();
    const bool success = g_hasReservation && g_reservationAddr == addr;
    g_hasReservation = false;
    if (success)
    {
        Memory::Write32(addr, value);
    }
    if (cpu)
    {
        const uint32_t so = (cpu->xer >> 31) & 1u;
        const uint32_t cr0 = (success ? 0x2u : 0u) | so;
        cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (cr0 << 28);
    }
    return success ? 1u : 0u;
}

extern "C" uint32_t PPC_Mcrfs(uint32_t dstField, uint32_t srcField)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    const uint32_t dst = dstField & 7u;
    const uint32_t src = srcField & 7u;
    const uint32_t value = (cpu->fpscr >> ((7u - src) * 4u)) & 0xFu;
    cpu->cr = (cpu->cr & ~(0xFu << ((7u - dst) * 4u))) | (value << ((7u - dst) * 4u));
    cpu->fpscr &= ~((0xFu << ((7u - src) * 4u)) & (kFpscrFx | kFpscrAnyX));
    UpdateFpscrSummary(cpu);
    return cpu->cr;
}

extern "C" uint32_t PPC_Eciwx(uint32_t addr)
{
    (void)addr;
    throw std::runtime_error("eciwx attempted external control/MMIO read");
}

extern "C" uint32_t PPC_Mftb()
{
    return static_cast<uint32_t>(GetTimeBase());
}

extern "C" uint32_t PPC_Mftbu()
{
    return static_cast<uint32_t>(GetTimeBase() >> 32);
}

extern "C" uint32_t OSSystemCall()
{
    return 0;
}

extern "C" void PPC_Ecowx(uint32_t addr, uint32_t value)
{
    (void)addr;
    (void)value;
    throw std::runtime_error("ecowx attempted external control/MMIO write");
}

extern "C" uint32_t PPC_Mcrxr(uint32_t crField)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    const uint32_t field = crField & 7u;
    const uint32_t value = (cpu->xer >> 28) & 0xFu;
    const uint32_t shift = (7u - field) * 4u;
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (value << shift);
    cpu->xer &= ~0xF0000000u;
    return cpu->cr;
}

extern "C" uint32_t PPC_ReadSpr(uint32_t spr)
{
    CpuContext* cpu = TryGetCpuContext();

    // Preserve previously written values for unknown SPRs so that callers see what they set.
    auto shadowValue = [&]() -> uint32_t
    {
        if (spr < g_sprShadow.size())
        {
            return g_sprShadow[spr];
        }
        return 0u;
    };

    if (spr >= 912u && spr <= 919u)
    {
        return cpu ? cpu->gqr[spr - 912u] : 0u;
    }

    switch (spr)
    {
        case 8: return cpu ? cpu->lr : 0u;
        case 9: return cpu ? cpu->ctr : 0u;
        case 1: return cpu ? cpu->xer : 0u;
        case 22: return shadowValue(); // DEC - emulate as latched value
        case 26: return cpu ? cpu->srr0 : 0u;
        case 27: return cpu ? cpu->srr1 : 0u;
        case 268: return PPC_Mftb();
        case 269: return PPC_Mftbu();
        case 287: return kBroadwayPvr; // PVR
        case 920: return cpu ? cpu->hid2 : 0u;
        case 1008: return cpu ? cpu->hid0 : 0u;
        case 1009: return cpu ? cpu->hid1 : 0u;
        case 1011: return shadowValue(); // spr3f3 in TRK save block
        case 1017: return shadowValue(); // DMA/L2 ancillary SPRs (preserve writes)
        case 952:  // PM registers - preserve writes
        case 953:
        case 954:
        case 956:
        case 957:
        case 958:
            return shadowValue();
        default:
            WarnOnce(g_warnedSprRead, spr, "ReadSpr");
            return shadowValue();
    }
}

extern "C" void PPC_WriteSpr(uint32_t spr, uint32_t value)
{
    CpuContext* cpu = TryGetCpuContext();

    if (spr >= 912u && spr <= 919u)
    {
        if (cpu)
        {
            cpu->gqr[spr - 912u] = value;
        }
        return;
    }

    switch (spr)
    {
        case 8: if (cpu) cpu->lr = value; return;
        case 9: if (cpu) cpu->ctr = value; return;
        case 1: if (cpu) cpu->xer = value; return;
        case 22: break; // DEC - fallthrough to shadow
        case 26: if (cpu) cpu->srr0 = value; return;
        case 27: if (cpu) cpu->srr1 = value; return;
        case 920: if (cpu) cpu->hid2 = value; return;
        case 1008: if (cpu) cpu->hid0 = value; return;
        case 1009: if (cpu) cpu->hid1 = value; return;
        case 1011: break;
        case 1017: break;
        case 952:  // PM registers - preserve values
        case 953:
        case 954:
        case 956:
        case 957:
        case 958:
            break;
        default:
            WarnOnce(g_warnedSprWrite, spr, "WriteSpr");
            break;
    }

    if (spr < g_sprShadow.size())
    {
        g_sprShadow[spr] = value;
    }
}

extern "C" uint32_t PPC_Cntlzw(uint32_t value)
{
    // cntlzw returns 32 when the input is zero; __builtin_clz is undefined in that case.
    if (value == 0)
    {
        return 32u;
    }
    return static_cast<uint32_t>(__builtin_clz(value));
}

// The CR semantics themselves live in isa/ppc_isa_cr.h, which is what generated
// code calls (Ppc*Resident on a register-resident CR). These out-of-line forms
// exist only for the !hasDest lifter path and must not re-implement them.
extern "C" uint32_t PPC_CrSetBit(uint32_t bitIndex, uint32_t value)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    cpu->cr = PpcCrSetBitResident(cpu->cr, bitIndex, value);
    return cpu->cr;
}

extern "C" uint32_t PPC_CrLogical(uint32_t op, uint32_t bt, uint32_t ba, uint32_t bb)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    cpu->cr = PpcCrLogicalResident(cpu->cr, op, bt, ba, bb);
    return cpu->cr;
}

extern "C" uint32_t PPC_Mcrf(uint32_t dstField, uint32_t srcField)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    cpu->cr = PpcMcrfResident(cpu->cr, dstField, srcField);
    return cpu->cr;
}

// The paired-single and scalar float helpers are inline in isa/ppc_isa_float.h
// on the PS3 (two-lane FPRs); only the integer and memory helpers live here.

extern "C" uint32_t PPC_LoadWordByteReverse(uint32_t addr)
{
    // lwbrx: Load Word Byte-Reverse Indexed
    // Loads a 32-bit word from memory and byte-swaps it
    uint32_t value = Memory::Read32(addr);
    
    // Byte-swap: ABCD -> DCBA
    return ((value >> 24) & 0x000000FF) |
           ((value >> 8)  & 0x0000FF00) |
           ((value << 8)  & 0x00FF0000) |
           ((value << 24) & 0xFF000000);
}

extern "C" void PPC_StoreWordByteReverse(uint32_t addr, uint32_t value)
{
    // stwbrx: Store Word Byte-Reverse Indexed
    // Byte-swaps a 32-bit value and stores it to memory
    // Byte-swap: ABCD -> DCBA
    uint32_t swapped = ((value >> 24) & 0x000000FF) |
                       ((value >> 8)  & 0x0000FF00) |
                       ((value << 8)  & 0x00FF0000) |
                       ((value << 24) & 0xFF000000);
    
    Memory::Write32(addr, swapped);
}

extern "C" uint32_t PPC_LoadHalfwordByteReverse(uint32_t addr)
{
    // lhbrx: Load Halfword Byte-Reverse Indexed
    // Loads a 16-bit halfword from memory and byte-swaps it, zero-extending to 32 bits
    uint16_t value = Memory::Read16(addr);
    
    // Byte-swap: AB -> BA
    uint16_t swapped = ((value >> 8) & 0x00FF) |
                       ((value << 8) & 0xFF00);
    return static_cast<uint32_t>(swapped);
}

extern "C" void PPC_StoreHalfwordByteReverse(uint32_t addr, uint32_t value)
{
    // sthbrx: Store Halfword Byte-Reverse Indexed
    // Byte-swaps the lower 16 bits of the value and stores it to memory
    uint16_t halfword = static_cast<uint16_t>(value & 0xFFFF);
    
    // Byte-swap: AB -> BA
    uint16_t swapped = ((halfword >> 8) & 0x00FF) |
                       ((halfword << 8) & 0xFF00);
    Memory::Write16(addr, swapped);
}

// psq_l / psq_st with a runtime GQR index. The lifter only reaches these when
// w/i are non-constant, which never happens in this DOL - every emitted site is
// rewritten to the isa templates in ppc_isa_quantized.h. The bodies below defer
// to the same isa runtime-GQR dispatch so the two can never drift.
extern "C" FprVal PPC_PsqL(uint32_t addr, uint32_t w, uint32_t i)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
        std::abort();
    const uint32_t gqr = cpu->gqr[i & 7];
    uint8_t* host = MemoryInline::GetPointerFast(addr, w == 0 ? 8u : 4u);
    if (!host) {
        // Not plain RAM: read through the checked accessors.
        const uint32_t type = (gqr >> 16) & 7u;
        if (type == 0u) {
            const float ps0 = PpcBitCastToFloatInline(Memory::Read32(addr));
            const float ps1 = w == 0 ? PpcBitCastToFloatInline(Memory::Read32(addr + 4)) : 1.0f;
            return PpcPackPairedInline(ps0, ps1);
        }
        std::abort();
    }
    return w == 0 ? PsqDetail::LoadGeneric<0u, 0u>(gqr, host, addr)
                  : PsqDetail::LoadGeneric<1u, 0u>(gqr, host, addr);
}

extern "C" void PPC_PsqSt(uint32_t addr, FprVal value, uint32_t w, uint32_t i)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
        std::abort();
    const uint32_t gqr = cpu->gqr[i & 7];
    uint8_t* host = MemoryInline::FlatWriteNeedsPolicy(addr)
                        ? nullptr
                        : MemoryInline::GetPointerFast(addr, w == 0 ? 8u : 4u);
    if (!host) {
        // MMIO (the GX FIFO) or unbacked: store through the checked path.
        if ((gqr & 7u) == 0u) {
            Memory::WriteFloat32(addr, value.ps0);
            if (w == 0)
                Memory::WriteFloat32(addr + 4, value.ps1);
            return;
        }
        std::abort();
    }
    if (w == 0)
        PsqDetail::StoreGeneric<0u, 0u>(gqr, host, addr, value);
    else
        PsqDetail::StoreGeneric<1u, 0u>(gqr, host, addr, value);
}

extern "C" int32_t memset_zero_32(int32_t address)
{
    // dcbz zeros a 32-byte block.
    // The translator aligns the address to 32 bytes before calling this.
    try {
        // Try fast path: get direct pointer to memory
        uint8_t* ptr = Memory::GetPointer(static_cast<uint32_t>(address), 32);
        std::memset(ptr, 0, 32);
    } catch (...) {
        // Fallback or MMIO (rare for dcbz): write using scalar helpers
        uint32_t addr = static_cast<uint32_t>(address);
        for (int i = 0; i < 8; ++i) {
             Memory::Write32(addr + i * 4, 0);
        }
    }
    return 0;
}

// Floating-point comparison (fcmpu/fcmpo)
// Sets the specified CR field based on comparing two double values.
extern "C" void PPC_Fcmp(uint32_t crField, double a, double b)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        return;
    }
    SetCRFloat(cpu, crField & 7, a, b);
}

// ps_cmpo and ps_cmpu differ only in which FPSCR invalid-operation bit a NaN
// operand raises (VXVC vs VXSNAN). This runtime never models FPSCR exception
// bits from a compare - SetCRFloat writes the CR field and nothing else - so the
// ordered and unordered forms are deliberately the same body. If FPSCR exception
// state ever becomes observable, these four need to diverge.
extern "C" void PPC_PsCmpo0(uint32_t crField, FprVal a, FprVal b)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        return;
    }
    SetCRFloat(cpu, crField & 7, a.ps0, b.ps0);
}

extern "C" void PPC_PsCmpu0(uint32_t crField, FprVal a, FprVal b)
{
    PPC_PsCmpo0(crField, a, b);
}

extern "C" void PPC_PsCmpo1(uint32_t crField, FprVal a, FprVal b)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        return;
    }
    SetCRFloat(cpu, crField & 7, a.ps1, b.ps1);
}

extern "C" void PPC_PsCmpu1(uint32_t crField, FprVal a, FprVal b)
{
    PPC_PsCmpo1(crField, a, b);
}

// ps_nabs and ps_mr are inline in isa/ppc_isa_float.h.
