#include <cctype>

#include "Hollowpoint/Core/Hash.h"

namespace Hollowpoint::Core
{

HashValue HashBytes(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return FnvOffsetBasis;
    }

    const UInt8* bytes = static_cast<const UInt8*>(data);
    HashValue hash = FnvOffsetBasis;
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<HashValue>(bytes[index]);
        hash *= FnvPrime;
    }
    return hash;
}

HashValue HashString(std::string_view text)
{
    return HashBytes(text.data(), text.size());
}

HashValue HashStringCaseInsensitive(std::string_view text)
{
    HashValue hash = FnvOffsetBasis;
    for (const char character : text)
    {
        // ASCII-only folding: identifiers in the engine, script globals and asset paths are all
        // ASCII, and locale-dependent folding would make hashes differ between machines.
        const char folded =
            (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
        hash ^= static_cast<HashValue>(static_cast<UInt8>(folded));
        hash *= FnvPrime;
    }
    return hash;
}

}  // namespace Hollowpoint::Core
