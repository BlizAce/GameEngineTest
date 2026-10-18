#pragma once

#include <string>
#include <type_traits>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Reflection/ReflectionTypes.h"

namespace Hollowpoint::Reflection
{

using Core::HashString;
using Core::HashValue;

// ---------------------------------------------------------------------------
// TypeTraits<T> — maps a C++ type to the ValueKind used to describe it.
//
// Anything not specialised below is described as a nested Struct and resolved by name at lookup time, so
// adding a new math or component type requires no change here — only a RegisterType call for it.
// ---------------------------------------------------------------------------

template <typename T, typename Enable = void>
struct TypeTraits
{
    static constexpr ValueKind Kind = ValueKind::Struct;
    using ElementType = void;  //!< Meaningful only when Kind == Array.
};

#define HP_REFLECT_PRIMITIVE(CppType, kind)     \
    template <>                                 \
    struct TypeTraits<CppType, void>            \
    {                                           \
        static constexpr ValueKind Kind = kind; \
        using ElementType = void;               \
    }

HP_REFLECT_PRIMITIVE(bool, ValueKind::Boolean);
HP_REFLECT_PRIMITIVE(char, ValueKind::Int8);
HP_REFLECT_PRIMITIVE(signed char, ValueKind::Int8);
HP_REFLECT_PRIMITIVE(unsigned char, ValueKind::UInt8);
HP_REFLECT_PRIMITIVE(short, ValueKind::Int16);
HP_REFLECT_PRIMITIVE(unsigned short, ValueKind::UInt16);
HP_REFLECT_PRIMITIVE(int, ValueKind::Int32);
HP_REFLECT_PRIMITIVE(unsigned int, ValueKind::UInt32);
HP_REFLECT_PRIMITIVE(long long, ValueKind::Int64);
HP_REFLECT_PRIMITIVE(unsigned long long, ValueKind::UInt64);
HP_REFLECT_PRIMITIVE(float, ValueKind::Float32);
HP_REFLECT_PRIMITIVE(double, ValueKind::Float64);

#undef HP_REFLECT_PRIMITIVE

template <>
struct TypeTraits<std::string, void>
{
    static constexpr ValueKind Kind = ValueKind::String;
    using ElementType = void;
};

// Enumerations are described by their underlying integer plus an EnumDescriptor, which lets the scene
// format store names instead of numbers (so reordering an enum does not silently reinterpret saves).
template <typename T>
struct TypeTraits<T, std::enable_if_t<std::is_enum_v<T>>>
{
    static constexpr ValueKind Kind = ValueKind::Enum;
    using ElementType = void;
};

template <typename TElement, typename TAllocator>
struct TypeTraits<std::vector<TElement, TAllocator>, void>
{
    static constexpr ValueKind Kind = ValueKind::Array;
    using ElementType = TElement;
};

}  // namespace Hollowpoint::Reflection
