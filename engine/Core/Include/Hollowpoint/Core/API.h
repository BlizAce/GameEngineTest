#pragma once

// ---------------------------------------------------------------------------
// Cross-platform symbol decoration for the engine's modules.
//
// Hollowpoint is built as static libraries (the default), in which case HP_STATIC_ENGINE is
// defined and every HP_<MODULE>_API macro expands to nothing. Building shared modules is also
// supported: each module target defines HP_<MODULE>_EXPORTS while compiling itself, consumers
// leave it undefined.
// ---------------------------------------------------------------------------

#if defined(_WIN32) || defined(__CYGWIN__)
    #define HP_DLL_EXPORT __declspec(dllexport)
    #define HP_DLL_IMPORT __declspec(dllimport)
#else
    #define HP_DLL_EXPORT __attribute__((visibility("default")))
    #define HP_DLL_IMPORT __attribute__((visibility("default")))
#endif

#if defined(HP_STATIC_ENGINE)
    #define HP_CORE_API
    #define HP_MATH_API
    #define HP_PLATFORM_API
    #define HP_ECS_API
    #define HP_REFLECTION_API
    #define HP_SERIALIZATION_API
    #define HP_ASSET_API
    #define HP_GRAPHICS_API
    #define HP_RENDERER_API
    #define HP_GAME_API
    #define HP_PHYSICS_API
    #define HP_AUDIO_API
    #define HP_SCRIPTING_API
#else
    #ifdef HP_CORE_EXPORTS
        #define HP_CORE_API HP_DLL_EXPORT
    #else
        #define HP_CORE_API HP_DLL_IMPORT
    #endif

    #ifdef HP_MATH_EXPORTS
        #define HP_MATH_API HP_DLL_EXPORT
    #else
        #define HP_MATH_API HP_DLL_IMPORT
    #endif

    #ifdef HP_PLATFORM_EXPORTS
        #define HP_PLATFORM_API HP_DLL_EXPORT
    #else
        #define HP_PLATFORM_API HP_DLL_IMPORT
    #endif

    #ifdef HP_ECS_EXPORTS
        #define HP_ECS_API HP_DLL_EXPORT
    #else
        #define HP_ECS_API HP_DLL_IMPORT
    #endif

    #ifdef HP_REFLECTION_EXPORTS
        #define HP_REFLECTION_API HP_DLL_EXPORT
    #else
        #define HP_REFLECTION_API HP_DLL_IMPORT
    #endif

    #ifdef HP_SERIALIZATION_EXPORTS
        #define HP_SERIALIZATION_API HP_DLL_EXPORT
    #else
        #define HP_SERIALIZATION_API HP_DLL_IMPORT
    #endif

    #ifdef HP_ASSET_EXPORTS
        #define HP_ASSET_API HP_DLL_EXPORT
    #else
        #define HP_ASSET_API HP_DLL_IMPORT
    #endif

    #ifdef HP_GRAPHICS_EXPORTS
        #define HP_GRAPHICS_API HP_DLL_EXPORT
    #else
        #define HP_GRAPHICS_API HP_DLL_IMPORT
    #endif

    #ifdef HP_RENDERER_EXPORTS
        #define HP_RENDERER_API HP_DLL_EXPORT
    #else
        #define HP_RENDERER_API HP_DLL_IMPORT
    #endif

    #ifdef HP_GAME_EXPORTS
        #define HP_GAME_API HP_DLL_EXPORT
    #else
        #define HP_GAME_API HP_DLL_IMPORT
    #endif

    #ifdef HP_PHYSICS_EXPORTS
        #define HP_PHYSICS_API HP_DLL_EXPORT
    #else
        #define HP_PHYSICS_API HP_DLL_IMPORT
    #endif

    #ifdef HP_AUDIO_EXPORTS
        #define HP_AUDIO_API HP_DLL_EXPORT
    #else
        #define HP_AUDIO_API HP_DLL_IMPORT
    #endif

    #ifdef HP_SCRIPTING_EXPORTS
        #define HP_SCRIPTING_API HP_DLL_EXPORT
    #else
        #define HP_SCRIPTING_API HP_DLL_IMPORT
    #endif
#endif

// ---------------------------------------------------------------------------
// Compiler helpers
// ---------------------------------------------------------------------------

#define HP_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#if defined(__GNUC__) || defined(__clang__)
    #define HP_FORCEINLINE inline __attribute__((always_inline))
    #define HP_NOINLINE __attribute__((noinline))
    #define HP_LIKELY(expr) __builtin_expect(!!(expr), 1)
    #define HP_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
    #define HP_BREAKPOINT() __builtin_trap()
#elif defined(_MSC_VER)
    #define HP_FORCEINLINE __forceinline
    #define HP_NOINLINE __declspec(noinline)
    #define HP_LIKELY(expr) (expr)
    #define HP_UNLIKELY(expr) (expr)
    #define HP_BREAKPOINT() __debugbreak()
#else
    #define HP_FORCEINLINE inline
    #define HP_NOINLINE
    #define HP_LIKELY(expr) (expr)
    #define HP_UNLIKELY(expr) (expr)
    #define HP_BREAKPOINT() abort()
#endif

// Marks a value as intentionally unused; keeps -Werror=/W4 clean without (void) casts at call sites.
//
// MUST NOT wrap a side-effecting expression. It expands through sizeof, and sizeof does not evaluate its
// operand — `HP_UNUSED(DoWork())` discards the call entirely and compiles silently. Use it only on names that
// are bound but unread (parameters in disabled code paths, structured-binding placeholders). For a call whose
// result is deliberately dropped, bind it to a named variable and static_cast<void> it, which does evaluate.
#define HP_UNUSED(x) ((void)(sizeof((x), 0)))
