#include <algorithm>
#include <cstdio>
#include <type_traits>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/PropertyView.h"
#include "Hollowpoint/Reflection/Descriptors.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Reflection/ScalarAccess.h"
#include "Hollowpoint/Scene/LocalTransform.h"

// ---------------------------------------------------------------------------
// See the header. The one design decision worth restating where the code lives: this file never reads a component's
// fields directly. It walks TypeDescriptor/FieldDescriptor and reads through Reflection::Read*/Write*, so every
// component type in the engine — plus anything a plugin or future script binding registers — is editable without a
// single line here changing. The exceptions are deliberate too, listed under "leaf kinds" below.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Int64;
using Reflection::ArrayOps;
using Reflection::EnumDescriptor;
using Reflection::FieldDescriptor;
using Reflection::Registry;
using Reflection::ScalarValue;
using Reflection::TypeDescriptor;
using Reflection::ValueKind;

namespace
{

constexpr Size kMaxStructDepth = 8;

[[nodiscard]] const TypeDescriptor* ResolveType(std::string_view typeName)
{
    if (typeName.empty())
    {
        return nullptr;
    }
    return Registry::Get().FindTypeByTypeInfoName(typeName);
}

}  // namespace

void PropertyView::Build(World& world, const Entity& entity)
{
    m_Fields.clear();
    m_Entity = Entity{};

    if (entity.IsNull() || !world.IsAlive(entity))
    {
        return;
    }

    m_Entity = entity;
    m_StructureVersion = world.GetStructureVersion();

    // Iterating storages rather than a known component list is what makes this generic. FindAddress gives the live
    // component; its type name is matched back to a reflected descriptor so fields can be enumerated.
    const Core::Index slot = entity.Slot;

    world.ForEachComponentStorage([&](const ECS::IComponentStorage& storage) {
        if (!storage.Contains(slot))
        {
            return;
        }

        void* componentAddress = const_cast<void*>(storage.FindAddress(slot));
        if (componentAddress == nullptr)
        {
            return;
        }

        // The storage's type name is the scene-file name ("MeshRenderer"); the reflected descriptor is registered
        // under the C++ type's mangled name for engine types, so both are tried. FindType first because a component
        // registered with an explicit readable name (the editor's own EntityName) resolves that way and its
        // registration name is what should head the group in the panel.
        const std::string_view storageName = storage.GetTypeName() != nullptr ? storage.GetTypeName() : "";
        const TypeDescriptor* descriptor = Registry::Get().FindType(storageName);
        if (descriptor == nullptr)
        {
            descriptor = ResolveType(storageName);
        }

        if (descriptor == nullptr)
        {
            // Not reflected: the panel cannot show fields it does not have. Reported once per build rather than per
            // frame — a component without reflection is a registration bug the author wants told about, but saying it
            // sixty times a second makes the log useless for everything else.
            HP_LOG_WARNING("EDITOR", "component \"{}\" has no reflected type; not shown in the details panel",
                           std::string(storageName));
            return;
        }

        AppendFields(componentAddress, descriptor, {}, descriptor->GetName(), 0);
    });
}

void PropertyView::AppendFields(void* componentAddress, const void* structTypeOpaque, std::string_view prefix,
                                std::string_view componentName, Size depth)
{
    const TypeDescriptor* type = static_cast<const TypeDescriptor*>(structTypeOpaque);
    if (type == nullptr || componentAddress == nullptr)
    {
        return;
    }

    if (depth > kMaxStructDepth)
    {
        HP_LOG_WARNING("EDITOR", "property path \"{}\" is deeper than {} levels; stopping", prefix,
                       static_cast<unsigned long long>(kMaxStructDepth));
        return;
    }

    for (Size fieldIndex = 0; fieldIndex < type->GetFieldCount(); ++fieldIndex)
    {
        const FieldDescriptor& field = type->GetField(fieldIndex);
        void* fieldAddress = field.GetAddress(componentAddress);
        if (fieldAddress == nullptr)
        {
            continue;
        }

        std::string path(prefix);
        if (!path.empty())
        {
            path += ".";
        }
        path += field.GetName();

        switch (field.GetKind())
        {
            case ValueKind::Struct:
            {
                const TypeDescriptor* nested = field.GetStructType();
                if (nested == nullptr)
                {
                    HP_LOG_WARNING("EDITOR", "struct field \"{}\" has no resolvable type; skipped", path);
                    break;
                }
                // Expanded, not emitted as a row: "Position: [struct]" that cannot be clicked is a panel that shows
                // data it refuses to edit. Math::Vec3/Quat register their members, so this yields Position.x/.y/.z —
                // which is exactly the UE-style layout authors expect.
                AppendFields(fieldAddress, nested, path, componentName, depth + 1);
                break;
            }

            case ValueKind::Array:
            {
                const ArrayOps& ops = field.GetArrayOps();
                if (ops.Count == nullptr || ops.ElementKind == ValueKind::Struct)
                {
                    // Arrays of structs are listed by count only. Editing a struct element would need the panel to own
                    // insertion/removal semantics (and an undo stack to make them safe), which is not built yet —
                    // showing a read-only count is honest, showing editable rows for something the panel cannot safely
                    // resize is not.
                    PropertyField row;
                    row.ComponentName = std::string(componentName);
                    row.Name = field.GetName();
                    row.Path = path;
                    row.Kind = static_cast<Core::UInt8>(ValueKind::Array);
                    row.Address = nullptr;  // not editable, and IsEditable() reports that
                    m_Fields.push_back(std::move(row));
                    break;
                }

                const Size count = ops.Count(fieldAddress);
                for (Size elementIndex = 0; elementIndex < count; ++elementIndex)
                {
                    const void* elementAddress = nullptr;
                    if (ops.Data != nullptr)
                    {
                        const char* base = static_cast<const char*>(ops.Data(fieldAddress));
                        elementAddress = base + static_cast<std::ptrdiff_t>(elementIndex) * ops.ElementSize;
                    }
                    if (elementAddress == nullptr)
                    {
                        continue;
                    }

                    PropertyField row;
                    row.ComponentName = std::string(componentName);
                    row.Name = std::string(field.GetName()) + "[" + std::to_string(elementIndex) + "]";
                    row.Path = path + "[" + std::to_string(elementIndex) + "]";
                    row.Kind = static_cast<Core::UInt8>(ops.ElementKind);
                    // const_cast: the array element is editable through WriteArrayElementScalar, which takes a void*;
                    // the data pointer above is const because ArrayOps::Data returns const void*.
                    row.Address = const_cast<void*>(elementAddress);
                    m_Fields.push_back(std::move(row));
                }
                break;
            }

            case ValueKind::Enum:
            {
                PropertyField row;
                row.ComponentName = std::string(componentName);
                row.Name = field.GetName();
                row.Path = std::move(path);
                row.Kind = static_cast<Core::UInt8>(ValueKind::Enum);
                row.Address = fieldAddress;
                const EnumDescriptor* enumeration = field.GetEnum();
                row.EnumName = enumeration != nullptr ? enumeration->GetName() : "";
                m_Fields.push_back(std::move(row));
                break;
            }

            case ValueKind::String:
            case ValueKind::Boolean:
            default:
            {
                // Scalars (integers of every width, floats) and strings are leaves. Anything else that reflection can
                // describe but this switch does not name arrives here with its kind preserved; the widget then falls
                // back to FormatValue's text rendering rather than dropping the field, so a new ValueKind shows up as
                // read-only text instead of vanishing from the inspector.
                PropertyField row;
                row.ComponentName = std::string(componentName);
                row.Name = field.GetName();
                row.Path = std::move(path);
                row.Kind = static_cast<Core::UInt8>(field.GetKind());
                row.Address = fieldAddress;
                m_Fields.push_back(std::move(row));
                break;
            }
        }
    }
}

std::vector<PropertyField> PropertyView::GetFieldsForComponent(std::string_view componentName) const
{
    std::vector<PropertyField> matches;
    for (const PropertyField& field : m_Fields)
    {
        if (field.ComponentName == componentName)
        {
            matches.push_back(field);
        }
    }
    return matches;
}

std::vector<std::string> PropertyView::GetComponentNames() const
{
    std::vector<std::string> names;
    for (const PropertyField& field : m_Fields)
    {
        if (std::find(names.begin(), names.end(), field.ComponentName) == names.end())
        {
            names.push_back(field.ComponentName);
        }
    }
    return names;
}

const PropertyField* PropertyView::FindByPath(std::string_view path) const noexcept
{
    for (const PropertyField& field : m_Fields)
    {
        if (field.Path == path)
        {
            return &field;
        }
    }
    return nullptr;
}

namespace
{

//! Locates the descriptor owning a leaf built by AppendFields, so writes use Reflection's clamping rather than a direct
//! store. The alternative — writing through `static_cast<float*>(Address)` — would bypass ClampRealTo and let the
//! inspector store values the loader would refuse, i.e. an edit that changes when the scene is reloaded.
[[nodiscard]] bool LocateLeaf(const Entity& entity, const PropertyField& field,
                              const Reflection::TypeDescriptor** outType, const Reflection::FieldDescriptor** outField)
{
    *outType = nullptr;
    *outField = nullptr;

    const TypeDescriptor* componentType = Registry::Get().FindType(field.ComponentName);
    if (componentType == nullptr)
    {
        return false;
    }

    // Paths are built by walking fields in order, so they are resolved the same way: split on '.', descend through
    // struct fields, and stop at the leaf. Array indices ("Weights[3]") are handled by stripping the bracket before the
    // name lookup — the element kind is what the field descriptor describes, so the index only affects the address,
    // which AppendFields already computed.
    std::string_view remaining = field.Path;
    // Drop the component prefix if present ("MeshRenderer.Roughness" → "Roughness"). Built paths never include it
    // today, but accepting it makes `entity.set MeshRenderer.Roughness` and a panel-generated path interchangeable.
    if (remaining.rfind(field.ComponentName + ".", 0) == 0)
    {
        remaining = remaining.substr(field.ComponentName.size() + 1);
    }

    const TypeDescriptor* current = componentType;
    while (!remaining.empty())
    {
        std::string_view name = remaining;
        const Size bracket = name.find('[');
        if (bracket != std::string_view::npos)
        {
            name = name.substr(0, bracket);
        }

        const Size dot = remaining.find('.', bracket == std::string_view::npos ? 0 : bracket);
        remaining = dot == std::string_view::npos ? std::string_view{} : remaining.substr(dot + 1);

        if (name.empty() || current == nullptr)
        {
            return false;
        }

        const FieldDescriptor* found = current->FindField(name);
        if (found == nullptr)
        {
            return false;
        }

        if (remaining.empty())
        {
            *outType = current;
            *outField = found;
            return true;
        }
        current = found->GetStructType();
    }

    HP_UNUSED(entity);
    return false;
}

}  // namespace

bool PropertyView::ReadReal(const PropertyField& field, double& outValue) const noexcept
{
    if (!field.IsEditable())
    {
        return false;
    }

    ScalarValue value;
    if (Reflection::ReadScalar(field.Address, static_cast<ValueKind>(field.Kind), value))
    {
        outValue = Reflection::Detail::ToDouble(value);
        return true;
    }
    return false;
}

bool PropertyView::WriteReal(const PropertyField& field, double value) noexcept
{
    if (!field.IsEditable())
    {
        return false;
    }
    return Reflection::WriteScalar(field.Address, static_cast<ValueKind>(field.Kind),
                                   ScalarValue::FromReal(static_cast<ValueKind>(field.Kind), value));
}

bool PropertyView::ReadBool(const PropertyField& field, bool& outValue) const noexcept
{
    if (!field.IsEditable())
    {
        return false;
    }
    ScalarValue value;
    if (!Reflection::ReadScalar(field.Address, ValueKind::Boolean, value))
    {
        return false;
    }
    outValue = value.AsBool;
    return true;
}

bool PropertyView::WriteBool(const PropertyField& field, bool value) noexcept
{
    if (!field.IsEditable())
    {
        return false;
    }
    return Reflection::WriteScalar(field.Address, ValueKind::Boolean, ScalarValue::FromBool(value));
}

bool PropertyView::ReadString(const PropertyField& field, std::string& outValue) const noexcept
{
    if (!field.IsEditable())
    {
        return false;
    }
    std::string_view text;
    if (!Reflection::ReadString(field.Address, text))
    {
        return false;
    }
    outValue.assign(text);
    return true;
}

bool PropertyView::WriteString(const PropertyField& field, std::string_view value) noexcept
{
    if (!field.IsEditable())
    {
        return false;
    }
    return Reflection::WriteString(field.Address, value);
}

bool PropertyView::ReadEnum(const PropertyField& field, Int64& outValue) const noexcept
{
    // Enums need the owning field descriptor (its ReadEnumAt knows the underlying storage width), so they are resolved
    // through the path rather than read from the raw address — guessing int32 would misread an `enum : UInt8`.
    if (!field.IsEditable())
    {
        return false;
    }

    const TypeDescriptor* owner = nullptr;
    const FieldDescriptor* fieldDescriptor = nullptr;
    if (!LocateLeaf(m_Entity, field, &owner, &fieldDescriptor) || fieldDescriptor == nullptr)
    {
        return false;
    }
    return fieldDescriptor->ReadEnumAt(field.Address, outValue);
}

bool PropertyView::WriteEnum(const PropertyField& field, Int64 value) noexcept
{
    if (!field.IsEditable())
    {
        return false;
    }

    const TypeDescriptor* owner = nullptr;
    const FieldDescriptor* fieldDescriptor = nullptr;
    if (!LocateLeaf(m_Entity, field, &owner, &fieldDescriptor) || fieldDescriptor == nullptr)
    {
        return false;
    }
    return fieldDescriptor->WriteEnumAt(field.Address, value);
}

std::string PropertyView::FormatValue(const PropertyField& field) const
{
    const ValueKind kind = static_cast<ValueKind>(field.Kind);

    if (!field.IsEditable())
    {
        // Read-only rows (array-of-struct placeholders) still say what they hold, because a panel that shows an empty
        // value next to a field name reads as "the value is empty" rather than "this value is not editable yet".
        return "(not editable)";
    }

    if (kind == ValueKind::String)
    {
        std::string text;
        return ReadString(field, text) ? text : std::string("(unreadable)");
    }

    if (kind == ValueKind::Boolean)
    {
        bool value = false;
        return (ReadBool(field, value) && value) ? "true" : "false";
    }

    if (kind == ValueKind::Enum)
    {
        Int64 raw = 0;
        if (!ReadEnum(field, raw))
        {
            return "(unreadable)";
        }

        // Show the enumerator name when the registry knows it. A number is technically correct and useless: an author
        // choosing between Culling modes cannot map 2 back to "None" without reading the header.
        // FindNameByValue rather than a scan: the descriptor already indexes by value, and hand-writing the loop here
        // would be a second copy of a lookup that has to agree with the serialiser's.
        const EnumDescriptor* enumeration = Registry::Get().FindEnum(field.EnumName);
        if (enumeration != nullptr)
        {
            if (const char* name = enumeration->FindNameByValue(raw); name != nullptr)
            {
                return std::string(name);
            }
        }
        return std::to_string(raw);
    }

    double value = 0.0;
    if (!ReadReal(field, value))
    {
        return "(unreadable)";
    }

    // Integers print without a decimal point (a "1.0000" for an int count is the sort of detail that makes a panel look
    // broken); reals get six significant digits, which round-trips a float32 exactly — fewer and an edited value
    // changes when the scene is saved and reopened, more and it shows noise the author cannot set.
    if (Reflection::ScalarValue::IsIntegerKind(kind))
    {
        return std::to_string(static_cast<Int64>(value));
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

}  // namespace Hollowpoint::Editor
