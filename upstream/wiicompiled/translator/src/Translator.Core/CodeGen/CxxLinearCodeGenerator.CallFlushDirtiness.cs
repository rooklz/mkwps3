using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// The resident registers that may differ from their <c>CpuContext</c> copy at each linked EABI
    /// call, for the body being emitted (<see cref="PlanCallFlushDirtiness"/>). Null when there is no
    /// plan, and then every call flushes everything its boundary covers, as before.
    /// </summary>
    [ThreadStatic]
    private static Dictionary<IrInstruction, (uint Gpr, uint Fpr)>? _callFlushDirtiness;

    /// <summary>
    /// A resident body flushes, before every call, each register it writes anywhere and the callee's
    /// contract covers -- including ones it hasn't touched since the last flush, whose context copy
    /// is already right, and volatile ones a previous call left dead. A forward pass finds, for each
    /// linked call into an EABI body, the registers that may have changed since the context last held
    /// them: on entry nothing has; a write dirties its register; a linked EABI call cleans what it
    /// flushes and reloads, leaves the other volatile registers dead (nothing may read them before
    /// writing them) and the other nonvolatile ones as they were (the callee restores them). A call
    /// that may go through a state-free entry hands results to locals instead and isn't modelled;
    /// it, and anything else that crosses the context -- an indirect call, a tail call -- leaves
    /// everything dirty. A body the static CFG doesn't fully describe gets no plan.
    /// </summary>
    private static Dictionary<IrInstruction, (uint Gpr, uint Fpr)>? PlanCallFlushDirtiness(
        IrFunction function,
        IrCfg cfg,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets,
        IReadOnlySet<uint> nonReturningCallTargets,
        IReadOnlySet<uint> lrContinuationCallTargets,
        IReadOnlySet<uint> stateFreeTargets)
    {
        if (!GuestEabiVisibility.NarrowsCallFlushes ||
            HasOutOfBandLocalEntry(function, nonReturningCallTargets, lrContinuationCallTargets))
        {
            return null;
        }

        foreach (var block in function.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                if (instruction is IrIndirectJump or IrUndefined)
                {
                    return null;
                }
            }
        }

        const uint allDirty = uint.MaxValue;
        var comparer = StringComparer.OrdinalIgnoreCase;
        var blockIn = new Dictionary<string, (uint Gpr, uint Fpr)>(comparer);
        foreach (var block in function.Blocks)
        {
            var reachable = block.Label.Equals(function.EntryLabel, StringComparison.OrdinalIgnoreCase) ||
                            cfg.Predecessors(block.Label).Count != 0;
            blockIn[block.Label] = reachable ? (0u, 0u) : (allDirty, allDirty);
        }

        var atCall = new Dictionary<IrInstruction, (uint Gpr, uint Fpr)>(ReferenceEqualityComparer.Instance);
        bool changed;
        do
        {
            changed = false;
            foreach (var block in function.Blocks)
            {
                var (gpr, fpr) = blockIn[block.Label];
                for (var index = 0; index < block.Instructions.Count; ++index)
                {
                    var instruction = block.Instructions[index];
                    if (instruction is IrCall call && TryParseAddress(call.Target, out var target))
                    {
                        // A state-free entry takes its inputs by value and hands its results to
                        // locals, never through the context: not modelled here.
                        if (!stateFreeTargets.Contains(target) &&
                            IsNarrowableLinkedCall(block.Instructions, index, target, guestAbiContracts,
                                modOverridableCallTargets))
                        {
                            atCall[instruction] = (gpr, fpr);
                            // What the call publishes or reloads matches the context afterwards (a
                            // nonvolatile comes back restored), and the volatile registers it doesn't
                            // reload are dead.
                            var sync = ResidencyBoundarySync.FromCalleeContract(guestAbiContracts[target]);
                            gpr &= ~(sync.FlushGprMask | sync.ReloadGprMask | GuestEabiVisibility.CallWriteGpr);
                            fpr &= ~(sync.FlushFprMask | sync.ReloadFprMask | GuestEabiVisibility.CallWriteFpr);
                        }
                        else
                        {
                            gpr = allDirty;
                            fpr = allDirty;
                        }
                        continue;
                    }

                    if (instruction is IrIndirectCall)
                    {
                        if (GuestEabiVisibility.TrustFences)
                        {
                            // An EABI call to a target known only at run time
                            // (ResidencyBoundarySync.EabiFencedCall): it publishes everything
                            // that changed, reloads the results, and the rest is dead or restored.
                            atCall[instruction] = (gpr, fpr);
                            gpr = 0;
                            fpr = 0;
                        }
                        else
                        {
                            gpr = allDirty;
                            fpr = allDirty;
                        }
                        continue;
                    }

                    foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                    {
                        var baseName = RegisterNameUtils.HardwareBase(definition);
                        if (baseName.Length is >= 2 and <= 3 &&
                            int.TryParse(baseName.AsSpan(1), out var register) && register is >= 0 and < 32)
                        {
                            if (baseName[0] is 'r' or 'R') gpr |= 1u << register;
                            else if (baseName[0] is 'f' or 'F') fpr |= 1u << register;
                        }
                    }
                }

                foreach (var successor in cfg.Successors(block.Label))
                {
                    if (!blockIn.TryGetValue(successor, out var current))
                    {
                        continue;
                    }

                    var merged = (current.Gpr | gpr, current.Fpr | fpr);
                    if (merged != current)
                    {
                        blockIn[successor] = merged;
                        changed = true;
                    }
                }
            }
        } while (changed);

        return atCall;
    }

    /// <summary>
    /// A call a <c>bl</c> made (it follows its <c>lr</c> load) into a body that keeps the EABI and
    /// whose published contract is the one that runs.
    /// </summary>
    private static bool IsNarrowableLinkedCall(
        IReadOnlyList<IrInstruction> instructions,
        int index,
        uint target,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets) =>
        index > 0 &&
        instructions[index - 1] is IrAssign { Value.Kind: "const" } lrAssign &&
        GetRegisterBaseName(lrAssign.Destination).Equals("lr", StringComparison.OrdinalIgnoreCase) &&
        GuestEabiVisibility.Applies(target) &&
        !modOverridableCallTargets.Contains(target) &&
        guestAbiContracts.TryGetValue(target, out var contract) &&
        !contract.HasFullSynchronizationFence;
}
