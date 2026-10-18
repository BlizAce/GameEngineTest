#pragma once

#include <exception>
#include <format>
#include <string>
#include <string_view>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Formatting
//
// The engine formats every user-visible string through std::format. Format.h centralises that so
// we keep exactly one formatting implementation, and it adds two things on top of the standard:
//
//   * HP_STR / Format() never throw: a format error yields a visible "<<format error>>" string
//     instead of unwinding through engine code (log lines must not crash the game).
//   * Hash helpers used by asset paths, scripting bindings and the reflection registry.
// ---------------------------------------------------------------------------

// Formats a message. Equivalent to std::format, with two differences:
//   * The format string is checked at compile time (std::format_string), so a mismatched placeholder
//     is a build error rather than a runtime surprise.
//   * A formatter that throws (a custom operator<< for a user type, say) degrades to a visible
//     marker instead of unwinding through engine code — log lines must never crash the game.
template <typename... Args>
[[nodiscard]] inline std::string Format(std::format_string<Args...> format, Args&&... args)
{
    try
    {
        return std::format(format, std::forward<Args>(args)...);
    }
    catch (const std::exception&)
    {
        return "<<format error>>";
    }
}

// Convenience wrapper so call sites can write HP_FORMAT("{}", value) in macro arguments where a
// comma would otherwise split the macro parameter. (Macros: use HP_FORMAT rather than Format().)
#define HP_FORMAT(...) ::Hollowpoint::Core::Format(__VA_ARGS__)

}  // namespace Hollowpoint::Core
