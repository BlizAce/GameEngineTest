#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>

#include "Hollowpoint/Core/Guid.h"

namespace Hollowpoint::Core
{

namespace
{

// Per-thread generator. std::random_device is the entropy source; on some platforms it is a weak
// PRNG, so we additionally mix in a process-wide atomic counter and the address of the generator
// itself. Uniqueness then does not depend on the quality of random_device alone — collisions across
// threads and processes require an astronomically unlikely coincidence of all three inputs.
struct GuidGenerator
{
    std::mt19937_64 Engine;

    GuidGenerator()
    {
        std::random_device randomDevice;
        std::uint64_t seed = 0;
        for (int index = 0; index < 4; ++index)
        {
            seed = (seed << 32) ^ static_cast<std::uint64_t>(randomDevice());
        }
        static std::atomic<std::uint64_t> s_ProcessCounter{0};
        const std::uint64_t counterMix = s_ProcessCounter.fetch_add(1, std::memory_order_relaxed);
        const std::uintptr_t addressMix = reinterpret_cast<std::uintptr_t>(this);

        seed ^= counterMix + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        seed ^= static_cast<std::uint64_t>(addressMix) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);

        Engine.seed(seed);
    }
};

int HexDigitValue(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

}  // namespace

Guid Guid::Generate()
{
    thread_local GuidGenerator generator;

    UInt32 words[4] = {0, 0, 0, 0};
    for (int index = 0; index < 4; ++index)
    {
        words[index] = static_cast<UInt32>(generator.Engine());
    }

    // RFC 4122: version 4 in the high nibble of word1, variant 1 (10xx) in the top bits of word2.
    words[1] = (words[1] & 0xFFFF0FFFu) | 0x00004000u;
    words[2] = (words[2] & 0x3FFFFFFFu) | 0x80000000u;

    const Guid result(words[0], words[1], words[2], words[3]);
    // A nil result would silently mean "no reference", so regenerate. Probability ~2^-122.
    if (result.IsNil())
    {
        return Generate();
    }
    return result;
}

std::optional<Guid> Guid::Parse(std::string_view text)
{
    // Collect hex digits, skipping hyphens. Anything else is invalid.
    UInt8 digits[32];
    std::size_t digitCount = 0;

    for (const char character : text)
    {
        if (character == '-')
        {
            continue;
        }
        const int value = HexDigitValue(character);
        if (value < 0 || digitCount >= 32)
        {
            return std::nullopt;
        }
        digits[digitCount++] = static_cast<UInt8>(value);
    }

    if (digitCount != 32)
    {
        return std::nullopt;
    }

    UInt32 words[4] = {0, 0, 0, 0};
    for (std::size_t index = 0; index < 32; ++index)
    {
        words[index / 8] = (words[index / 8] << 4) | static_cast<UInt32>(digits[index]);
    }

    return Guid(words[0], words[1], words[2], words[3]);
}

Guid Guid::ParseOrNil(std::string_view text)
{
    const std::optional<Guid> parsed = Parse(text);
    return parsed.has_value() ? *parsed : Guid{};
}

std::string Guid::ToString() const
{
    // 8-4-4-4-12 canonical form, lower case.
    char buffer[37];
    const UInt64 high = (static_cast<UInt64>(m_Words[0]) << 32) | m_Words[1];
    const UInt64 low = (static_cast<UInt64>(m_Words[2]) << 32) | m_Words[3];

    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(m_Words[0]),
                  static_cast<unsigned>((high >> 16) & 0xFFFFu), static_cast<unsigned>(high & 0xFFFFu),
                  static_cast<unsigned>((low >> 48) & 0xFFFFu),
                  static_cast<unsigned long long>(low & 0x0000FFFFFFFFFFFFull));
    return std::string(buffer);
}

std::array<UInt8, 16> Guid::ToBytes() const
{
    std::array<UInt8, 16> bytes{};
    for (std::size_t word = 0; word < 4; ++word)
    {
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            bytes[word * 4 + byte] = static_cast<UInt8>((m_Words[word] >> (24 - 8 * byte)) & 0xFFu);
        }
    }
    return bytes;
}

std::size_t GuidHash::operator()(const Guid& guid) const noexcept
{
    // Mix the four words with the same SplitMix64 finalizer used by CombineHash so that hashes of
    // Guids are well distributed in low-bit-indexed buckets.
    HashValue hash = 0x243f6a8885a308d3ull;
    for (std::size_t index = 0; index < 4; ++index)
    {
        hash ^= static_cast<HashValue>(guid.GetWord(index));
        hash *= 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
    }
    return static_cast<std::size_t>(hash);
}

}  // namespace Hollowpoint::Core
