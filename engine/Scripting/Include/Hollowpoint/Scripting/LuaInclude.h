#pragma once

// ---------------------------------------------------------------------------
// Include Lua's C headers from C++ through this file, not directly.
//
// Two problems are solved here, and both bite only depending on how a target happens to be configured, which is why
// they belong in one shared header rather than at each call site:
//
//   * **Warning policy.** Lua's headers are C89-flavoured and contain constructs our engine policy rejects —
//     old-style casts (inside `luaL_Buffer`, via the `LUAL_BUFFERSIZE` macro), and declarations that read as
//     unused-parameter or documentation-shaped to clang. Including them directly makes a clang build with
//     -Werror fail on code we do not own, while a GCC build of the same file passes — so the breakage appears only in
//     the sanitiser preset, far from the commit that caused it. sol2 gets away with it because it is included as a
//     SYSTEM header; our own `extern "C" { #include ... }` blocks are not.
//
//   * **Linkage.** lua.h declares its API `extern "C"`-safe but relies on being included outside C++ name mangling in
//     some configurations. Wrapping guarantees it once instead of trusting each includer to remember.
//
// The suppression is scoped as tightly as the preprocessor allows — around the includes only, so our own code after
// this point is still fully checked.
// ---------------------------------------------------------------------------

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wold-style-cast"
    #pragma clang diagnostic ignored "-Wreserved-identifier"
    #pragma clang diagnostic ignored "-Wreserved-id-macro"
    #pragma clang diagnostic ignored "-Wdocumentation"
#elif defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wold-style-cast"
    #pragma GCC diagnostic ignored "-Wpedantic"
#endif

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#if defined(__clang__) || defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
