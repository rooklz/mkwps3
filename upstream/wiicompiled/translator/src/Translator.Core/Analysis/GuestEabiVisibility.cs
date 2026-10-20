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
/// </summary>
public static class GuestEabiVisibility
{
    public const uint VisibleGpr = 0xFFFFE01Eu;   // r1-r4, r13-r31
    public const uint VisibleFpr = 0xFFFFC002u;   // f1, f14-f31
    public const byte VisibleCr = 0x1C;           // cr2-cr4

    public static readonly bool Enabled =
        Environment.GetEnvironmentVariable("MKW_TRUST_EABI") != "0";

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

    public static GuestAbiContract Apply(uint address, GuestAbiContract contract)
    {
        if (!Applies(address) || contract.HasFullSynchronizationFence)
            return contract;
        return contract with
        {
            GprPossibleWriteMask = contract.GprPossibleWriteMask & VisibleGpr,
            FprPossibleWriteMask = contract.FprPossibleWriteMask & VisibleFpr,
            CrPossibleWriteMask = (byte)(contract.CrPossibleWriteMask & VisibleCr),
            MayWriteXer = false,
            MayWriteCtr = false,
            GprDefiniteWriteMask = contract.GprDefiniteWriteMask & VisibleGpr,
            FprDefiniteWriteMask = contract.FprDefiniteWriteMask & VisibleFpr,
            CrDefiniteWriteMask = (byte)(contract.CrDefiniteWriteMask & VisibleCr),
            DefinitelyWritesXer = false,
            DefinitelyWritesCtr = false
        };
    }
}
