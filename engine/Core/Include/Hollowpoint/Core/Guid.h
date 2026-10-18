#pragma once

#include <array>
#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Guid — a 128-bit universally unique identifier (RFC 4122 version 4, random).
//
// Identities are the backbone of a persistent scene format: entities, components and assets keep
// their Guid across renames, moves and re-imports, so references between them survive. That is why
// the engine never uses pointers or array indices as identities in serialized data.
//
// The value is stored as four big-endian-normalized 32-bit words so that hashing and comparison are
// cheap and independent of struct padding.
// ---------------------------------------------------------------------------

class HP_CORE_API Guid
{
public:
    // Creates a nil (all-zero) Guid. The nil Guid is never produced by Generate() and marks
    // "no reference" in serialized data.
    constexpr Guid() noexcept = default;

    // Explicit construction from the four words, most significant first (network byte order).
    constexpr Guid(UInt32 word0, UInt32 word1, UInt32 word2, UInt32 word3) noexcept
        : m_Words{word0, word1, word2, word3}
    {}

    // Generates a new random (v4) Guid. Thread-safe; uses a per-thread CSPRNG-stirred generator.
    [[nodiscard]] static Guid Generate();

    // Parses "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (hyphens optional, hex case insensitive).
    // Returns std::nullopt when the text is not a valid Guid.
    [[nodiscard]] static std::optional<Guid> Parse(std::string_view text);

    // Same as Parse but returns the nil Guid instead of an optional — convenience at call sites
    // where "invalid input" and "empty reference" should be treated identically.
    [[nodiscard]] static Guid ParseOrNil(std::string_view text);

    // Canonical lower-case hyphenated form, e.g. "3f6b2a1c-...".
    [[nodiscard]] std::string ToString() const;

    [[nodiscard]] constexpr bool IsNil() const noexcept
    {
        return m_Words[0] == 0 && m_Words[1] == 0 && m_Words[2] == 0 && m_Words[3] == 0;
    }

    [[nodiscard]] constexpr UInt32 GetWord(std::size_t index) const noexcept
    {
        return m_Words[index];
    }

    // Raw big-endian bytes, byte 0 being the most significant. Used for hashing and file formats.
    [[nodiscard]] std::array<UInt8, 16> ToBytes() const;

    friend constexpr auto operator<=>(const Guid& lhs, const Guid& rhs) noexcept = default;
    friend constexpr bool operator==(const Guid& lhs, const Guid& rhs) noexcept = default;

private:
    std::array<UInt32, 4> m_Words{};
};

static_assert(sizeof(Guid) == 16, "Guid must be exactly 16 bytes for the binary scene format");

// Hash functor so Guid works as a key in unordered containers.
struct GuidHash
{
    [[nodiscard]] std::size_t operator()(const Guid& guid) const noexcept;
};

}  // namespace Hollowpoint::Core

template <>
struct std::hash<Hollowpoint::Core::Guid>
{
    [[nodiscard]] std::size_t operator()(const Hollowpoint::Core::Guid& guid) const noexcept
    {
        return Hollowpoint::Core::GuidHash{}(guid);
    }
};
