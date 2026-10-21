using System;
using System.Collections.Generic;
using System.Globalization;

namespace Translator.Core.Analysis;

/// <summary>
/// Trusts the PowerPC EABI for what a caller can observe after a call returns: the stack and small-data
/// pointers, the r3/r4 and f1 return registers, the nonvolatile r14-r31, f14-f31 and CR fields 2-4.
/// Every other register a callee writes is dead at the return by the ABI, so a contract narrowed to the
/// visible set lets the callee skip writing those back to <c>CpuContext</c> and its callers skip
/// flushing and reloading them around the call. On the Cell PPE every such store/load pair is a
/// load-hit-store, which is where much of a translated call's cost went.
///
/// Compiler-generated code follows the ABI by construction. Hand-written code that deliberately hands
/// over the whole register file (the OS context switch) must keep exact contracts and is exempted by
/// address range. <c>MKW_TRUST_EABI=0</c> turns the narrowing off; <c>MKW_EABI_EXEMPT</c> lists exempt
/// ranges as <c>start-end</c> hex pairs separated by commas (default: Mario Kart Wii PAL's OSContext).
///
/// Two further steps rest on the same trust. A nonvolatile register a body saves before it writes it
/// (it reads it first: the prologue store) comes back unchanged, so it leaves the write set and a
/// caller keeps its own copy instead of reloading it (<c>MKW_EABI_PRESERVE=0</c> disables). And a
/// callee that needs the complete context -- a function-pointer call, an HLE that touches
/// <c>CpuContext</c> -- is still an EABI function at its boundary: the caller publishes everything
/// and reloads only r3, r4 and f1, and the need does not spread to the caller's own callers
/// (<c>MKW_EABI_FENCES=0</c> disables).
///
/// Taken one step further, r14-r31 and f14-f31 need not carry anything into or out of a body: its
/// caller keeps its own values in its own locals across the call, and compiled code only reads them to
/// save them. So a body could start them at zero, never publish them, and its callers never flush them
/// (<c>MKW_EABI_LOCAL_NV=1</c> enables; off by default). Jump-table cases the map lists as functions
/// break the premise -- they run on their parent's live registers -- and are recognized by
/// <see cref="GuestAbiContract.NonvolatileGprInputMask"/>, but the gain measured nil, so it stays opt-in.
/// </summary>
public static class GuestEabiVisibility
{
    public const uint VisibleGpr = 0xFFFFE01Eu;   // r1-r4, r13-r31
    public const uint VisibleFpr = 0xFFFFC002u;   // f1, f14-f31
    public const byte VisibleCr = 0x1C;           // cr2-cr4

    public const uint ReturnGpr = 0x00000018u;       // r3, r4
    public const uint ReturnFpr = 0x00000002u;       // f1
    public const uint NonvolatileGpr = 0xFFFFE006u;  // r1, r2, r13-r31
    public const uint NonvolatileFpr = 0xFFFFC000u;  // f14-f31

    // What an EABI callee can consume: arguments, the base registers and every nonvolatile (it may
    // save them). r11/r12 too, which the pointer-to-member thunk takes its target in; only r0 is out.
    public const uint CallReadGpr = 0xFFFFFFFEu;
    public const uint CallReadFpr = 0xFFFFC1FEu;     // f1-f8, f14-f31

    // What it can leave changed: the volatile registers, results included.
    public const uint CallWriteGpr = 0x00001FF9u;    // r0, r3-r12
    public const uint CallWriteFpr = 0x00003FFFu;    // f0-f13
    public const byte CallWriteCr = 0xE3;            // cr0, cr1, cr5-cr7

    public static readonly bool Enabled =
        Environment.GetEnvironmentVariable("MKW_TRUST_EABI") != "0";

    public static readonly bool PreserveNonvolatile =
        Enabled && Environment.GetEnvironmentVariable("MKW_EABI_PRESERVE") != "0";

    public static readonly bool TrustFences =
        Enabled && Environment.GetEnvironmentVariable("MKW_EABI_FENCES") != "0";

    /// <summary>
    /// A narrowed body publishes CR fields 2-4 and no XER at its return, so flags it leaves in the others
    /// are dead there (<c>MKW_EABI_RETURN_FLAGS=0</c> keeps them all live).
    /// </summary>
    public static readonly bool NarrowsReturnFlags =
        Enabled && Environment.GetEnvironmentVariable("MKW_EABI_RETURN_FLAGS") != "0";

    public const uint LocalGpr = 0xFFFFC000u;  // r14-r31
    public const uint LocalFpr = 0xFFFFC000u;  // f14-f31

    public static readonly bool LocalizeNonvolatile =
        PreserveNonvolatile && TrustFences && Environment.GetEnvironmentVariable("MKW_EABI_LOCAL_NV") == "1";

    /// <summary>Whether the body at <paramref name="address"/> keeps r14-r31/f14-f31 to itself.</summary>
    public static bool Localizes(uint address, GuestAbiContract contract) =>
        LocalizeNonvolatile && Applies(address) && !contract.HasFullSynchronizationFence &&
        contract.NonvolatileGprInputMask == 0 && contract.NonvolatileFprInputMask == 0;

    private static readonly (uint Start, uint End)[] ExemptRanges = ParseExempt(
        Environment.GetEnvironmentVariable("MKW_EABI_EXEMPT") ?? "801A1C1C-801A2400");

    private static (uint, uint)[] ParseExempt(string spec)
    {
        var ranges = new List<(uint, uint)>();
        foreach (var part in spec.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var bounds = part.Split('-');
            if (bounds.Length == 2 &&
                uint.TryParse(bounds[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var start) &&
                uint.TryParse(bounds[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var end))
            {
                ranges.Add((start, end));
            }
        }
        return ranges.ToArray();
    }

    public static bool Applies(uint address)
    {
        if (!Enabled)
            return false;
        foreach (var (start, end) in ExemptRanges)
            if (address >= start && address < end)
                return false;
        return true;
    }

    /// <summary>
    /// Whether a call to <paramref name="target"/> may be treated as an EABI boundary even though the
    /// callee's contract demands the complete context.
    /// </summary>
    public static bool TrustsFencedCall(uint target) => TrustFences && Applies(target);

    public static GuestAbiContract Apply(uint address, GuestAbiContract contract)
    {
        if (!Applies(address) || contract.HasFullSynchronizationFence)
            return contract;
        var gpr = VisibleGpr;
        var fpr = VisibleFpr;
        var cr = VisibleCr;
        if (PreserveNonvolatile)
        {
            // A nonvolatile written without being read first was never saved, and one whose
            // incoming value is really used is not the caller's to keep: both stay visible.
            // CR stays as it was: the return flush keys off the CR write mask, and a body that
            // touches cr2-cr4 at all is rare enough not to matter.
            gpr = ReturnGpr | (NonvolatileGpr &
                               ~(contract.GprReadBeforeWriteMask & ~contract.NonvolatileGprInputMask));
            fpr = ReturnFpr | (NonvolatileFpr &
                               ~(contract.FprReadBeforeWriteMask & ~contract.NonvolatileFprInputMask));
        }
        var gprReads = contract.GprReadBeforeWriteMask;
        var fprReads = contract.FprReadBeforeWriteMask;
        if (Localizes(address, contract))
        {
            gpr &= ~LocalGpr;
            fpr &= ~LocalFpr;
            gprReads &= ~LocalGpr;
            fprReads &= ~LocalFpr;
        }
        return contract with
        {
            GprReadBeforeWriteMask = gprReads,
            FprReadBeforeWriteMask = fprReads,
            GprPossibleWriteMask = contract.GprPossibleWriteMask & gpr,
            FprPossibleWriteMask = contract.FprPossibleWriteMask & fpr,
            CrPossibleWriteMask = (byte)(contract.CrPossibleWriteMask & cr),
            MayWriteXer = false,
            MayWriteCtr = false,
            GprDefiniteWriteMask = contract.GprDefiniteWriteMask & gpr,
            FprDefiniteWriteMask = contract.FprDefiniteWriteMask & fpr,
            CrDefiniteWriteMask = (byte)(contract.CrDefiniteWriteMask & cr),
            DefinitelyWritesXer = false,
            DefinitelyWritesCtr = false
        };
    }
}
