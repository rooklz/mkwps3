// MSL runtime helpers the compiler calls from ordinary code, done natively.
// They are tiny, pure and called per sample or per element, so the call into
// translated code costs more than their work; native_cpu_calls.inc binds them
// as leaf natives (a direct host call, no register guard).

#include <cstdint>

// __cvt_fp2unsigned (0x80021478): double -> uint32, as the original computes
// it: negative -> 0, 2^32 or more (or NaN) -> 0xFFFFFFFF, otherwise truncated,
// going through fctiwz in two halves above 2^31.
extern "C" uint32_t MSL__cvt_fp2unsigned_80021478(double value)
{
    if (value < 0.0)
        return 0u;
    if (!(value < 4294967296.0))
        return 0xFFFFFFFFu;
    if (value < 2147483648.0)
        return static_cast<uint32_t>(static_cast<int32_t>(value));
    return static_cast<uint32_t>(static_cast<int32_t>(value - 2147483648.0)) + 0x80000000u;
}
