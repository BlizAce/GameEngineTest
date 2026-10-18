#pragma once

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include "Hollowpoint/Reflection/Detail/ScalarConvert.h"
#include "Hollowpoint/Reflection/ReflectionTypes.h"
#include "Hollowpoint/Reflection/TypeTraits.h"

namespace Hollowpoint::Reflection
{

class Registry;
class TypeDescriptor;
class EnumDescriptor;

// ---------------------------------------------------------------------------
// ArrayOps — type-erased operations for a reflected std::vector<T>.
//
// Everything about the element type is captured at registration, from the concrete member type: count, data,
// resize, and scalar/enum element access. That means nothing downstream has to assume how a vector is laid
// out (a hand-rolled three-pointer struct compiles and then breaks on another standard library) or what an
// element's stride is. Arrays of scalars work too, which a descriptor-only design could not express — a
// std::vector<float> has no TypeDescriptor but still needs saving and editing.
//
// Function pointers cost one indirection on paths that are nowhere near hot (inspector redraw, scene load),
// in exchange for removing an entire class of layout bugs.
// ---------------------------------------------------------------------------

struct ScalarValue;

struct ArrayOps
{
    ValueKind ElementKind = ValueKind::Invalid;
    Size ElementSize = 0;

    Size (*Count)(const void* array) = nullptr;
    const void* (*Data)(const void* array) = nullptr;
    //! Default-constructs elements up to `newCount`, destroying the surplus. Returns the new first element.
    void* (*Resize)(void* array, Size newCount) = nullptr;
    //! Appends one default-constructed element and returns its address (reallocation-safe).
    void* (*PushBackDefault)(void* array) = nullptr;

    //! Element access for scalar kinds (including enums, which need the underlying-type bridge).
    bool (*ReadElementScalar)(const void* array, Size index, ScalarValue& outValue) = nullptr;
    bool (*WriteElementScalar)(void* array, Size index, const ScalarValue& value) = nullptr;
};

namespace Detail
{

template <typename TVector>
[[nodiscard]] inline Size VectorCount(const void* array)
{
    return static_cast<const TVector*>(array)->size();
}

template <typename TVector>
[[nodiscard]] inline const void* VectorData(const void* array)
{
    return static_cast<const void*>(static_cast<const TVector*>(array)->data());
}

template <typename TVector>
[[nodiscard]] inline void* VectorResize(void* array, Size newCount)
{
    TVector* vector = static_cast<TVector*>(array);
    vector->resize(newCount);
    return static_cast<void*>(vector->data());
}

template <typename TVector>
[[nodiscard]] inline void* VectorPushBackDefault(void* array)
{
    TVector* vector = static_cast<TVector*>(array);
    vector->emplace_back();
    // Re-reads back(): emplace_back may reallocate, and the caller wants the *new* element's address.
    return static_cast<void*>(&vector->back());
}

//! Reads one element as a scalar. Enum elements are widened from their underlying integer type; strings and
//! nested structs have no scalar form and report failure so the caller takes its own path.
template <typename TVector>
[[nodiscard]] inline bool VectorReadElementScalar(const void* array, Size index, ScalarValue& outValue)
{
    const TVector* vector = static_cast<const TVector*>(array);
    if (index >= vector->size())
    {
        return false;
    }

    using Element = typename TVector::value_type;

    if constexpr (std::is_enum_v<Element>)
    {
        // Cast through the underlying type rather than reinterpreting the object: a static_cast from an enum
        // to an integer is well defined, while aliasing the object as its underlying type is not.
        outValue = ScalarValue::FromSigned(
            ValueKind::Enum, static_cast<Int64>(static_cast<std::underlying_type_t<Element>>((*vector)[index])));
        return true;
    }
    else if constexpr (std::is_same_v<Element, bool>)
    {
        outValue = ScalarValue::FromBool((*vector)[index]);
        return true;
    }
    else if constexpr (std::is_floating_point_v<Element>)
    {
        outValue = ScalarValue::FromReal(TypeTraits<Element>::Kind, static_cast<double>((*vector)[index]));
        return true;
    }
    else if constexpr (std::is_signed_v<Element>)
    {
        outValue = ScalarValue::FromSigned(TypeTraits<Element>::Kind, static_cast<Int64>((*vector)[index]));
        return true;
    }
    else if constexpr (std::is_unsigned_v<Element>)
    {
        outValue = ScalarValue::FromUnsigned(TypeTraits<Element>::Kind, static_cast<UInt64>((*vector)[index]));
        return true;
    }
    else
    {
        // std::string or a reflected struct: no scalar representation.
        HP_UNUSED(outValue);
        return false;
    }
}

//! Writes one element from a scalar, using the same clamping rules as a direct field write so that editing a
//! value in the inspector and loading it from a scene cannot store different bytes.
template <typename TVector>
[[nodiscard]] inline bool VectorWriteElementScalar(void* array, Size index, const ScalarValue& value)
{
    TVector* vector = static_cast<TVector*>(array);
    if (index >= vector->size())
    {
        return false;
    }

    using Element = typename TVector::value_type;

    if constexpr (std::is_enum_v<Element>)
    {
        using Underlying = std::underlying_type_t<Element>;
        const Int64 lowest = static_cast<Int64>(std::numeric_limits<Underlying>::lowest());
        const Int64 highest = static_cast<Int64>(std::numeric_limits<Underlying>::max());
        Int64 raw = ToSigned(value);
        if (raw < lowest)
        {
            raw = lowest;
        }
        else if (raw > highest)
        {
            raw = highest;
        }
        (*vector)[index] = static_cast<Element>(raw);
        return true;
    }
    else if constexpr (std::is_same_v<Element, bool>)
    {
        // Must land as a canonical 0/1 or reads of it as a bool are undefined.
        (*vector)[index] = ToSigned(value) != 0;
        return true;
    }
    else if constexpr (std::is_floating_point_v<Element>)
    {
        (*vector)[index] = ClampRealTo<Element>(value);
        return true;
    }
    else if constexpr (std::is_signed_v<Element>)
    {
        (*vector)[index] = ClampSignedTo<Element>(value);
        return true;
    }
    else if constexpr (std::is_unsigned_v<Element>)
    {
        (*vector)[index] = ClampUnsignedTo<Element>(value);
        return true;
    }
    else
    {
        HP_UNUSED(value);
        return false;
    }
}

template <typename TVector>
[[nodiscard]] inline ArrayOps MakeArrayOps()
{
    using Element = typename TVector::value_type;

    ArrayOps ops;
    ops.ElementKind = TypeTraits<Element>::Kind;
    ops.ElementSize = sizeof(Element);
    ops.Count = &VectorCount<TVector>;
    ops.Data = &VectorData<TVector>;
    ops.Resize = &VectorResize<TVector>;
    ops.PushBackDefault = &VectorPushBackDefault<TVector>;
    ops.ReadElementScalar = &VectorReadElementScalar<TVector>;
    ops.WriteElementScalar = &VectorWriteElementScalar<TVector>;
    return ops;
}

}  // namespace Detail

// ---------------------------------------------------------------------------
// EnumDescriptor — a reflected scoped enumeration.
// ---------------------------------------------------------------------------

class HP_REFLECTION_API EnumDescriptor
{
public:
    [[nodiscard]] const char* GetName() const
    {
        return m_Name;
    }
    [[nodiscard]] HashValue GetGuid() const
    {
        return m_Guid;
    }
    [[nodiscard]] Size GetEntryCount() const
    {
        return m_EntryCount;
    }
    [[nodiscard]] const EnumEntry& GetEntry(Size index) const;

    [[nodiscard]] bool FindValueByName(std::string_view name, Int64& outValue) const;
    //! nullptr when the value is not part of the enumeration (e.g. a scene file written by a newer build).
    [[nodiscard]] const char* FindNameByValue(Int64 value) const;

private:
    friend class Registry;

    // Enumerations are small and registered once, so an inline array avoids per-entry allocation. The bound
    // is generous for component state enums; exceeding it is reported rather than silently truncated.
    static constexpr Size MaxEntries = 64;

    const char* m_Name = nullptr;
    HashValue m_Guid = 0;
    EnumEntry m_Entries[MaxEntries];
    Size m_EntryCount = 0;
};

// ---------------------------------------------------------------------------
// FieldDescriptor — one reflected member of a type.
// ---------------------------------------------------------------------------

class HP_REFLECTION_API FieldDescriptor
{
    // The registry builds descriptors; nothing else may write them, which is what makes "descriptors are
    // immutable once registered" a property of the type rather than a convention.
    friend class Registry;
    friend class TypeBuilder;
    // TypeDescriptor owns an array of these and scans them by name.
    friend class TypeDescriptor;

public:
    [[nodiscard]] const char* GetName() const
    {
        return m_Name;
    }
    [[nodiscard]] std::ptrdiff_t GetOffset() const
    {
        return m_Offset;
    }
    [[nodiscard]] ValueKind GetKind() const
    {
        return m_Kind;
    }

    //! Struct and array-element types resolve lazily by name so registration order cannot matter. Returns
    //! nullptr for kinds without a sub-type, and also when the named type was never registered — callers
    //! must treat null as "cannot traverse this field", not as "empty struct".
    [[nodiscard]] const TypeDescriptor* GetStructType() const;
    [[nodiscard]] const TypeDescriptor* GetArrayElementType() const;
    [[nodiscard]] const EnumDescriptor* GetEnum() const;

    [[nodiscard]] const FieldAttributes& GetAttributes() const
    {
        return m_Attributes;
    }
    [[nodiscard]] bool HasFlag(FieldFlag flag) const
    {
        return (m_Attributes.Flags & static_cast<UInt32>(flag)) != 0u;
    }

    //! Address of this field within a live instance of the owning type.
    [[nodiscard]] void* GetAddress(void* object) const;
    [[nodiscard]] const void* GetAddress(const void* object) const;

    //! Type-erased vector operations (registered from the concrete member type). Empty ops for non-arrays.
    [[nodiscard]] const ArrayOps& GetArrayOps() const
    {
        return m_ArrayOps;
    }

    //! Reports that this field's sub-type could not be resolved, at most once per field.
    //!
    //! Traversal happens on every inspector redraw and every scene save, so an unconditional diagnostic for a
    //! registration bug would bury the log — and a warning nobody reads is worse than none, because it teaches
    //! people to ignore the channel. The flag is mutable and written only on the error path; a benign race
    //! between two threads could print the line twice, which is acceptable against unbounded spam.
    void ReportUnresolvedTypeOnce(const char* what) const;

    //! Reads/writes an enumerated field as int64 whatever its underlying integer type and signedness. The
    //! accessors are captured at registration from the member's declared type, so nothing downstream has to
    //! guess how the enum is stored (which matters: `enum : UInt8` and a plain `enum` differ).
    [[nodiscard]] bool ReadEnumAt(const void* address, Int64& outValue) const;
    [[nodiscard]] bool WriteEnumAt(void* address, Int64 value) const;

private:
    const char* m_Name = nullptr;
    std::ptrdiff_t m_Offset = 0;
    ValueKind m_Kind = ValueKind::Invalid;

    // Resolved on demand via the registry (see GetStructType).
    const char* m_StructTypeName = nullptr;
    const char* m_ArrayElementTypeName = nullptr;
    HashValue m_EnumGuid = 0;

    FieldAttributes m_Attributes;
    ArrayOps m_ArrayOps;

    bool (*m_ReadEnum)(const void* address, Int64& outValue) = nullptr;
    bool (*m_WriteEnum)(void* address, Int64 value) = nullptr;

    mutable bool m_UnresolvedTypeReported = false;
};

// ---------------------------------------------------------------------------
// TypeDescriptor — a reflected struct/class and its fields.
// ---------------------------------------------------------------------------

class HP_REFLECTION_API TypeDescriptor
{
public:
    [[nodiscard]] const char* GetName() const
    {
        return m_Name;
    }
    [[nodiscard]] HashValue GetGuid() const
    {
        return m_Guid;
    }
    [[nodiscard]] Size GetSize() const
    {
        return m_Size;
    }
    [[nodiscard]] Size GetAlign() const
    {
        return m_Align;
    }

    [[nodiscard]] Size GetFieldCount() const
    {
        return m_FieldCount;
    }
    [[nodiscard]] const FieldDescriptor& GetField(Size index) const;
    [[nodiscard]] const FieldDescriptor* FindField(std::string_view name) const;

    //! Base type for inheritance-aware traversal (the inspector shows inherited fields too). Null when there
    //! is no reflected base.
    [[nodiscard]] const TypeDescriptor* GetBaseType() const
    {
        return m_BaseType;
    }

private:
    friend class Registry;

    const char* m_Name = nullptr;
    HashValue m_Guid = 0;
    Size m_Size = 0;
    Size m_Align = 0;
    const TypeDescriptor* m_BaseType = nullptr;

    // Fields live in a heap block owned by the registry. Component types range from two to thirty fields and
    // are described once at startup, so one allocation per type is the right trade over an inline buffer that
    // would have to be large enough for the worst case.
    FieldDescriptor* m_Fields = nullptr;
    Size m_FieldCount = 0;
    Size m_FieldCapacity = 0;
};

}  // namespace Hollowpoint::Reflection
