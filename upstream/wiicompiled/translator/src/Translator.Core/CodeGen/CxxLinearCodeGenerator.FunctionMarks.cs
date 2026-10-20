using System;
using System.Globalization;
using System.Text;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Guest address of the body being emitted, for <c>MKW_FN_MARK</c>: a sampler on another host
    /// thread reads the mark to attribute time to the guest function actually executing. Emitted on
    /// entry and after every call returns, so time spent after a callee comes back is the caller's.
    /// </summary>
    [ThreadStatic]
    private static uint? _markAddress;

    private static uint? MarkAddressOf(IrFunction func)
    {
        foreach (var candidate in new[] { func.EntryLabel, func.Name })
        {
            if (candidate is { Length: >= 8 } &&
                uint.TryParse(candidate.AsSpan(candidate.Length - 8), NumberStyles.HexNumber,
                    CultureInfo.InvariantCulture, out var address) &&
                address >= 0x80000000u)
            {
                return address;
            }
        }
        return null;
    }

    private static void AppendFunctionMark(StringBuilder sb, string pad)
    {
        if (_markAddress is { } address)
        {
            sb.AppendLine($"{pad}MKW_FN_MARK(0x{address:X8}u);");
        }
    }

    private static void AppendFunctionEntryMark(StringBuilder sb, string pad)
    {
        if (_markAddress is { } address)
        {
            sb.AppendLine($"{pad}MKW_FN_ENTER(0x{address:X8}u);");
        }
    }

    /// <summary>
    /// Public contracts with each state-free target's call inputs and outputs folded in, for the
    /// register cache of a state-free clone (see the call site).
    /// </summary>
    private static IReadOnlyDictionary<uint, GuestAbiContract>? WithStateFreeCallInputs(
        IReadOnlyDictionary<uint, GuestAbiContract>? publicContracts,
        IReadOnlyDictionary<uint, GuestAbiContract>? stateFreeContracts)
    {
        if (publicContracts is null || stateFreeContracts is null || stateFreeContracts.Count == 0)
            return publicContracts;
        var merged = new Dictionary<uint, GuestAbiContract>(publicContracts);
        foreach (var (address, stateFree) in stateFreeContracts)
        {
            if (!merged.TryGetValue(address, out var contract) || contract.HasFullSynchronizationFence)
                continue;
            merged[address] = contract with
            {
                GprReadBeforeWriteMask = contract.GprReadBeforeWriteMask | stateFree.GprReadBeforeWriteMask,
                GprPossibleWriteMask = contract.GprPossibleWriteMask | stateFree.GprPossibleWriteMask,
                FprReadBeforeWriteMask = contract.FprReadBeforeWriteMask | stateFree.FprReadBeforeWriteMask,
                FprPossibleWriteMask = contract.FprPossibleWriteMask | stateFree.FprPossibleWriteMask
            };
        }
        return merged;
    }
}
