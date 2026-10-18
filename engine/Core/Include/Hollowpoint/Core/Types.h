#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Hollowpoint::Core
{

// Fixed-width aliases used throughout the engine. We always use these instead of bare int/long in
// APIs and serialized formats so that a scene file written on Windows loads identically on Linux.
using Int8 = std::int8_t;
using Int16 = std::int16_t;
using Int32 = std::int32_t;
using Int64 = std::int64_t;
using UInt8 = std::uint8_t;
using UInt16 = std::uint16_t;
using UInt32 = std::uint32_t;
using UInt64 = std::uint64_t;

// Sizes, counts and indices.
using Size = std::size_t;
using Index = std::uint32_t;
using HashValue = std::uint64_t;

// A stable "no value" index for Index-typed handles.
inline constexpr Index InvalidIndex = std::numeric_limits<Index>::max();

static_assert(sizeof(void*) >= sizeof(UInt32), "Hollowpoint requires at least a 32-bit platform");

}  // namespace Hollowpoint::Core
