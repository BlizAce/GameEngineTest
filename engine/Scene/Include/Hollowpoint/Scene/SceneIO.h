#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Core/Guid.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Reflection/Descriptors.h"
#include "Hollowpoint/Serialization/JsonValue.h"
#include "Hollowpoint/Serialization/ReflectionBridge.h"

namespace Hollowpoint::Scene
{

using Core::Guid;
using Core::Size;
using ECS::Entity;
using ECS::World;
using Reflection::TypeDescriptor;
using Serialization::ConversionDiagnostics;
using Serialization::JsonValue;

// ---------------------------------------------------------------------------
// ComponentFactory — maps a scene-file component name to code that can create one.
//
// Why this exists: loading knows only the *name* written in the file ("MeshRenderer"). The ECS creates components
// through the templated AddComponent<T>, so something must bridge name -> concrete type without the world knowing any
// component types (that open-type property is what lets plugins add components at all). This is that bridge, and it
// lives here rather than in the ECS so the ECS stays free of reflection and serialisation concerns.
//
// Registration sits next to the type's HP_REGISTER_TYPE block, so a component gains saving by declaring itself in two
// adjacent lines. Forgetting the factory registration is caught at load time with a message naming the component, not
// by silently dropping it — a scene that loses its renderers because of a missing macro line is a bad afternoon.
// ---------------------------------------------------------------------------

class ComponentFactory
{
public:
    static ComponentFactory& Get();

    //! Registers the factory for a reflected component type. `typeName` must match the Reflection registration name,
    //! because that is what appears in scene files and what the loader looks up.
    //!
    //! Defined inline over the non-template RegisterFactory: this is a template, so a definition in the .cpp would not
    //! be visible to any other translation unit (link error "used but never defined" for every caller outside SceneIO).
    //! Keeping the type-erased part — the map, the ordering rules — out of line means the header exposes one small
    //! function and no container.
    template <typename TComponent>
    void Register(std::string_view typeName)
    {
        RegisterFactory(
            typeName,
            [](World& world, Entity entity) -> void* {
                if (!world.IsAlive(entity))
                {
                    return nullptr;
                }
                // AddComponent default-constructs in place and returns a reference; the bridge then fills its fields.
                // A component that already exists (a prefab overriding a value set by code) is returned untouched,
                // which is what instantiation wants: later layers patch, they do not reset.
                return static_cast<void*>(&world.AddComponent<TComponent>(entity));
            },
            typeName);
    }

    //! Type-erased registration used by Register<T> and by anything that cannot name the type at compile time (a
    //! plugin boundary, a scripting binding). `reflectionTypeName` is what FindType must resolve, which for engine
    //! types differs from the scene-file name only when a prefix is in play ("Math.Vec3").
    //!
    //! Re-registering an existing name keeps the first factory and reports it: two components claiming one scene-file
    //! name would otherwise make loads depend on registration order, which is a bug with no good error message.
    void RegisterFactory(std::string_view typeName, std::function<void*(World&, Entity)> create,
                         std::string_view reflectionTypeName);

    //! Removes every registration. Used by tests that need a known-clean registry; production code registers once at
    //! startup and never unregisters (a scene arriving later must still load).
    void Clear() noexcept
    {
        m_Factories.clear();
    }

    [[nodiscard]] bool IsRegistered(std::string_view typeName) const;

    //! Names of every registered component type, sorted. Used by the editor's "add component" list and by validation
    //! tooling that checks a scene against what the build understands.
    [[nodiscard]] std::vector<std::string> GetRegisteredNames() const;

    //! Creates (or fetches) the component on `entity` and returns its address plus reflected type, or nullptr when the
    //! name is unknown or its reflected type cannot be found.
    [[nodiscard]] void* CreateOrGet(const World& world, Entity entity, std::string_view typeName,
                                    const TypeDescriptor*& outType) const;

private:
    //! Callback plus the registry name its descriptor is looked up under.
    struct Entry
    {
        std::function<void*(World&, Entity)> Create;
        std::string ReflectionTypeName;
    };

    void AddEntry(std::string_view typeName, Entry entry);

    // Insertion order does not matter here: scene files list components in their own order, and the editor list sorts.
    std::unordered_map<std::string, Entry> m_Factories;
};

// ---------------------------------------------------------------------------
// Scene document — the in-memory form of a `.hpscene` file.
//
// Format (version 1), written by WriteScene:
//
//   {
//     "formatVersion": 1,
//     "name": "Level01",
//     "entities": [
//       { "id": 0, "guid": "...", "name": "DirectionalLight",
//         "parent": 2,                       // file-local id of the parent, omitted for roots
//         "components": { "LocalTransform": {...}, "Light": {...} } }
//     ]
//   }
//
// Two identity schemes, deliberately: `id` is a dense file-local index used for structure (parent links), and `guid`
// is the persistent identity used for cross-file references. Using only GUIDs for parents would make every hand-edit
// of a large scene require pasting UUIDs; using only indices would make references break when entities are reordered
// between files. Both, with the local index confined to one document, gets readable files and stable identity.
//
// Loading is two-phase on purpose: every entity is created first, then parents are assigned. Otherwise a child
// appearing before its parent in the file could not link, which would force authoring tools and hand-editors to
// guarantee a topological order for no benefit.
// ---------------------------------------------------------------------------

//! Errors and warnings from a load or save, collected rather than thrown: a scene with problems should still load as
//! much of itself as possible, with every problem named.
struct SceneLoadReport
{
    struct Issue
    {
        Core::UInt32 EntityIndex = 0;  //!< Index into the file's entity array (not an ECS slot).
        std::string Message;
    };

    //! True when the scene was created and is usable. Errors here mean parts were dropped.
    bool Loaded = false;

    //! Every entity this load created, in document order. Populated only on success, so a caller can act on exactly
    //! what arrived (a prefab instance selecting its new entities, a streaming layer tracking a level's worth of
    //! entities) without diffing the world before and after — which would race anything else creating entities.
    std::vector<Entity> CreatedEntities;

    //! The subset of CreatedEntities that had no parent in the document. For a prefab this is what an instance attaches
    //! to a scene node and repositions, so it has to be known rather than inferred by querying parents afterwards
    //! (which would also report entities attached to a parent *by this load* as roots).
    std::vector<Entity> RootEntities;

    std::vector<Issue> Errors;
    std::vector<Issue> Warnings;

    [[nodiscard]] bool HasErrors() const noexcept
    {
        return !Errors.empty();
    }

    void ReportToLog(const char* sourceName) const;
};

struct SceneSaveOptions
{
    //! Display name written to the document. Empty (the default) omits the key entirely rather than writing a
    //! placeholder: a scene whose file says "UntitledScene" because nothing set it is worse than one with no name,
    //! since the empty value looks like data and will be trusted by anything reading it. The World does not currently
    //! store a name, so callers that have one (the editor, the asset database) pass it here.
    std::string SceneName;

    Serialization::ConversionOptions Conversion;
};

//! Registers the Scene module's serialisable component types (LocalTransform plus the math types it is built from).
//!
//! Explicit and idempotent rather than a static initialiser on purpose: this lives in a static library, and a
//! translation unit whose symbols are unreferenced is not linked in at all — its registrations would silently never
//! run, so scenes would save without transforms. WriteScene and LoadScene both call it, so ordinary use cannot forget;
//! tools that inspect the registry directly can call it first.
void RegisterSceneTypes();

//! True once RegisterSceneTypes has completed.
bool AreSceneTypesRegistered();

//! Writes every entity in `world` (with its hierarchy and registered components) as a `.hpscene` document.
//! Entity order is slot order, so saving twice produces identical bytes unless something changed.
JsonValue WriteScene(const World& world, const SceneSaveOptions& options = {});

struct SceneLoadOptions
{
    //! When true the existing contents of `world` are cleared first; otherwise the file's entities are added to it
    //! (used by prefab instantiation and streaming levels in).
    bool ClearWorldFirst = true;

    //! Ignores the identities stored in the document and assigns a fresh Guid to every entity.
    //!
    //! Required by prefab instantiation: spawning the same prefab twice must not produce two entities claiming one
    //! Guid, because cross-file references (script bindings, gameplay links) resolve *by* Guid and would then bind to
    //! whichever copy the registry happens to reach first. Scene loading keeps stored identities — that is the whole
    //! point of persisting them — so this is opt-in rather than a property of loading in general.
    bool AssignFreshIdentities = false;

    Serialization::ConversionOptions Conversion;
};

//! Parses a `.hpscene` document into `world`. Returns a report; `Report.Loaded` is false only when the document is
//! structurally unusable (not an object, unknown major version), in which case `world` is untouched.
SceneLoadReport LoadScene(World& world, const JsonValue& document, const SceneLoadOptions& options = {});

//! Convenience: parses text then loads. Reports a JSON syntax error as a single load error rather than throwing.
SceneLoadReport LoadSceneFromText(World& world, std::string_view text, const SceneLoadOptions& options = {});

// --- File I/O ------------------------------------------------------------------
//
// Thin wrappers over the text functions plus Core::FileSystem, which owns path handling, encoding and failure
// classification. They live here rather than at each call site for two reasons: a save that silently truncates the old
// file destroys a level when the process dies mid-write (WriteFileAtomic is the only correct choice, and it must not be
// something every caller has to remember), and a load failure has to distinguish "no such file" from "unparseable" —
// an editor offers "create it" for the first and "here is the syntax error" for the second.

//! Outcome of saving: either success, or why nothing was written. Carries no report because a save that got as far as
//! producing JSON has already succeeded or failed at writing — the two-step (serialise, then write) keeps a
//! serialisation problem from being reported as an I/O one.
struct SceneSaveFileResult
{
    bool Saved = false;
    Core::FileError Error = Core::FileError::None;
    std::string Message;  //!< Empty on success; names the path and reason otherwise.
};

//! Serialises `world` and writes it atomically, creating parent directories as needed. Bytes are identical to
//! WriteScene + WriteJson, so a file saved here round-trips with the text API and stays diff-friendly in version
//! control.
[[nodiscard]] SceneSaveFileResult SaveSceneToFile(const World& world, std::string_view path,
                                                  const SceneSaveOptions& options = {});

//! Reads a `.hpscene` file and loads it. A missing or unreadable file reports through the returned report's Errors
//! with the path in the message, so callers have one failure channel for "could not read" and "would not parse" —
//! which is what an editor needs to decide between offering to create a scene and showing a syntax error.
[[nodiscard]] SceneLoadReport LoadSceneFromFile(World& world, std::string_view path,
                                                const SceneLoadOptions& options = {});

}  // namespace Hollowpoint::Scene
