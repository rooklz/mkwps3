#pragma once
// Which guest functions take the memory slow paths, for finding stores and loads the translator
// could have proved fast (the control port's `slow`). Off unless switched on: one flag load on
// paths that are slow anyway.

#include <cstdint>

enum MkwSlowPath : uint32_t {
    kMkwSlowReadMmio,
    kMkwSlowReadRam,
    kMkwSlowWriteFifo,
    kMkwSlowWriteMmio,
    kMkwSlowWriteRam,
    kMkwSlowPsqLoad,
    kMkwSlowPsqStore,
    kMkwSlowResolvedFallback,
};

extern "C" volatile int g_mkwSlowCensus;
extern "C" void mkw_slow_census_note(uint32_t kind);

#define MKW_SLOW_NOTE(kind)                       \
    do {                                          \
        if (g_mkwSlowCensus)                      \
            mkw_slow_census_note(kind);           \
    } while (0)
