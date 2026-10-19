#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Hollowpoint/Core/Guid.h"
#include "Hollowpoint/ECS/Entity.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/Transform.h"
#include "Hollowpoint/Serialization/JsonValue.h"

namespace Hollowpoint::Scene
{

using Core::Guid;
using Core::Size;
using ECS::Entity;
using ECS::World;
using Math::Transform;
using Serialization::JsonValue;

struct SceneLoadReport;  // struct, matching its definition — clang's -Wmismatched-tags makes a class declaration here
                         // an error

// ---------------------------------------------------------------------------
// Prefab — a reusable, immutable entity sub-scene.
//
// A prefab is exactly "a saved scene document plus a name", deliberately not a parallel data structure. Everything
// that makes scenes work (component factories, the reflection bridge, hierarchy resolution, unknown-component
// tolerance) therefore applies to prefabs for free, and there is one loader to debug rather than two. The alternative
// — a dedicated prefab format — reliably drifts from the scene format within months, and the drift shows up as
// "prefabs support X but levels don't" or vice versa.
//
// Immutability matters: prefabs are shared assets instantiated many times. A mutable shared document would mean an
// instance could edit its template, so the tenth spawn differs from the first — a bug that only appears after minutes
// of runtime and never in a smoke test. The document is stored by value here and never handed out non-const.
// ---------------------------------------------------------------------------

class Prefab
{
public:
    //! An invalid prefab: IsLoaded() is false and instantiating it reports an error rather than asserting, because
    //! loading a missing or corrupt asset at runtime is a normal condition (a level referencing content that was
    //! deleted), not a programming error.
    Prefab() = default;

    //! Parses `.hpscene` text into a prefab. On failure the returned prefab is invalid and `outError` explains why —
    //! including the JSON position, which is what makes a hand-edited asset fixable.
    static Prefab FromSceneText(std::string_view text, std::string& outError);

    //! Takes an already-parsed scene document. The document is copied, so callers keep ownership and may cache or edit
    //! their own copy without affecting this prefab.
    static Prefab FromSceneDocument(const JsonValue& document, std::string& outError);

    [[nodiscard]] bool IsLoaded() const noexcept
    {
        return m_HasDocument;
    }

    [[nodiscard]] const std::string& GetName() const noexcept
    {
        return m_Name;
    }

    void SetName(std::string name)
    {
        m_Name = std::move(name);
    }

    //! The shared, read-only scene document. Exposed for tooling (the editor draws a prefab's component list without
    //! instantiating it); there is deliberately no mutable accessor.
    [[nodiscard]] const JsonValue& GetDocument() const noexcept
    {
        return m_Document;
    }

    //! Entity count in the document. Computed once at load so an asset browser can show it per prefab without reparsing
    //! on every frame it is displayed.
    [[nodiscard]] Size GetEntityCount() const noexcept
    {
        return m_EntityCount;
    }

    //! Root entity count — how many separate trees this prefab contributes when instantiated. A single-root prefab
    //! (the common case) has exactly one, which is what the instance attaches to a parent.
    [[nodiscard]] Size GetRootCount() const noexcept
    {
        return m_RootCount;
    }

private:
    bool m_HasDocument = false;
    std::string m_Name;
    JsonValue m_Document;
    Size m_EntityCount = 0;
    Size m_RootCount = 0;
};

// ---------------------------------------------------------------------------
// Instantiation.
// ---------------------------------------------------------------------------

struct InstantiateOptions
{
    //! Entity the prefab's roots are attached under. When null the instance is placed at world level, which is correct
    //! for streaming a sub-level in but almost never what a gameplay spawn wants — an entity spawned relative to a
    //! moving character would stay where it was spawned.
    Entity Parent;

    //! Transform applied to each instantiated root, composed against `Parent` (so it is relative to the parent, like
    //! any LocalTransform). This is what makes "spawn at this position/rotation" work without every caller writing the
    //! roots' transforms by hand afterwards.
    std::optional<Transform> RootTransform;

    //! Give every instantiated entity a fresh Guid. Defaults to true: instantiating one asset twice must not produce
    //! two entities claiming one identity, since Guid-based references would then resolve ambiguously. Set false only
    //! when re-creating a saved world where the identities must match exactly (a deterministic replay, or a test that
    //! compares against a golden file).
    bool AssignFreshIdentities = true;
};

//! What came out of an instantiation. A thin wrapper over SceneLoadReport with the fields that matter renamed to the
//! instance's vocabulary — callers should not have to know that a prefab is loaded through the scene loader.
struct InstantiateResult
{
    bool Created = false;

    //! Every entity created for this instance, in document order.
    std::vector<Entity> Entities;

    //! The instance's root entities: what to attach to a parent, and what to move when the whole instance is
    //! repositioned.
    std::vector<Entity> Roots;

    std::vector<std::string> Warnings;

    [[nodiscard]] bool HasWarnings() const noexcept
    {
        return !Warnings.empty();
    }
};

//! Creates a copy of `prefab` inside `world`.
//!
//! Idempotency note: each call is independent and creates new entities — there is no instance cache, because two
//! spawns of the same prefab are expected to be separate entities. Deduplication belongs to whoever owns the spawn
//! semantics (a level streaming layer), not here.
InstantiateResult InstantiatePrefab(World& world, const Prefab& prefab, const InstantiateOptions& options = {});

}  // namespace Hollowpoint::Scene
