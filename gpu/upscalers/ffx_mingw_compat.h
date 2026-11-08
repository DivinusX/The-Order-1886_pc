/* mingw-w64 compatibility for the AMD FFX 1.1.4 host and Vulkan backend.
 *
 * The FidelityFX SDK sources rely on transitive includes and MSVC spellings that
 * not every mingw-w64 C++ standard library exposes:
 *   - _countof: an MSVC <stdlib.h> macro; mingw-w64 deliberately omits it.
 *   - swprintf_s: declared by UCRT's <corecrt_wstdio.h>, which the SDK translation
 *     units reach only sometimes; including <wchar.h> first makes it unconditional.
 *   - std::lock_guard / placement operator new / log2: the SDK uses them without
 *     including <mutex>, <new> or <cmath>; libstdc++ pulls them in transitively,
 *     libc++ does not.
 *
 * This header is force-included on the FFX targets only (never on the Order
 * sources), so upstream code compiles unmodified on GCC, clang+libstdc++ and
 * clang+libc++. Every inclusion is idempotent; on toolchains where the SDK
 * already compiled, this header changes nothing observable.
 */
#ifndef ORDER1886_FFX_MINGW_COMPAT_H
#define ORDER1886_FFX_MINGW_COMPAT_H

#if defined(__MINGW32__) && !defined(RC_INVOKED) && !defined(__windres__)
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <math.h>
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof(array)[0])
#endif
#ifdef __cplusplus
#include <new>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <cmath>
#endif
#endif

#endif /* ORDER1886_FFX_MINGW_COMPAT_H */
