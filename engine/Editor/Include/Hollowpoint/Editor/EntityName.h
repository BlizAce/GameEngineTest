#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"

// ---------------------------------------------------------------------------
// EntityName — the display name an author gives an entity.
//
// Why this exists as a component rather than a field on the entity: the ECS stores identity (slot+generation) and
// GUIDs, deliberately not names — a name is authoring metadata, and putting it in the world would mean every
// iteration, every archetype query and every serialized entity carries a string the simulation never reads. As a
// component it costs nothing for entities that have none, which scripted spawners and engine-internal entities
// mostly don't.
//
// The editor is what makes it load-bearing: an Outliner listing "Entity 41, Entity 42, Entity 43" is useless the
// moment a scene has more than a handful of things in it, and UE's hierarchy works precisely because every actor
// has a name. When absent, consumers fall back to a stable derived label ("MeshRenderer (slot 41)") rather than
// showing the raw slot — the fallback lives in the editor, not here, because "what should this be called" is a
// presentation decision and this type's job is only to store what the author typed.
//
// Registered by RegisterEditorTypes() rather than RegisterGameTypes(): Game must not know the editor exists
// (layering), and a runtime that never shows names shouldn't pay for serialising one. A scene saved with names and
// loaded by a runtime without them reports the usual "unknown component type" warning and loses only the label —
// entities keep their GUIDs, so nothing else is affected.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

//! A named entity. Empty Name means "no author-set name", which is distinct from the derived fallback an Outliner
//! shows; clearing a name in the UI sets this back to empty rather than inventing one.
struct EntityName
{
    std::string Name;

    EntityName() = default;
    explicit EntityName(std::string name) : Name(std::move(name)) {}

    bool operator==(const EntityName&) const = default;
};

//! Registers the editor's scene types (EntityName, plus Scene/Game types it depends on). Idempotent; call before
//! loading a scene that may contain them. Safe to call from several entry points — see RegisterEditorTypes.cpp.
void RegisterEditorTypes();

//! True once RegisterEditorTypes has completed and its registrations are still present.
[[nodiscard]] bool AreEditorTypesRegistered();

}  // namespace Hollowpoint::Editor
