#include <string>
#include <unordered_map>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Hash.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Reflection/NameInterner.h"
#include "Hollowpoint/Reflection/Registry.h"

namespace Hollowpoint::Reflection
{

using Core::HashString;

const char* ValueKindName(ValueKind kind)
{
    switch (kind)
    {
        case ValueKind::Invalid:
            return "Invalid";
        case ValueKind::Boolean:
            return "bool";
        case ValueKind::Int8:
            return "int8";
        case ValueKind::Int16:
            return "int16";
        case ValueKind::Int32:
            return "int32";
        case ValueKind::Int64:
            return "int64";
        case ValueKind::UInt8:
            return "uint8";
        case ValueKind::UInt16:
            return "uint16";
        case ValueKind::UInt32:
            return "uint32";
        case ValueKind::UInt64:
            return "uint64";
        case ValueKind::Float32:
            return "float";
        case ValueKind::Float64:
            return "double";
        case ValueKind::String:
            return "string";
        case ValueKind::Enum:
            return "enum";
        case ValueKind::Struct:
            return "struct";
        case ValueKind::Array:
            return "array";
    }

    // Outside the known range means memory corruption or a kind written by a newer build; reporting beats
    // inventing a name that would hide the real type in logs.
    HP_ASSERT_MSG(false, "ValueKind is outside the known range");
    return "?";
}

ScalarValue ScalarValue::FromBool(bool value)
{
    ScalarValue result;
    result.Kind = ValueKind::Boolean;
    result.AsBool = value;
    return result;
}

ScalarValue ScalarValue::FromSigned(ValueKind kind, Int64 value)
{
    ScalarValue result;
    result.Kind = kind;
    result.AsInt = value;
    return result;
}

ScalarValue ScalarValue::FromUnsigned(ValueKind kind, UInt64 value)
{
    ScalarValue result;
    result.Kind = kind;
    result.AsUnsigned = value;
    return result;
}

ScalarValue ScalarValue::FromReal(ValueKind kind, double value)
{
    ScalarValue result;
    result.Kind = kind;
    result.AsReal = value;
    return result;
}

bool ScalarValue::IsNumericKind(ValueKind kind)
{
    switch (kind)
    {
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::UInt8:
        case ValueKind::UInt16:
        case ValueKind::UInt32:
        case ValueKind::UInt64:
        case ValueKind::Float32:
        case ValueKind::Float64:
            return true;
        default:
            return false;
    }
}

bool ScalarValue::IsIntegerKind(ValueKind kind)
{
    switch (kind)
    {
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::UInt8:
        case ValueKind::UInt16:
        case ValueKind::UInt32:
        case ValueKind::UInt64:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Registry state.
// ---------------------------------------------------------------------------

struct Registry::Impl
{
    // Registration order, for the editor's component list.
    std::vector<TypeDescriptor*> Types;
    std::vector<EnumDescriptor*> Enums;

    // Two name spaces per kind: the readable name is what scene files and the UI use, the compiler
    // type-info name is what a nested field resolves through. Both map to the same descriptor.
    std::unordered_map<std::string, TypeDescriptor*> TypesByName;
    std::unordered_map<std::string, TypeDescriptor*> TypesByTypeInfoName;
    std::unordered_map<HashValue, TypeDescriptor*> TypesByGuid;

    std::unordered_map<std::string, EnumDescriptor*> EnumsByName;
    std::unordered_map<std::string, EnumDescriptor*> EnumsByTypeInfoName;
    std::unordered_map<HashValue, EnumDescriptor*> EnumsByGuid;
};

Registry& Registry::Get()
{
    // Leaked singleton: descriptors are handed out as raw pointers that scenes and UI code hold, and they
    // must stay readable during shutdown (the editor draws an inspector until the process exits). The
    // footprint is a few dozen descriptors.
    static Registry* instance = new Registry();
    return *instance;
}

Registry::Registry() : m_Impl(new Impl()) {}

Registry::~Registry()
{
    for (TypeDescriptor* descriptor : m_Impl->Types)
    {
        delete[] descriptor->m_Fields;
        delete descriptor;
    }
    for (EnumDescriptor* descriptor : m_Impl->Enums)
    {
        delete descriptor;
    }
    delete m_Impl;
}

void Registry::ReportError(std::string_view message)
{
    // Assert first (visible in every build, and debuggers break here), then log so the reason survives into
    // a release log file. Execution continues: a bad registration is a bug in the engine or a plugin, but it
    // must not stop a player from loading a scene they already own.
    HP_ASSERT_MSG(false, "Reflection registry misuse");
    HP_LOG_ERROR("REFLECT", "Reflection: {}", message);
}

TypeDescriptor& Registry::AllocateType(std::string_view name, std::string_view typeInfoName, Size size, Size align,
                                       std::string_view baseTypeName)
{
    const std::string typeName(name);

    auto existing = m_Impl->TypesByName.find(typeName);
    if (existing != m_Impl->TypesByName.end())
    {
        TypeDescriptor& descriptor = *existing->second;

        // Two different C++ types claiming one scene-format name would make saves ambiguous — a far more
        // expensive bug to diagnose later than a hard failure now.
        if (descriptor.m_Guid != HashString(typeInfoName))
        {
            ReportError("Type name '" + typeName + "' is registered for two different C++ types");
        }
        return descriptor;
    }

    TypeDescriptor* descriptor = new TypeDescriptor();
    descriptor->m_Name = InternName(name);
    descriptor->m_Size = size;
    descriptor->m_Align = align;
    descriptor->m_Guid = HashString(typeInfoName);

    m_Impl->Types.push_back(descriptor);
    m_Impl->TypesByName.emplace(typeName, descriptor);
    m_Impl->TypesByTypeInfoName.emplace(std::string(typeInfoName), descriptor);
    m_Impl->TypesByGuid.emplace(descriptor->m_Guid, descriptor);

    if (!baseTypeName.empty())
    {
        auto base = m_Impl->TypesByName.find(std::string(baseTypeName));
        if (base != m_Impl->TypesByName.end())
        {
            descriptor->m_BaseType = base->second;
        }
        else
        {
            // A missing base is a real defect: the inspector would silently show an incomplete component.
            // Registration order cannot cause this because HP_REGISTER_TYPE_WITH_BASE requires the base to be
            // registered first — enforced here rather than by link-order luck, and tested.
            ReportError("Base type '" + std::string(baseTypeName) + "' of '" + typeName + "' is not registered");
        }
    }

    return *descriptor;
}

EnumDescriptor& Registry::AllocateEnum(std::string_view name, std::string_view typeInfoName)
{
    const std::string enumName(name);

    auto existing = m_Impl->EnumsByName.find(enumName);
    if (existing != m_Impl->EnumsByName.end())
    {
        return *existing->second;
    }

    EnumDescriptor* descriptor = new EnumDescriptor();
    descriptor->m_Name = InternName(name);
    // The GUID is derived from the compiler type name so a field of this enum type resolves to it without
    // the registration site repeating the name (see FieldDescriptor::GetEnum).
    descriptor->m_Guid = HashString(typeInfoName);

    m_Impl->Enums.push_back(descriptor);
    m_Impl->EnumsByName.emplace(enumName, descriptor);
    m_Impl->EnumsByTypeInfoName.emplace(std::string(typeInfoName), descriptor);
    m_Impl->EnumsByGuid.emplace(descriptor->m_Guid, descriptor);

    return *descriptor;
}

void Registry::AddField(TypeDescriptor& type, const FieldDescriptor& field)
{
    // Grow in powers of two. A type registers once at startup, so amortising is sufficient and keeping the
    // descriptors contiguous is what makes inspector iteration cheap.
    if (type.m_FieldCount == type.m_FieldCapacity)
    {
        const Size newCapacity = type.m_FieldCapacity == 0 ? 8 : type.m_FieldCapacity * 2;
        FieldDescriptor* grown = new FieldDescriptor[newCapacity];
        for (Size index = 0; index < type.m_FieldCount; ++index)
        {
            grown[index] = type.m_Fields[index];
        }
        delete[] type.m_Fields;
        type.m_Fields = grown;
        type.m_FieldCapacity = newCapacity;
    }

    if (field.m_Name == nullptr)
    {
        ReportError("Reflected field with no name");
        return;
    }

    // A duplicate key would make the serialized form ambiguous — two members claiming one name, so a load
    // could write either. The second registration is dropped rather than stored: keeping the descriptor set
    // consistent matters more than preserving a field that cannot be addressed by name anyway.
    if (type.FindField(field.m_Name) != nullptr)
    {
        ReportError(std::string("Duplicate field name '") + field.m_Name + "' in reflected type '" +
                    (type.GetName() != nullptr ? type.GetName() : "?") + "'");
        return;
    }

    type.m_Fields[type.m_FieldCount++] = field;
}

void Registry::AddEnumEntry(EnumDescriptor& descriptor, std::string_view name, Int64 value)
{
    if (descriptor.m_EntryCount >= EnumDescriptor::MaxEntries)
    {
        ReportError("Enumeration exceeds the maximum number of reflected entries");
        return;
    }

    descriptor.m_Entries[descriptor.m_EntryCount].Name = InternName(name);
    descriptor.m_Entries[descriptor.m_EntryCount].Value = value;
    ++descriptor.m_EntryCount;
}

const TypeDescriptor* Registry::FindType(std::string_view name) const
{
    auto found = m_Impl->TypesByName.find(std::string(name));
    return found != m_Impl->TypesByName.end() ? found->second : nullptr;
}

const TypeDescriptor* Registry::FindTypeByTypeInfoName(std::string_view typeInfoName) const
{
    auto found = m_Impl->TypesByTypeInfoName.find(std::string(typeInfoName));
    return found != m_Impl->TypesByTypeInfoName.end() ? found->second : nullptr;
}

const TypeDescriptor* Registry::FindTypeByGuid(HashValue guid) const
{
    auto found = m_Impl->TypesByGuid.find(guid);
    return found != m_Impl->TypesByGuid.end() ? found->second : nullptr;
}

const EnumDescriptor* Registry::FindEnum(std::string_view name) const
{
    auto found = m_Impl->EnumsByName.find(std::string(name));
    return found != m_Impl->EnumsByName.end() ? found->second : nullptr;
}

const EnumDescriptor* Registry::FindEnumByGuid(HashValue guid) const
{
    auto found = m_Impl->EnumsByGuid.find(guid);
    return found != m_Impl->EnumsByGuid.end() ? found->second : nullptr;
}

Size Registry::GetTypeCount() const
{
    return m_Impl->Types.size();
}

const TypeDescriptor& Registry::GetTypeByRegistrationIndex(Size index) const
{
    HP_ASSERT_MSG(index < m_Impl->Types.size(), "Type registration index out of range");
    return *m_Impl->Types[index];
}

void TypeBuilder::AddRawField(const char* name, std::ptrdiff_t offset, ValueKind kind, const char* structTypeInfoName,
                              const char* arrayElementTypeInfoName, HashValue enumGuid,
                              const FieldAttributes& attributes, const ArrayOps& arrayOps, EnumAccessor readEnum,
                              EnumWriter writeEnum)
{
    FieldDescriptor field;
    field.m_Name = name;
    field.m_Offset = offset;
    field.m_Kind = kind;
    field.m_EnumGuid = enumGuid;
    field.m_Attributes = attributes;
    field.m_StructTypeName = structTypeInfoName;
    field.m_ArrayElementTypeName = arrayElementTypeInfoName;
    field.m_ArrayOps = arrayOps;
    field.m_ReadEnum = readEnum;
    field.m_WriteEnum = writeEnum;

    Registry::Get().AddField(m_Type, field);
}

void EnumBuilder::AddRawEntry(std::string_view name, Int64 value)
{
    Registry::Get().AddEnumEntry(m_Descriptor, name, value);
}

// ---------------------------------------------------------------------------
// Descriptor accessors that need the registry.
// ---------------------------------------------------------------------------

const TypeDescriptor* FieldDescriptor::GetStructType() const
{
    if (m_Kind != ValueKind::Struct || m_StructTypeName == nullptr)
    {
        return nullptr;
    }
    // A struct field whose type was never registered resolves to null: the caller must treat that as "cannot
    // traverse", not as an empty struct. Reported once so a missing registration is visible in a debug run.
    const TypeDescriptor* structType = Registry::Get().FindTypeByTypeInfoName(m_StructTypeName);
    if (structType == nullptr)
    {
        ReportUnresolvedTypeOnce("struct field type");
    }
    return structType;
}

const TypeDescriptor* FieldDescriptor::GetArrayElementType() const
{
    if (m_Kind != ValueKind::Array || m_ArrayElementTypeName == nullptr)
    {
        return nullptr;
    }
    // Arrays of scalars, strings and enums have no element descriptor by design — a std::vector<float> is
    // perfectly saveable through ArrayOps alone. Returning null must therefore be silent: reporting it was a
    // false alarm fired for the most common array shape in the engine (UVs, indices, sample data). Only an
    // array whose elements are *supposed* to be reflected structs and cannot be resolved is a real defect.
    if (m_ArrayOps.ElementKind != ValueKind::Struct)
    {
        return nullptr;
    }

    const TypeDescriptor* elementType = Registry::Get().FindTypeByTypeInfoName(m_ArrayElementTypeName);
    if (elementType == nullptr)
    {
        ReportUnresolvedTypeOnce("array element type");
    }
    return elementType;
}

const EnumDescriptor* FieldDescriptor::GetEnum() const
{
    if (m_Kind != ValueKind::Enum || m_EnumGuid == 0)
    {
        return nullptr;
    }
    const EnumDescriptor* descriptor = Registry::Get().FindEnumByGuid(m_EnumGuid);
    if (descriptor == nullptr)
    {
        // An enum member registered without its enum loses the name mapping, so a scene would store bare
        // numbers and an enumerator rename would silently reinterpret them.
        ReportUnresolvedTypeOnce("enum type");
    }
    return descriptor;
}

bool FieldDescriptor::ReadEnumAt(const void* address, Int64& outValue) const
{
    if (m_Kind != ValueKind::Enum || m_ReadEnum == nullptr)
    {
        return false;
    }
    return m_ReadEnum(address, outValue);
}

bool FieldDescriptor::WriteEnumAt(void* address, Int64 value) const
{
    if (m_Kind != ValueKind::Enum || m_WriteEnum == nullptr)
    {
        return false;
    }
    return m_WriteEnum(address, value);
}

void FieldDescriptor::ReportUnresolvedTypeOnce(const char* what) const
{
    if (m_UnresolvedTypeReported)
    {
        return;
    }
    m_UnresolvedTypeReported = true;

    HP_LOG_ERROR("REFLECT", "Field '{}' has an unresolved {}; its type was never registered with reflection",
                 m_Name != nullptr ? m_Name : "?", what != nullptr ? what : "?");
}

void* FieldDescriptor::GetAddress(void* object) const
{
    HP_ASSERT(object != nullptr);
    return static_cast<std::byte*>(object) + m_Offset;
}

const void* FieldDescriptor::GetAddress(const void* object) const
{
    HP_ASSERT(object != nullptr);
    return static_cast<const std::byte*>(object) + m_Offset;
}

const FieldDescriptor& TypeDescriptor::GetField(Size index) const
{
    HP_ASSERT_MSG(index < m_FieldCount, "Field index out of range");
    return m_Fields[index];
}

const FieldDescriptor* TypeDescriptor::FindField(std::string_view name) const
{
    for (Size index = 0; index < m_FieldCount; ++index)
    {
        if (m_Fields[index].m_Name != nullptr && name == std::string_view(m_Fields[index].m_Name))
        {
            return &m_Fields[index];
        }
    }
    return nullptr;
}

const EnumEntry& EnumDescriptor::GetEntry(Size index) const
{
    HP_ASSERT_MSG(index < m_EntryCount, "Enum entry index out of range");
    return m_Entries[index];
}

bool EnumDescriptor::FindValueByName(std::string_view name, Int64& outValue) const
{
    for (Size index = 0; index < m_EntryCount; ++index)
    {
        if (m_Entries[index].Name != nullptr && name == std::string_view(m_Entries[index].Name))
        {
            outValue = m_Entries[index].Value;
            return true;
        }
    }
    return false;
}

const char* EnumDescriptor::FindNameByValue(Int64 value) const
{
    for (Size index = 0; index < m_EntryCount; ++index)
    {
        if (m_Entries[index].Value == value)
        {
            return m_Entries[index].Name;
        }
    }
    return nullptr;
}

}  // namespace Hollowpoint::Reflection
