#pragma once
// Force-included into every runtime translation unit on the PS3.
//
// The PS3 toolchain's libstdc++ (GCC 7, newlib) is configured without C99
// stdio/math in namespace std and without gthreads. The runtime is written
// against a full C++17 library; this fills the gaps it actually uses. The
// thread primitives are in the compat <mutex>, <thread> and
// <condition_variable> headers next to this one.

#ifdef __cplusplus
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <deque>
#include <list>

namespace std {
using ::_Exit;
using ::snprintf;
using ::vsnprintf;
using ::trunc;
using ::truncf;
using ::round;
using ::roundf;
using ::lround;
using ::llround;
using ::fma;
using ::fmaf;
using ::copysign;
using ::copysignf;
using ::nearbyint;
using ::exp2;
using ::log2;
using ::cbrt;
using ::hypot;
using ::strtof;
using ::strtold;
using ::strtoll;
using ::strtoull;

inline string mkw_to_string_fmt(const char* fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    const int n = ::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return string(buf, n < 0 ? 0 : (n < static_cast<int>(sizeof buf) ? n : static_cast<int>(sizeof buf) - 1));
}
inline string to_string(int v) { return mkw_to_string_fmt("%d", v); }
inline string to_string(long v) { return mkw_to_string_fmt("%ld", v); }
inline string to_string(long long v) { return mkw_to_string_fmt("%lld", v); }
inline string to_string(unsigned v) { return mkw_to_string_fmt("%u", v); }
inline string to_string(unsigned long v) { return mkw_to_string_fmt("%lu", v); }
inline string to_string(unsigned long long v) { return mkw_to_string_fmt("%llu", v); }
inline string to_string(float v) { return mkw_to_string_fmt("%f", static_cast<double>(v)); }
inline string to_string(double v) { return mkw_to_string_fmt("%f", v); }

inline int stoi(const string& s, size_t* idx = nullptr, int base = 10)
{
    char* end = nullptr;
    const long v = ::strtol(s.c_str(), &end, base);
    if (idx) *idx = static_cast<size_t>(end - s.c_str());
    return static_cast<int>(v);
}
inline unsigned long stoul(const string& s, size_t* idx = nullptr, int base = 10)
{
    char* end = nullptr;
    const unsigned long v = ::strtoul(s.c_str(), &end, base);
    if (idx) *idx = static_cast<size_t>(end - s.c_str());
    return v;
}
inline unsigned long long stoull(const string& s, size_t* idx = nullptr, int base = 10)
{
    char* end = nullptr;
    const unsigned long long v = ::strtoull(s.c_str(), &end, base);
    if (idx) *idx = static_cast<size_t>(end - s.c_str());
    return v;
}
inline double stod(const string& s, size_t* idx = nullptr)
{
    char* end = nullptr;
    const double v = ::strtod(s.c_str(), &end);
    if (idx) *idx = static_cast<size_t>(end - s.c_str());
    return v;
}
inline float stof(const string& s, size_t* idx = nullptr)
{
    return static_cast<float>(stod(s, idx));
}
// C++20 std::erase_if for the containers the runtime uses it on.
template <class K, class V, class C, class A, class Pred>
typename map<K, V, C, A>::size_type erase_if(map<K, V, C, A>& c, Pred pred)
{
    auto old = c.size();
    for (auto it = c.begin(); it != c.end();)
        it = pred(*it) ? c.erase(it) : std::next(it);
    return old - c.size();
}
template <class K, class V, class H, class E, class A, class Pred>
typename unordered_map<K, V, H, E, A>::size_type erase_if(unordered_map<K, V, H, E, A>& c, Pred pred)
{
    auto old = c.size();
    for (auto it = c.begin(); it != c.end();)
        it = pred(*it) ? c.erase(it) : std::next(it);
    return old - c.size();
}
template <class K, class C, class A, class Pred>
typename set<K, C, A>::size_type erase_if(set<K, C, A>& c, Pred pred)
{
    auto old = c.size();
    for (auto it = c.begin(); it != c.end();)
        it = pred(*it) ? c.erase(it) : std::next(it);
    return old - c.size();
}
template <class T, class A, class Pred>
typename vector<T, A>::size_type erase_if(vector<T, A>& c, Pred pred)
{
    auto it = std::remove_if(c.begin(), c.end(), pred);
    auto n = static_cast<typename vector<T, A>::size_type>(c.end() - it);
    c.erase(it, c.end());
    return n;
}
template <class T, class A, class Pred>
typename deque<T, A>::size_type erase_if(deque<T, A>& c, Pred pred)
{
    auto it = std::remove_if(c.begin(), c.end(), pred);
    auto n = static_cast<typename deque<T, A>::size_type>(c.end() - it);
    c.erase(it, c.end());
    return n;
}
} // namespace std
#endif
