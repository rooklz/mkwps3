using Translator.Core.Loading;

namespace Translator.Core.Analysis;

/// <summary>
/// CodeWarrior runtime helpers a call site runs inline instead of calling, found by their symbol
/// names in the function map like the save/restore thunks (<see cref="GuestSaveRestoreThunks"/>).
/// </summary>
public sealed class GuestRuntimeIntrinsics
{
    public static GuestRuntimeIntrinsics None { get; } = new(null);

    /// <summary>
    /// The helpers in effect for this process. Set once from the loaded project; <see cref="None"/>
    /// until then, so every call stays a call for a project with no function map.
    /// </summary>
    public static GuestRuntimeIntrinsics Current { get; set; } = None;

    private GuestRuntimeIntrinsics(uint? cvtFp2Unsigned)
    {
        CvtFp2Unsigned = cvtFp2Unsigned;
    }

    /// <summary><c>__cvt_fp2unsigned</c>: the double in f1 to an unsigned word in r3.</summary>
    public uint? CvtFp2Unsigned { get; }

    public static GuestRuntimeIntrinsics FromFunctionMap(FunctionMap map) =>
        new(map.TryGetAddress("__cvt_fp2unsigned", out var cvtFp2Unsigned) ? cvtFp2Unsigned : null);
}
