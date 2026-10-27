#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/Entity.h"
#include "Hollowpoint/ECS/World.h"

// ---------------------------------------------------------------------------
// PropertyView — the data model behind the details panel.
//
// Everything the panel shows comes from the reflection registry rather than a hand-written branch per component type.
// That is not laziness: an inspector that lists whatever a type registers means a new component (or a plugin's, or a
// script-defined one) is editable the moment it is registered, with no editor change — and the alternative in a
// UE-style editor is a growing switch statement that silently omits whichever field nobody remembered. The registry
// already carries names, kinds, offsets and array operations for the scene serialiser; the inspector is the same data
// read by a different consumer, which is why this file is short relative to what it does.
//
// Split into a model (this type) and drawing (DrawPropertyPanel in EditorUi.cpp), so editing rules are testable without
// ImGui: "writing a Float32 field stores the value", "an out-of-range integer clamps the way the loader clamps",
// "a struct field expands into its members". Those are the behaviours an inspector can get wrong invisibly, and each is
// asserted in tests/Editor/TestPropertyView.cpp.
//
// Clamping note: writes go through Reflection::WriteFieldValue, i.e. the *same* function the scene loader uses.
// Deliberate — an editor that rounds differently from the loader produces a file that changes when reopened, which is
// the sort of bug that survives every test because it only appears across a save/load round trip.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Index;
using Core::Size;
using ECS::Entity;
using ECS::World;

//! One editable leaf in the panel: a scalar, a string, or one element of an array/struct path.
struct PropertyField
{
    //! Component type name ("MeshRenderer"), for the group heading.
    std::string ComponentName;

    //! Field name as registered ("Roughness"). For struct members this is the leaf name; see Path for display.
    std::string Name;

    //! Dotted path from the component ("Position.x"), which is what the panel shows and what a console command can
    //  target: `entity.set MeshRenderer.Roughness 0.2` needs one unambiguous string per editable value.
    std::string Path;

    //! Reflection kind of the leaf (Boolean/Float32/Int32/String/Enum). Struct and Array kinds are expanded into their
    //  leaves rather than emitted as one row, so every PropertyField is directly editable by a widget.
    Core::UInt8 Kind = 0;

    //! Pointer to the leaf value. Valid until the world changes structurally (a component added or removed, an entity
    //  destroyed) — the panel rebuilds its list whenever the selection changes or the world's structure version moves,
    //  which is exactly the condition that invalidates these. Held raw and non-owning by design: the ECS owns component
    //  memory and hands out stable addresses between structural changes.
    void* Address = nullptr;

    //! For enums: the registered enum name, so the widget can offer names rather than integers. Empty otherwise.
    std::string EnumName;

    [[nodiscard]] bool IsEditable() const noexcept
    {
        return Address != nullptr;
    }
};

//! Builds and edits the property list for one entity.
class HP_EDITOR_API PropertyView
{
public:
    //! Rebuilds the field list for `entity` against `world`. Empty when the entity is dead or has no components — an
    //  entity with only an EntityName component shows that one string, which is the correct answer, not an empty panel.
    void Build(World& world, const Entity& entity);

    [[nodiscard]] const std::vector<PropertyField>& GetFields() const noexcept
    {
        return m_Fields;
    }

    [[nodiscard]] Size GetFieldCount() const noexcept
    {
        return m_Fields.size();
    }

    //! Fields belonging to one component type, in build order — the panel draws a collapsible group per component.
    [[nodiscard]] std::vector<PropertyField> GetFieldsForComponent(std::string_view componentName) const;

    //! Component type names present on the entity, in registration/storage order and deduplicated.
    [[nodiscard]] std::vector<std::string> GetComponentNames() const;

    [[nodiscard]] const PropertyField* FindByPath(std::string_view path) const noexcept;

    // --- typed reads/writes (the widget calls these; tests call them directly) ---------

    [[nodiscard]] bool ReadReal(const PropertyField& field, double& outValue) const noexcept;
    [[nodiscard]] bool WriteReal(const PropertyField& field, double value) noexcept;

    [[nodiscard]] bool ReadBool(const PropertyField& field, bool& outValue) const noexcept;
    [[nodiscard]] bool WriteBool(const PropertyField& field, bool value) noexcept;

    [[nodiscard]] bool ReadString(const PropertyField& field, std::string& outValue) const noexcept;
    [[nodiscard]] bool WriteString(const PropertyField& field, std::string_view value) noexcept;

    [[nodiscard]] bool ReadEnum(const PropertyField& field, Core::Int64& outValue) const noexcept;
    [[nodiscard]] bool WriteEnum(const PropertyField& field, Core::Int64 value) noexcept;

    //! Formats a field's current value for display. Used by the panel and by `entity.inspect`, so both show exactly the
    //  same text — a console that prints more precision than the widget hides discrepancies instead of finding them.
    [[nodiscard]] std::string FormatValue(const PropertyField& field) const;

private:
    //! Recursively expands struct/array fields into leaves. Depth-capped: reflection cannot have cycles today (a
    //  component may not contain itself), but a hand-built descriptor could, and an inspector that recurses forever is
    //  a hang in the tool rather than in the game — where the user's work is unsaved.
    void AppendFields(void* componentAddress, const void* structTypeOpaque, std::string_view prefix,
                      std::string_view componentName, Size depth);

    Entity m_Entity;  //!< What the list describes; a null handle means "nothing built".
    Size m_StructureVersion = 0;
    std::vector<PropertyField> m_Fields;
};

}  // namespace Hollowpoint::Editor
