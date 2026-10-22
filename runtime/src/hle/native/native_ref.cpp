// The translated originals of the native ports here, under other names: the
// difftest's references, and memcpy's and memset's path for what isn't plain RAM.
// Built from the workspace's translation; nothing of it is in this repository.

#define func_80005F34 mkw_ref_80005F34
#include "functions/func_80005F34.cpp"
#undef func_80005F34

#define func_80005F84 mkw_ref_80005F84
#include "functions/func_80005F84.cpp"
#undef func_80005F84

#define func_80006038 mkw_ref_80006038
#include "functions/func_80006038.cpp"
#undef func_80006038

#if defined(MKW_NATIVE_DIFFTEST)
#define func_80199D64 mkw_ref_80199D64
#include "functions/func_80199D64.cpp"
#undef func_80199D64

#define func_8019A91C mkw_ref_8019A91C
#include "functions/func_8019A91C.cpp"
#undef func_8019A91C
#endif
