using System.Collections.Generic;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private static bool TryEmitInlineGuestThunk(
        uint address,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        bool frameBaseIsStack)
    {
        if (!TryGetInlineGuestThunkSpec(address, out var spec))
        {
            return false;
        }

        var frameBase = RegisterToReadExpression("r11", types);
        var isFloat = spec.Kind is InlineGuestThunkKind.SaveFpr or InlineGuestThunkKind.RestFpr;
        var isStore = spec.Kind is InlineGuestThunkKind.SaveGpr or InlineGuestThunkKind.SaveFpr;
        EmitInlineGuestThunkRange(spec, frameBase, sb, pad, types, frameBaseIsStack, isFloat, isStore);
        return true;
    }

    /// <summary>
    /// A call to a CodeWarrior runtime helper the body runs inline (<see cref="GuestRuntimeIntrinsics"/>):
    /// <c>__cvt_fp2unsigned</c> becomes <c>PPC_CvtFp2UnsignedInline</c> on f1 into r3. Everything else it
    /// touches -- f0, f2-f4, r4, cr0/cr6/cr7 and a frame below r1 -- is volatile under the EABI, so no
    /// caller reads it, and the call's flush and reload around a body this small cost more than its work.
    /// </summary>
    private static bool TryEmitGuestRuntimeIntrinsic(
        uint address,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        Dictionary<string, bool> localPaired)
    {
        if (!IsGuestRuntimeIntrinsic(address))
        {
            return false;
        }

        var value = ToScalarFloatExpression(IrValue.Register("f1"), types, localPaired);
        sb.AppendLine($"{pad}{RegisterToWriteExpression("r3", types)} = PPC_CvtFp2UnsignedInline({value});");
        return true;
    }

    /// <summary>
    /// Whether a call to <paramref name="address"/> runs inline (<see cref="TryEmitGuestRuntimeIntrinsic"/>):
    /// then it flushes and reloads nothing, and every pass that reasons about what a call leaves in the
    /// context has to see it as the plain write of r3 it becomes.
    /// </summary>
    private static bool IsGuestRuntimeIntrinsic(uint address) =>
        GuestRuntimeIntrinsics.Current.CvtFp2Unsigned == address && GuestEabiVisibility.Applies(address);

    private static void EmitInlineGuestThunkRange(
        InlineGuestThunkSpec spec,
        string frameBase,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        bool frameBaseIsStack,
        bool isFloat,
        bool isStore)
    {
        var bytesPerRegister = isFloat ? 8 : 4;
        var helperSize = bytesPerRegister;

        if (frameBaseIsStack)
        {
            var firstOffset = (spec.StartRegister - 32) * bytesPerRegister;
            var length = (32 - spec.StartRegister) * bytesPerRegister;
            sb.AppendLine($"{pad}{{");
            var needsRead = isStore ? "false" : "true";
            var needsWrite = isStore ? "true" : "false";
            sb.AppendLine($"{pad}    uint8_t* const guest_thunk_stack = MemoryInline::ResolveRangeHost(({frameBase} + {firstOffset}), 0, {length}u, {needsRead}, {needsWrite});");
            for (var reg = spec.StartRegister; reg <= 31; reg++)
            {
                var offset = (reg - 32) * bytesPerRegister;
                var rangeOffset = offset - firstOffset;
                var registerName = isFloat ? FprRegisterName(reg) : GprRegisterName(reg);
                var value = isStore
                    ? RegisterToReadExpression(registerName, types)
                    : RegisterToWriteExpression(registerName, types);
                if (isStore)
                {
                    var storedValue = isFloat ? value : $"static_cast<uint32_t>({value})";
                    var helper = isFloat ? "WriteResolvedFloat64" : "WriteResolved32";
                    sb.AppendLine($"{pad}    MemoryInline::{helper}(guest_thunk_stack, {rangeOffset}u, ({frameBase} + {offset}), {storedValue});");
                }
                else
                {
                    var helper = isFloat ? "ReadResolvedFloat64" : "ReadResolved32";
                    sb.AppendLine($"{pad}    {value} = MemoryInline::{helper}(guest_thunk_stack, {rangeOffset}u, ({frameBase} + {offset}));");
                }
            }
            sb.AppendLine($"{pad}}}");
            return;
        }

        for (var reg = spec.StartRegister; reg <= 31; reg++)
        {
            var offset = (reg - 32) * bytesPerRegister;
            var registerName = isFloat ? FprRegisterName(reg) : GprRegisterName(reg);
            var value = isStore
                ? RegisterToReadExpression(registerName, types)
                : RegisterToWriteExpression(registerName, types);
            if (isStore)
            {
                var storedValue = isFloat ? value : $"static_cast<uint32_t>({value})";
                sb.AppendLine($"{pad}{GuestStoreHelper(helperSize, isFloat)}({FlatAddressArguments(frameBase, offset)}, {storedValue});");
            }
            else
            {
                sb.AppendLine($"{pad}{value} = {GuestLoadHelper(helperSize, isFloat)}({FlatAddressArguments(frameBase, offset)});");
            }
        }
    }

    private static bool HasLocallyProvenStackFrameBase(
        IReadOnlyList<Translator.Core.Ir.IrInstruction> instructions,
        int callIndex,
        StackAddressFacts stackFacts)
    {
        // EABI save/restore thunks implicitly consume r11, so it is not present
        // in the ordinary argument list. Prove the thunk's frame base from the
        // nearest local SSA definition. Standard compiler prologues/epilogues
        // establish r11 immediately before the thunk; anything less direct
        // deliberately falls back to generic guest memory.
        for (var index = callIndex - 1; index >= 0; --index)
        {
            var destination = instructions[index] switch
            {
                Translator.Core.Ir.IrAssign assign => assign.Destination,
                Translator.Core.Ir.IrBinary binary => binary.Destination,
                Translator.Core.Ir.IrLoad load => load.Destination,
                Translator.Core.Ir.IrCall call => call.Destination,
                Translator.Core.Ir.IrIndirectCall call => call.Destination,
                Translator.Core.Ir.IrPhi phi => phi.Destination,
                _ => null
            };
            if (destination is null ||
                !GetRegisterBaseName(destination).Equals("r11", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            return stackFacts.ContainsTemporary(destination);
        }

        return false;
    }
}
