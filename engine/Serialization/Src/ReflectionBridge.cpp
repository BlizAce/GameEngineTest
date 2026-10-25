#include <cstddef>
#include <string>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Reflection/Descriptors.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Reflection/ScalarAccess.h"
#include "Hollowpoint/Reflection/TypeTraits.h"
#include "Hollowpoint/Serialization/ReflectionBridge.h"

namespace Hollowpoint::Serialization
{

using Reflection::ArrayOps;
using Reflection::EnumDescriptor;
using Reflection::FieldDescriptor;
using Reflection::FieldFlag_DontSerialize;
using Reflection::FieldFlag_ReadOnly;
using Reflection::ScalarValue;
using Reflection::ValueKind;

// ---------------------------------------------------------------------------
// Addressing convention — read this before editing.
//
// Every function here takes the address of the **containing object** and a field, never a pre-computed field
// address. That matches the Reflection API exactly: ReadFieldValue, ReadStringField, GetArrayElementAddress,
// ResizeArrayField and friends all apply `field.GetAddress(object)` internally. Passing an already-offset address
// therefore offsets twice — which is not a wrong value but an out-of-bounds read (it segfaulted on the first string
// field when this file was written). If you are tempted to "optimise" by hoisting GetAddress, don't: either change
// every call in a function consistently or none of them.
// ---------------------------------------------------------------------------

namespace
{

//! Depth-limited recursion guard. Reflected types cannot be directly self-referential (a member of its own type is
//! incomplete), but deep nesting and arrays-of-structs-of-arrays can still recurse far enough to matter, so the bound
//! exists and is reported rather than silently truncating.
class DepthGuard
{
public:
    explicit DepthGuard(Size maxDepth) noexcept : m_MaxDepth(maxDepth) {}

    [[nodiscard]] bool Enter() noexcept
    {
        if (m_Depth >= m_MaxDepth)
        {
            return false;
        }
        ++m_Depth;
        return true;
    }

    void Leave() noexcept
    {
        --m_Depth;
    }

private:
    Size m_MaxDepth = 0;
    Size m_Depth = 0;
};

[[nodiscard]] std::string JoinPath(const std::string& path, std::string_view name)
{
    std::string result = path;
    if (!result.empty())
    {
        result += '.';
    }
    result.append(name);
    return result;
}

[[nodiscard]] std::string JoinIndex(const std::string& path, Size index)
{
    return path + "[" + std::to_string(index) + "]";
}

// ---------------------------------------------------------------------------
// Field traversal across an inheritance chain.
//
// TypeDescriptor::GetFieldCount reports *own* fields only (the descriptor for `Derived` deliberately does not list
// `Base`'s members, so counts never double-count and the inspector can group them). Every consumer therefore has to
// walk GetBaseType() itself — which this file initially did not, so a component with a reflected base saved without
// its inherited fields: valid-looking output, silently missing data. Worst class of bug, hence the helper exists once
// rather than at each call site.
//
// Order is base-first, then derived, so a scene file reads "the parts inherited from Base" followed by what the
// specific type adds — which matches how the type is declared and keeps diffs stable when a field moves into a base.
//
// Constraint worth stating: field offsets in a descriptor are relative to the *most derived* object passed in, which
// is correct for single inheritance (offset 0 for the base subobject). Multiple inheritance can place a base at a
// non-zero offset, where the stored offset would be wrong; reflected component hierarchies use single inheritance, and
// nothing in the engine needs more. Enforcing it at runtime would cost a check per field access to guard a case we do
// not have, so it is documented instead.
// ---------------------------------------------------------------------------

//! Visits every serialisable field of `type` including inherited ones, base-first. `onField` receives the field and
//! the offset-from-`object` address to read/write (always object + field.GetAddress(), see the constraint above).
template <typename TCallback>
void ForEachFieldInChain(const TypeDescriptor& type, TCallback&& onField)
{
    // Recursion depth here is the inheritance depth of a component type — a handful at most, and bounded in practice
    // by registration (a base must already be registered), so no explicit stack is warranted.
    if (const TypeDescriptor* base = type.GetBaseType(); base != nullptr)
    {
        ForEachFieldInChain(*base, onField);
    }

    for (Size index = 0; index < type.GetFieldCount(); ++index)
    {
        onField(type.GetField(index));
    }
}

//! True when `name` is a field of `type` or of any base — needed so an inherited field is not reported as an unknown
//! member while loading.
[[nodiscard]] bool ChainHasField(const TypeDescriptor& type, std::string_view name)
{
    for (const TypeDescriptor* current = &type; current != nullptr; current = current->GetBaseType())
    {
        if (current->FindField(name) != nullptr)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Encode.
// ---------------------------------------------------------------------------

JsonValue ValueToJson(const void* object, const FieldDescriptor& field, DepthGuard& depth,
                      ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path);

JsonValue StructToJson(const void* object, const TypeDescriptor& type, DepthGuard& depth,
                       ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path)
{
    JsonObject members;

    ForEachFieldInChain(type, [&](const FieldDescriptor& field) {
        // Runtime-only state never reaches disk. Skipped on both paths, so a file cannot smuggle a stale cached
        // pointer or accumulated timer back into a live component.
        if (field.HasFlag(FieldFlag_DontSerialize))
        {
            return;
        }

        // Honoured on both paths: excluding read-only fields when writing but still loading them when reading would
        // make a save/load pair asymmetric, so a component saved with the option off would still be overwritten by a
        // file that happens to contain the key.
        if (!options.IncludeReadOnlyFields && field.HasFlag(FieldFlag_ReadOnly))
        {
            return;
        }

        const std::string fieldPath = JoinPath(path, field.GetName());
        JsonValue encoded = ValueToJson(object, field, depth, diagnostics, options, fieldPath);

        // A null result means the field could not be encoded and ValueToJson already reported why. Writing the key
        // anyway would put a failure marker where data belongs; dropping it means the field simply keeps its default
        // on load, which is safer and visible in the diagnostics. An *empty* struct still encodes as {} rather than
        // null, so this never discards a legitimately empty nested object.
        if (encoded.IsNull())
        {
            return;
        }

        members.Set(field.GetName(), std::move(encoded));
    });

    return JsonValue(std::move(members));
}

JsonValue ArrayToJson(const void* object, const FieldDescriptor& field, DepthGuard& depth,
                      ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path)
{
    const ArrayOps& ops = field.GetArrayOps();
    JsonArray elements;

    if (ops.Count == nullptr)
    {
        diagnostics.Add(path + ": reflected array has no element-count operation");
        return JsonValue(std::move(elements));
    }

    const void* arrayAddress = field.GetAddress(object);
    const Size count = ops.Count(arrayAddress);

    const TypeDescriptor* elementStruct = field.GetArrayElementType();
    const bool elementsAreStructs = ops.ElementKind == ValueKind::Struct && elementStruct != nullptr;

    // Reserve up front: a mesh's index list or a thousand-item loot table should not reallocate per element.
    elements.reserve(count);

    for (Size elementIndex = 0; elementIndex < count; ++elementIndex)
    {
        const std::string elementPath = JoinIndex(path, elementIndex);

        if (elementsAreStructs)
        {
            // GetArrayElementAddress takes the containing object and resolves the array itself.
            const void* elementAddress = GetArrayElementAddress(object, field, elementIndex);
            if (elementAddress == nullptr)
            {
                diagnostics.Add(elementPath + ": array element address could not be resolved");
                elements.push_back(JsonValue(nullptr));
                continue;
            }

            if (!depth.Enter())
            {
                diagnostics.Add(elementPath + ": nested struct depth exceeded the conversion limit (" +
                                std::to_string(options.MaxDepth) + ")");
                break;
            }
            JsonValue encoded = StructToJson(elementAddress, *elementStruct, depth, diagnostics, options, elementPath);
            depth.Leave();
            elements.push_back(std::move(encoded));
            continue;
        }

        if (ops.ElementKind == ValueKind::String)
        {
            // A vector<std::string> has no scalar element bridge and ArrayOps does not carry string operations, so
            // this shape is unsupported rather than silently empty. Reported once per element with the field path so
            // the author can wrap it in a reflected struct or handle it with a custom hook.
            diagnostics.Add(elementPath + ": arrays of strings are not supported by the reflection bridge");
            elements.push_back(JsonValue(nullptr));
            continue;
        }

        ScalarValue scalar;
        if (!ReadArrayElementScalar(object, field, elementIndex, scalar))
        {
            diagnostics.Add(elementPath + ": array element could not be read");
            elements.push_back(JsonValue(nullptr));
            continue;
        }

        if (scalar.Kind == ValueKind::Boolean)
        {
            elements.push_back(JsonValue(scalar.AsBool));
        }
        else if (ScalarValue::IsIntegerKind(scalar.Kind))
        {
            // Includes enums, which the scalar bridge widens to int64. Element enums have no registered descriptor
            // reachable through ArrayOps, so they stay numeric — unlike a top-level enum field, which is written by
            // name. Documented rather than prettified: inventing names for array elements would need a registry
            // lookup per element kind that ArrayOps deliberately does not carry.
            elements.push_back(JsonValue(scalar.AsInt));
        }
        else
        {
            elements.push_back(JsonValue(scalar.AsReal));
        }
    }

    return JsonValue(std::move(elements));
}

JsonValue ValueToJson(const void* object, const FieldDescriptor& field, DepthGuard& depth,
                      ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path)
{
    switch (field.GetKind())
    {
        case ValueKind::String:
        {
            std::string_view text;
            if (!ReadStringField(object, field, text))
            {
                diagnostics.Add(path + ": string field could not be read");
                return JsonValue(nullptr);
            }
            return JsonValue(std::string(text));
        }

        case ValueKind::Enum:
        {
            const EnumDescriptor* enumeration = field.GetEnum();
            if (enumeration == nullptr)
            {
                diagnostics.Add(path + ": enum field's type was never registered");
                return JsonValue(nullptr);
            }

            Int64 raw = 0;
            if (!field.ReadEnumAt(field.GetAddress(object), raw))
            {
                diagnostics.Add(path + ": enum field could not be read");
                return JsonValue(nullptr);
            }

            // By name, so inserting an enumerator in the middle of a list cannot silently reinterpret saved data —
            // and so a scene reads as "mode": "Directional" rather than "mode": 2.
            if (const char* name = enumeration->FindNameByValue(raw))
            {
                return JsonValue(std::string(name));
            }

            // A value outside the registered set means the component holds something the enum does not describe
            // (uninitialised memory, or enumerators removed by a refactor). Written numerically so no information is
            // lost, and reported because it should not happen.
            diagnostics.Add(std::string(path) + ": enum value " + std::to_string(raw) + " is not part of '" +
                            enumeration->GetName() + "'; written numerically");
            return JsonValue(raw);
        }

        case ValueKind::Struct:
        {
            const TypeDescriptor* structType = field.GetStructType();
            if (structType == nullptr)
            {
                diagnostics.Add(path + ": struct type could not be resolved (never registered?)");
                return JsonValue(nullptr);
            }

            if (!depth.Enter())
            {
                diagnostics.Add(path + ": nested struct depth exceeded the conversion limit (" +
                                std::to_string(options.MaxDepth) + ")");
                return JsonValue(nullptr);
            }
            JsonValue encoded = StructToJson(field.GetAddress(object), *structType, depth, diagnostics, options, path);
            depth.Leave();
            return encoded;
        }

        case ValueKind::Array:
            return ArrayToJson(object, field, depth, diagnostics, options, path);

        default:
        {
            ScalarValue scalar;
            if (!ReadFieldValue(object, field, scalar))
            {
                diagnostics.Add(path + ": field could not be read");
                return JsonValue(nullptr);
            }

            if (scalar.Kind == ValueKind::Boolean)
            {
                return JsonValue(scalar.AsBool);
            }
            if (ScalarValue::IsIntegerKind(scalar.Kind))
            {
                // Integers stay integers on the wire: no "3.0" for a count, and full precision above 2^53.
                return JsonValue(scalar.AsInt);
            }
            return JsonValue(scalar.AsReal);
        }
    }
}

// ---------------------------------------------------------------------------
// Decode.
// ---------------------------------------------------------------------------

bool ValueFromJson(const JsonValue& json, void* object, const FieldDescriptor& field, DepthGuard& depth,
                   ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path);

bool StructFromJson(const JsonValue& json, void* object, const TypeDescriptor& type, DepthGuard& depth,
                    ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path)
{
    if (!json.IsObject())
    {
        diagnostics.Add(path + ": expected an object for '" + type.GetName() + "' but found " +
                        JsonValue::KindName(json.GetKind()));
        return false;
    }

    const JsonObject& source = json.AsObject();
    bool allOk = true;

    // Unknown members are reported and skipped rather than fatal. This is the ordinary signature of a scene saved by
    // a newer build or of a renamed field, and refusing to load a level over one dead field is worse than losing that
    // value with a message naming the type it did not fit.
    for (Size index = 0; index < source.GetCount(); ++index)
    {
        const std::string& key = source.GetKey(index);
        if (!ChainHasField(type, key))
        {
            diagnostics.Add(JoinPath(path, key) + ": '" + type.GetName() + "' has no field named '" + key +
                            "'; ignored");
        }
    }

    // Fields are visited in declaration order and a field absent from the JSON keeps its current value. That is what
    // makes adding a field to a component safe for every existing scene file: no migration, no version bump — the new
    // field simply gets the same default a freshly constructed component would have.
    ForEachFieldInChain(type, [&](const FieldDescriptor& field) {
        if (field.HasFlag(FieldFlag_DontSerialize))
        {
            return;
        }
        if (!options.IncludeReadOnlyFields && field.HasFlag(FieldFlag_ReadOnly))
        {
            return;
        }

        // Deprecated needs no case here: that flag means "hidden from the inspector", not "not on disk" — old scenes
        // still carry the value and loading it is the whole point of keeping the field.

        const JsonValue* member = source.Find(field.GetName());
        if (member == nullptr)
        {
            return;  // Absent from the file: keep the current/default value.
        }

        const std::string fieldPath = JoinPath(path, field.GetName());
        if (!ValueFromJson(*member, object, field, depth, diagnostics, options, fieldPath))
        {
            allOk = false;
        }
    });

    return allOk;
}

bool ArrayFromJson(const JsonValue& json, void* object, const FieldDescriptor& field, DepthGuard& depth,
                   ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path)
{
    if (!json.IsArray())
    {
        diagnostics.Add(path + ": expected an array but found " + JsonValue::KindName(json.GetKind()));
        return false;
    }

    const ArrayOps& ops = field.GetArrayOps();
    const JsonArray& elements = json.AsArray();

    // Resize once to the exact length, then write elements in place through the returned base pointer. Growing per
    // element instead would be O(n^2) on reallocation for large lists and buys nothing.
    void* base = ResizeArrayField(object, field, elements.size());

    if (elements.empty())
    {
        // Clearing an array legitimately has no element storage to return.
        return true;
    }

    if (base == nullptr)
    {
        diagnostics.Add(path + ": array resize returned no storage");
        return false;
    }

    const TypeDescriptor* elementStruct = field.GetArrayElementType();
    const bool elementsAreStructs = ops.ElementKind == ValueKind::Struct && elementStruct != nullptr;

    if (elementsAreStructs)
    {
        if (ops.ElementSize == 0)
        {
            diagnostics.Add(path + ": struct array has no element stride");
            return false;
        }

        bool allOk = true;
        for (Size elementIndex = 0; elementIndex < elements.size(); ++elementIndex)
        {
            const std::string elementPath = JoinIndex(path, elementIndex);

            if (!depth.Enter())
            {
                diagnostics.Add(elementPath + ": nested struct depth exceeded the conversion limit (" +
                                std::to_string(options.MaxDepth) + ")");
                return false;
            }
            const bool ok =
                StructFromJson(elements[elementIndex], static_cast<std::byte*>(base) + elementIndex * ops.ElementSize,
                               *elementStruct, depth, diagnostics, options, elementPath);
            depth.Leave();
            allOk = allOk && ok;
        }
        return allOk;
    }

    if (ops.ElementKind == ValueKind::String)
    {
        diagnostics.Add(path + ": arrays of strings are not supported by the reflection bridge");
        return false;
    }

    bool allOk = true;

    for (Size elementIndex = 0; elementIndex < elements.size(); ++elementIndex)
    {
        const std::string elementPath = JoinIndex(path, elementIndex);
        const JsonValue& element = elements[elementIndex];

        if (!element.IsNumber() && !(ops.ElementKind == ValueKind::Boolean && element.IsBoolean()))
        {
            diagnostics.Add(elementPath + ": expected a " +
                            (ops.ElementKind == ValueKind::Boolean ? "boolean" : "number") + " but found " +
                            JsonValue::KindName(element.GetKind()));
            allOk = false;
            continue;
        }

        ScalarValue scalar;
        if (ops.ElementKind == ValueKind::Boolean)
        {
            scalar = ScalarValue::FromBool(element.AsBool());
        }
        else if (ScalarValue::IsIntegerKind(ops.ElementKind) || ops.ElementKind == ValueKind::Enum)
        {
            scalar = ScalarValue::FromSigned(ops.ElementKind, element.AsInt());
        }
        else
        {
            scalar = ScalarValue::FromReal(ops.ElementKind, element.AsDouble());
        }

        if (!WriteArrayElementScalar(object, field, elementIndex, scalar))
        {
            diagnostics.Add(elementPath + ": element could not be written");
            allOk = false;
        }
    }

    return allOk;
}

bool ValueFromJson(const JsonValue& json, void* object, const FieldDescriptor& field, DepthGuard& depth,
                   ConversionDiagnostics& diagnostics, const ConversionOptions& options, const std::string& path)
{
    switch (field.GetKind())
    {
        case ValueKind::String:
        {
            if (!json.IsString())
            {
                diagnostics.Add(path + ": expected a string but found " + JsonValue::KindName(json.GetKind()));
                return false;
            }
            return WriteStringField(object, field, json.AsString());
        }

        case ValueKind::Enum:
        {
            const EnumDescriptor* enumeration = field.GetEnum();
            if (enumeration == nullptr)
            {
                diagnostics.Add(path + ": enum field's type was never registered");
                return false;
            }

            Int64 raw = 0;
            if (json.IsString())
            {
                // Names are the canonical form. An unrecognised name is an error rather than a silent fallback to
                // zero: "Direcional" quietly becoming Point is a bug nobody finds.
                if (!enumeration->FindValueByName(json.AsString(), raw))
                {
                    diagnostics.Add(path + ": '" + json.AsString() + "' is not a value of '" + enumeration->GetName() +
                                    "'");
                    return false;
                }
            }
            else if (json.IsNumber())
            {
                // Numeric form accepted for hand-written files and values written by an older build.
                raw = json.AsInt();
            }
            else
            {
                diagnostics.Add(path + ": expected an enum name or number but found " +
                                JsonValue::KindName(json.GetKind()));
                return false;
            }

            if (!field.WriteEnumAt(field.GetAddress(object), raw))
            {
                diagnostics.Add(path + ": enum field could not be written");
                return false;
            }
            return true;
        }

        case ValueKind::Struct:
        {
            const TypeDescriptor* structType = field.GetStructType();
            if (structType == nullptr)
            {
                diagnostics.Add(path + ": struct type could not be resolved (never registered?)");
                return false;
            }
            if (!depth.Enter())
            {
                diagnostics.Add(path + ": nested struct depth exceeded the conversion limit (" +
                                std::to_string(options.MaxDepth) + ")");
                return false;
            }
            const bool ok =
                StructFromJson(json, field.GetAddress(object), *structType, depth, diagnostics, options, path);
            depth.Leave();
            return ok;
        }

        case ValueKind::Array:
            return ArrayFromJson(json, object, field, depth, diagnostics, options, path);

        case ValueKind::Boolean:
        {
            if (!json.IsBoolean())
            {
                diagnostics.Add(path + ": expected a boolean but found " + JsonValue::KindName(json.GetKind()));
                return false;
            }
            return WriteFieldValue(object, field, ScalarValue::FromBool(json.AsBool()));
        }

        default:
        {
            // Numeric kinds. Clamping happens inside WriteFieldValue with the same rules the inspector uses, so a
            // value loaded from disk and one dragged into a slider cannot store different bytes.
            if (!json.IsNumber())
            {
                diagnostics.Add(path + ": expected a number but found " + JsonValue::KindName(json.GetKind()));
                return false;
            }

            const bool wantsInteger = ScalarValue::IsIntegerKind(field.GetKind());
            const ScalarValue scalar = wantsInteger ? ScalarValue::FromSigned(field.GetKind(), json.AsInt())
                                                    : ScalarValue::FromReal(field.GetKind(), json.AsDouble());
            return WriteFieldValue(object, field, scalar);
        }
    }
}

}  // namespace

void ConversionDiagnostics::ReportToLog(const char* context) const
{
    if (Messages.empty())
    {
        return;
    }

    // One line per problem plus a summary: the first message is usually enough to start fixing, and the count tells
    // the author whether this is one typo or a format mismatch across the whole file.
    for (const std::string& message : Messages)
    {
        HP_LOG_ERROR("Serialization", "{}: {}", context != nullptr ? context : "conversion", message);
    }

    if (Messages.size() > 1)
    {
        HP_LOG_ERROR("Serialization", "{}: {} problem(s) total", context != nullptr ? context : "conversion",
                     Messages.size());
    }
}

JsonValue TypeToJson(const void* object, const TypeDescriptor& type, ConversionDiagnostics& diagnostics,
                     const ConversionOptions& options)
{
    if (object == nullptr)
    {
        diagnostics.Add(std::string(type.GetName()) + ": cannot serialise a null object");
        return JsonValue(nullptr);
    }

    DepthGuard depth(options.MaxDepth);
    if (!depth.Enter())
    {
        diagnostics.Add(std::string(type.GetName()) + ": conversion depth limit reached immediately");
        return JsonValue(nullptr);
    }

    JsonValue result = StructToJson(object, type, depth, diagnostics, options, type.GetName());
    depth.Leave();
    return result;
}

bool JsonToType(const JsonValue& json, void* object, const TypeDescriptor& type, ConversionDiagnostics& diagnostics,
                const ConversionOptions& options)
{
    if (object == nullptr)
    {
        diagnostics.Add(std::string(type.GetName()) + ": cannot deserialise into a null object");
        return false;
    }

    DepthGuard depth(options.MaxDepth);
    if (!depth.Enter())
    {
        diagnostics.Add(std::string(type.GetName()) + ": conversion depth limit reached immediately");
        return false;
    }

    const bool ok = StructFromJson(json, object, type, depth, diagnostics, options, type.GetName());
    depth.Leave();
    return ok;
}

}  // namespace Hollowpoint::Serialization
