#pragma once

#include <string_view>

#include "Hollowpoint/Core/API.h"

namespace Hollowpoint::Reflection
{

// Interns a name for the process lifetime and returns a pointer that stays valid forever.
//
// Descriptors store plain `const char*` (they are built once at startup, read constantly, and must not
// carry a std::string each), so the storage has to be stable across every later insertion. A node-based
// container provides that; a std::vector would dangle on its first reallocation, which is exactly the bug
// this helper exists to prevent.
[[nodiscard]] HP_REFLECTION_API const char* InternName(std::string_view name);

}  // namespace Hollowpoint::Reflection
