#pragma once

// ---------------------------------------------------------------------------
// Include glslang's headers from C++ through this file, never directly.
//
// glslang predates our warning policy and its own headers do not satisfy -Wshadow (its Includer::IncludeResult
// constructor parameters shadow its members) or -Wold-style-cast. Including them raw therefore breaks the build
// with an error pointing at third-party code — confusing, since nothing in the engine is wrong. The suppression is
// scoped to these includes so engine code below them remains fully checked.
//
// Centralised in one header (rather than pragmas at each use site) so that adding a new translation unit that
// compiles shaders cannot reintroduce the problem: include this and it works.
//
// The glslang:: namespace is NOT imported here. Its unqualified enum names (EShSourceGlsl, EShLangVertex, ...) are
// generic enough to collide with engine identifiers, so callers qualify them explicitly.
// ---------------------------------------------------------------------------

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wshadow"
    #pragma clang diagnostic ignored "-Wold-style-cast"
    #pragma clang diagnostic ignored "-Wreserved-identifier"
    #pragma clang diagnostic ignored "-Wpedantic"
    #pragma clang diagnostic ignored "-Wextra-semi"
    #pragma clang diagnostic ignored "-Wcast-qual"
    #pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
    #pragma clang diagnostic ignored "-Wmissing-noreturn"
    #pragma clang diagnostic ignored "-Wcomma"
#elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wshadow"
    #pragma GCC diagnostic ignored "-Wold-style-cast"
    #pragma GCC diagnostic ignored "-Wpedantic"
    #pragma GCC diagnostic ignored "-Wextra-semi"
    #pragma GCC diagnostic ignored "-Wcast-qual"
    #pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
    #pragma GCC diagnostic ignored "-Wmissing-noreturn"
#endif

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#if defined(__clang__) || defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
