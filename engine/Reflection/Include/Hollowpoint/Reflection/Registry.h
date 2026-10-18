#pragma once

#include <cstddef>
#include <limits>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Reflection/Descriptors.h"
#include "Hollowpoint/Reflection/NameInterner.h"
#include "Hollowpoint/Reflection/TypeTraits.h"

namespace Hollowpoint::Reflection
{

// ---------------------------------------------------------------------------
// Builders — fluent registration of fields.
//
// A builder is a lightweight handle (one reference) returned by value from Registry::RegisterType, which
// makes the registration macro a single expression with no lifetime questions:
//
//     HP_REGISTER_TYPE(Transform)
//         .Field("position", &Transform::Position)
//         .Field("rotation", &Transform::Rotation);
// ---------------------------------------------------------------------------

// Enumerated fields are exchanged as int64 with the serializer and UI; these accessors bridge to whatever
// integer representation the member actually declares.
using EnumAccessor = bool (*)(const void* address, Int64& outValue);
using EnumWriter = bool (*)(void* address, Int64 value);

namespace Detail
{

//! Reads an enum value stored as any integral type (including char/bool-sized ones) into int64.
template <typename TEnum>
[[nodiscard]] inline bool ReadEnumUnderlying(const void* address, Int64& outValue)
{
    using Underlying = std::underlying_type_t<TEnum>;
    const Underlying raw = *static_cast<const Underlying*>(address);
    outValue = static_cast<Int64>(raw);
    return true;
}

//! Writes an int64 into an enum member, clamping to the representable range of its underlying type so an
//! out-of-range scene value cannot wrap into a different enumerator.
template <typename TEnum>
[[nodiscard]] inline bool WriteEnumUnderlying(void* address, Int64 value)
{
    using Underlying = std::underlying_type_t<TEnum>;

    const Int64 lowest = static_cast<Int64>(std::numeric_limits<Underlying>::lowest());
    const Int64 highest = static_cast<Int64>(std::numeric_limits<Underlying>::max());

    if (value < lowest)
    {
        value = lowest;
    }
    else if (value > highest)
    {
        value = highest;
    }

    *static_cast<Underlying*>(address) = static_cast<Underlying>(value);
    return true;
}

//! Enum accessors for a non-enum field: never used, but the builder takes a uniform signature.
inline bool NoReadEnum(const void*, Int64&)
{
    return false;
}
inline bool NoWriteEnum(void*, Int64)
{
    return false;
}

}  // namespace Detail

class HP_REFLECTION_API TypeBuilder
{
public:
    //! Reflects a non-static data member. `name` is the serialized name and must stay stable.
    template <typename TOwner, typename TField>
    TypeBuilder& Field(std::string_view name, TField TOwner::* memberPointer);

    //! Reflects a member together with authoring metadata (ranges, tooltips, flags).
    template <typename TOwner, typename TField>
    TypeBuilder& Field(std::string_view name, TField TOwner::* memberPointer, const FieldAttributes& attributes);

    [[nodiscard]] const TypeDescriptor& GetType() const
    {
        return m_Type;
    }

private:
    friend class Registry;
    explicit TypeBuilder(TypeDescriptor& type) : m_Type(type) {}

    void AddRawField(const char* name, std::ptrdiff_t offset, ValueKind kind, const char* structTypeInfoName,
                     const char* arrayElementTypeInfoName, HashValue enumGuid, const FieldAttributes& attributes,
                     const ArrayOps& arrayOps, EnumAccessor readEnum, EnumWriter writeEnum);

    TypeDescriptor& m_Type;
};

class HP_REFLECTION_API EnumBuilder
{
public:
    //! Adds one enumeration value. `name` is what scene files store.
    template <typename TEnum>
    EnumBuilder& Value(std::string_view name, TEnum value);

private:
    friend class Registry;
    explicit EnumBuilder(EnumDescriptor& descriptor) : m_Descriptor(descriptor) {}

    void AddRawEntry(std::string_view name, Int64 value);

    EnumDescriptor& m_Descriptor;
};

// ---------------------------------------------------------------------------
// Offset computation.
//
// The standard offsetof takes a member *name*, not a pointer-to-member value, so it cannot be used from a
// template. Instead an object is constructed in aligned storage and the member's address measured — well
// defined, unlike the null-base reinterpret_cast trick, and free at runtime (startup only). Requiring
// default-constructibility costs nothing: reflected types must be default-constructible anyway for the ECS
// and the serializer to instantiate them.
// ---------------------------------------------------------------------------

namespace Detail
{

template <typename TOwner, typename TField>
[[nodiscard]] inline std::ptrdiff_t ComputeMemberOffset(TField TOwner::* memberPointer)
{
    static_assert(std::is_default_constructible_v<TOwner>,
                  "Reflected types must be default-constructible (the ECS and serializer create them too)");
    static_assert(!std::is_reference_v<TField>, "Reflected fields must be values, not references");

    alignas(TOwner) unsigned char storage[sizeof(TOwner)];
    TOwner* object = ::new (static_cast<void*>(storage)) TOwner();

    const std::byte* base = reinterpret_cast<const std::byte*>(storage);
    const std::byte* memberAddress =
        reinterpret_cast<const std::byte*>(static_cast<const void*>(&(object->*memberPointer)));
    const std::ptrdiff_t offset = memberAddress - base;

    object->~TOwner();

    // An out-of-range offset means the measurement went wrong; catching it here beats corrupting objects.
    HP_ASSERT_MSG(offset >= 0 && static_cast<std::size_t>(offset) <= sizeof(TOwner),
                  "Reflected field offset is outside its owner object");
    return offset;
}

//! Key used to resolve a nested struct/array element type back to its descriptor.
template <typename T>
[[nodiscard]] inline std::string_view TypeInfoName()
{
    return std::string_view(typeid(T).name());
}

}  // namespace Detail

// ---------------------------------------------------------------------------
// Registry — owns every descriptor for the process lifetime.
// ---------------------------------------------------------------------------

class HP_REFLECTION_API Registry
{
public:
    //! Function-local static so registration from translation-unit initialisers cannot depend on link
    //! order — which is exactly when component types register themselves.
    [[nodiscard]] static Registry& Get();

    // Lookups ---------------------------------------------------------------

    //! `name` is the readable registration name written into scene files ("Transform").
    [[nodiscard]] const TypeDescriptor* FindType(std::string_view name) const;

    //! Lookup by compiler type-identification string, used to resolve struct and array field sub-types.
    [[nodiscard]] const TypeDescriptor* FindTypeByTypeInfoName(std::string_view typeInfoName) const;

    [[nodiscard]] const TypeDescriptor* FindTypeByGuid(HashValue guid) const;
    [[nodiscard]] const EnumDescriptor* FindEnum(std::string_view name) const;
    [[nodiscard]] const EnumDescriptor* FindEnumByGuid(HashValue guid) const;

    //! Iteration for the editor's "Add Component" list, in registration order.
    [[nodiscard]] Size GetTypeCount() const;
    [[nodiscard]] const TypeDescriptor& GetTypeByRegistrationIndex(Size index) const;

    // Registration ----------------------------------------------------------

    //! Registers a type, or returns the existing descriptor when `name` is already registered. Declaring
    //! the same name for a different C++ type is reported: two classes sharing one scene-format name would
    //! silently corrupt saves.
    template <typename T>
    TypeBuilder RegisterType(std::string_view name, std::string_view baseTypeName = {});

    template <typename TEnum>
    EnumBuilder RegisterEnum(std::string_view name);

    //! Reports a reflection misuse (duplicate or mismatched name, unknown type, kind disagreement) through
    //! the engine assert handler, so it is visible in every build and capturable by tests.
    static void ReportError(std::string_view message);

private:
    Registry();
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    //! The builders are the only writers; they are declared friends so that registration is expressible as a
    //! fluent chain while keeping every other translation unit out of descriptor internals.
    friend class TypeBuilder;
    friend class EnumBuilder;

    TypeDescriptor& AllocateType(std::string_view name, std::string_view typeInfoName, Size size, Size align,
                                 std::string_view baseTypeName);
    EnumDescriptor& AllocateEnum(std::string_view name, std::string_view typeInfoName);
    void AddField(TypeDescriptor& type, const FieldDescriptor& field);
    void AddEnumEntry(EnumDescriptor& descriptor, std::string_view name, Int64 value);

    struct Impl;
    Impl* m_Impl = nullptr;  //!< Owned; pimpl keeps the maps out of the public header.
};

// ---------------------------------------------------------------------------
// Registration macros
//
// Each invocation is one expression: RegisterTypeOf returns a builder by value (it holds only a reference
// to the registry-owned descriptor), and the chained .Field calls return references to that same builder.
// Registration happens during static initialisation; field sub-types are resolved lazily, so the order of
// translation units cannot matter.
// ---------------------------------------------------------------------------

#define HP_REFLECTION_CONCAT_INNER(a, b) a##b
#define HP_REFLECTION_CONCAT(a, b) HP_REFLECTION_CONCAT_INNER(a, b)

// The macro declares a static builder object. A builder holds only a reference to the registry-owned
// descriptor, so it is trivially copyable and safe at namespace scope; being a named lvalue is what lets
// the chained .Field calls follow on the next line. Registration therefore costs one 8-byte global per
// type and no dynamic allocation beyond the descriptor itself.

//! Starts a type registration; chain .Field("name", &Type::Member).
#define HP_REGISTER_TYPE(TypeName)                                                                 \
    static ::Hollowpoint::Reflection::TypeBuilder HP_REFLECTION_CONCAT(HpTypeBuilder_, __LINE__) = \
        ::Hollowpoint::Reflection::RegisterTypeOf<TypeName>(#TypeName)

//! As above, naming the reflected base so the inspector groups inherited fields.
#define HP_REGISTER_TYPE_WITH_BASE(TypeName, BaseTypeName)                                         \
    static ::Hollowpoint::Reflection::TypeBuilder HP_REFLECTION_CONCAT(HpTypeBuilder_, __LINE__) = \
        ::Hollowpoint::Reflection::RegisterTypeOf<TypeName>(#TypeName, #BaseTypeName)

//! Starts an enumeration registration; chain .Value("Name", EnumType::Member).
#define HP_REGISTER_ENUM(EnumTypeName)                                                             \
    static ::Hollowpoint::Reflection::EnumBuilder HP_REFLECTION_CONCAT(HpEnumBuilder_, __LINE__) = \
        ::Hollowpoint::Reflection::RegisterEnumOf<EnumTypeName>(#EnumTypeName)

template <typename T>
[[nodiscard]] inline TypeBuilder RegisterTypeOf(std::string_view name, std::string_view baseTypeName = {})
{
    return Registry::Get().RegisterType<T>(name, baseTypeName);
}

template <typename TEnum>
[[nodiscard]] inline EnumBuilder RegisterEnumOf(std::string_view name)
{
    return Registry::Get().RegisterEnum<TEnum>(name);
}

// ---------------------------------------------------------------------------
// Template definitions
// ---------------------------------------------------------------------------

template <typename TOwner, typename TField>
TypeBuilder& TypeBuilder::Field(std::string_view name, TField TOwner::* memberPointer)
{
    return Field(name, memberPointer, FieldAttributes{});
}

template <typename TOwner, typename TField>
TypeBuilder& TypeBuilder::Field(std::string_view name, TField TOwner::* memberPointer,
                                const FieldAttributes& attributes)
{
    using Traits = TypeTraits<TField>;

    // Names are interned once here so the descriptor stores a stable pointer and later lookups never build a
    // std::string.
    const char* internedName = InternName(name);
    const char* structTypeInfoName = nullptr;
    const char* arrayElementTypeInfoName = nullptr;
    HashValue enumGuid = 0;

    ArrayOps arrayOps;
    EnumAccessor readEnum = &Detail::NoReadEnum;
    EnumWriter writeEnum = &Detail::NoWriteEnum;

    if constexpr (Traits::Kind == ValueKind::Struct)
    {
        structTypeInfoName = InternName(Detail::TypeInfoName<TField>());
    }
    else if constexpr (Traits::Kind == ValueKind::Array)
    {
        using Element = typename Traits::ElementType;
        arrayElementTypeInfoName = InternName(Detail::TypeInfoName<Element>());
        arrayOps = Detail::MakeArrayOps<std::vector<Element>>();
    }
    else if constexpr (Traits::Kind == ValueKind::Enum)
    {
        // The enum descriptor is keyed by the same type-info string, so no registration boilerplate is needed.
        enumGuid = HashString(Detail::TypeInfoName<TField>());
        readEnum = &Detail::ReadEnumUnderlying<TField>;
        writeEnum = &Detail::WriteEnumUnderlying<TField>;
    }

    AddRawField(internedName, Detail::ComputeMemberOffset<TOwner, TField>(memberPointer), Traits::Kind,
                structTypeInfoName, arrayElementTypeInfoName, enumGuid, attributes, arrayOps, readEnum, writeEnum);
    return *this;
}

template <typename TEnum>
EnumBuilder& EnumBuilder::Value(std::string_view name, TEnum value)
{
    static_assert(std::is_enum_v<TEnum>, "EnumBuilder::Value requires an enumeration value");
    AddRawEntry(name, static_cast<Int64>(value));
    return *this;
}

template <typename T>
TypeBuilder Registry::RegisterType(std::string_view name, std::string_view baseTypeName)
{
    TypeDescriptor& descriptor = AllocateType(name, Detail::TypeInfoName<T>(), sizeof(T), alignof(T), baseTypeName);
    return TypeBuilder(descriptor);
}

template <typename TEnum>
EnumBuilder Registry::RegisterEnum(std::string_view name)
{
    static_assert(std::is_enum_v<TEnum>, "RegisterEnum requires an enumeration type");
    // Keyed by both the readable name (scene files, UI) and the compiler type-info name (what a field of
    // this enum type resolves through), so neither registration order nor a rename breaks the other key.
    EnumDescriptor& descriptor = AllocateEnum(name, Detail::TypeInfoName<TEnum>());
    return EnumBuilder(descriptor);
}

}  // namespace Hollowpoint::Reflection
