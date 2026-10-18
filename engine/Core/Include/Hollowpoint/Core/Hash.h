#pragma once

#include <bit>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Hashing.
//
// The engine needs hashes that are (1) deterministic across processes and platforms — asset keys,
// component type ids and script binding names are stored in saved scenes — and (2) cheap. FNV-1a 64
// satisfies both and is trivially auditable, which matters more here than raw speed: we hash strings
// at load time and type names at registration time, not per frame.
//
// IMPORTANT: never change these constants or the mixing below without bumping the serialized format
// version, otherwise existing scenes silently lose their references.
// ---------------------------------------------------------------------------

inline constexpr UInt64 FnvOffsetBasis = 14695981039346656037ull;
inline constexpr UInt64 FnvPrime = 1099511628211ull;

// Hashes a byte buffer.
[[nodiscard]] HP_CORE_API HashValue HashBytes(const void* data, std::size_t size);

// Hashes a string view (bytes are taken as-is; no case folding — use HashStringCaseInsensitive).
[[nodiscard]] HP_CORE_API HashValue HashString(std::string_view text);

// Case-insensitive ASCII hash of a string view. Used for user-facing identifiers where "Player" and
// "player" must resolve to the same thing (script globals, console commands).
[[nodiscard]] HP_CORE_API HashValue HashStringCaseInsensitive(std::string_view text);

// Combines two hashes into one, for composite keys (type + field name, path + subobject index...).
[[nodiscard]] HP_FORCEINLINE constexpr HashValue CombineHash(HashValue seed, HashValue value) noexcept
{
    // SplitMix64 finalizer — good avalanche, no branches.
    UInt64 mixed = seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    mixed ^= mixed >> 30;
    mixed *= 0xbf58476d1ce4e5b9ull;
    mixed ^= mixed >> 27;
    mixed *= 0x94d049bb133111ebull;
    mixed ^= mixed >> 31;
    return mixed;
}

namespace Detail
{
// Maps a hashable type to the unsigned-friendly integer type used for its canonical byte form.
template <typename T>
struct HashIntegralType
{
    using Type = T;
};

template <typename T>
    requires std::is_enum_v<T>
struct HashIntegralType<T>
{
    using Type = std::underlying_type_t<T>;
};
}  // namespace Detail

// Hash of an integer, floating point or enum value. The value is converted to an unsigned integer
// and then emitted as little-endian bytes, so the hash is identical on big-endian hosts — hashes end
// up inside saved scenes and must not depend on the machine that wrote them.
template <typename T>
[[nodiscard]] HP_FORCEINLINE HashValue HashValueOf(T value) noexcept
{
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "HashValueOf takes arithmetic or enum types");

    // Enumerations are hashed through their underlying integer type (std::make_unsigned does not
    // accept scoped enums). Detail::HashIntegralType keeps the trait valid for non-enum types too.
    using IntegralType = typename Detail::HashIntegralType<T>::Type;

    UInt64 raw = 0;
    if constexpr (std::is_floating_point_v<IntegralType>)
    {
        // Canonicalize -0.0 so it hashes like +0.0 (they compare equal and therefore must hash equal).
        const IntegralType normalized = (value == IntegralType{0}) ? IntegralType{0} : value;
        if constexpr (sizeof(IntegralType) == 4)
        {
            raw = std::bit_cast<UInt32>(normalized);
        }
        else if constexpr (sizeof(IntegralType) == 8)
        {
            raw = std::bit_cast<UInt64>(normalized);
        }
        else
        {
            static_assert(sizeof(IntegralType) == 4 || sizeof(IntegralType) == 8,
                          "HashValueOf supports 32- and 64-bit floating point types only");
        }
    }
    else if constexpr (std::is_signed_v<IntegralType>)
    {
        raw = static_cast<UInt64>(static_cast<std::make_unsigned_t<IntegralType>>(value));
    }
    else
    {
        raw = static_cast<UInt64>(value);
    }

    UInt8 bytes[sizeof(T)];
    for (std::size_t index = 0; index < sizeof(T); ++index)
    {
        bytes[index] = static_cast<UInt8>((raw >> (8 * index)) & 0xffull);
    }
    return HashBytes(bytes, sizeof(T));
}

// ---------------------------------------------------------------------------
// Hash functors for use with the engine's unordered containers.
// ---------------------------------------------------------------------------

struct Hash
{
    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept
    {
        return static_cast<std::size_t>(HashString(text));
    }
};

template <typename T>
struct ValueHash
{
    [[nodiscard]] std::size_t operator()(T value) const noexcept
    {
        return static_cast<std::size_t>(HashValueOf(value));
    }
};

}  // namespace Hollowpoint::Core
