#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Reflection/Registry.h"

namespace Hollowpoint::Scripting
{

using ECS::Entity;
using ECS::World;

class ScriptVM;

// ---------------------------------------------------------------------------
// ScriptComponent — attaches a script to an entity.
//
// It stores only the *name* of a script table, not a reference to it. Lua values cannot be stored in a component
// anyway (the VM owns them and they are not copyable into engine memory), and storing a name means the component is
// trivially serialisable: a scene file says which behaviour an entity has, and the binding is re-established when the
// script is loaded. That ordering also means loading a scene before its scripts is safe — entities exist with an
// unresolved script name and simply do not tick until the script arrives, rather than failing the load.
// ---------------------------------------------------------------------------

struct ScriptComponent
{
    //! Name of the Lua table holding this entity's hooks, e.g. "PatrolGuard". Empty means "no script attached yet",
    //! which is a valid state (an entity created by a level designer before the behaviour was written).
    std::string ScriptName;

    bool operator==(const ScriptComponent&) const = default;
};

// ---------------------------------------------------------------------------
// ScriptSystem — dispatches lifecycle hooks to scripts.
//
// Hook lookup: an entity's script name must be registered as a **function reference** via BindScript() before it can
// tick. Names found in components are looked up in that registry, and an unregistered name is reported once (not per
// frame) — this is what makes "loading a scene before its scripts" safe: entities hold the name, nothing runs until
// BindScript() supplies the function, and forgetting to bind produces one clear message rather than silently dead
// behaviour.
//
// The alternative — resolving `N.on_update` as a Lua global on every call — was rejected: it costs two table lookups
// per entity per frame for no benefit, and it lets any script overwrite another's globals at runtime (a typo assigning
// to a shared name would hijack unrelated entities' behaviour). A registry of explicitly-bound functions means the set
// of callable code is fixed by the host.
//
// Error policy: a hook that raises is reported once per call and the VM stays usable (the VM's protected calls make
// that possible). Errors do *not* stop the remaining entities from ticking — one broken script must not freeze every
// other AI in the level, which is what aborting the loop would do. The error count is exposed so a debug overlay can
// show "scripts are failing every frame" instead of the failure being invisible behind normal-looking behaviour.
//
// Ordering: entities tick in slot order, deterministically. Script-visible order must not vary between runs or replays
// desynchronise, and "which entity ran first this frame" is exactly the kind of nondeterminism that takes days to find
// once a game depends on it.
// ---------------------------------------------------------------------------

class ScriptSystem
{
public:
    explicit ScriptSystem(ScriptVM& vm) noexcept : m_VM(&vm) {}

    //! Binds the Lua global `name` (a table with hook fields such as `on_update`) under the script name `name`, so
    //! entities can reference it. Returns false when no such global exists or it is not a table — reported rather than
    //! asserted because scripts are content and a missing/renamed global is an everyday authoring mistake.
    //!
    //! Taking the name (rather than a sol2 object) keeps Lua types out of this header: the binding is stored as a
    //! registry reference, so ScriptSystem.h needs neither sol2 nor lua.h, and includers do not pay for either. It also
    //! fixes the set of callable code at bind time — a script cannot later be swapped by assigning to a shared global,
    //! which would otherwise let one typo hijack unrelated entities' behaviour.
    bool BindScript(std::string_view name);

    [[nodiscard]] bool IsScriptBound(std::string_view name) const;

    //! Drops every binding and releases the Lua references. Called on reload so stale references to discarded script
    //! tables cannot keep old code (and everything it closes over) alive for the process's lifetime.
    void UnbindAll();

    //! Ticks every entity with a ScriptComponent. `deltaTimeSeconds` is passed to each hook unchanged — the caller owns
    //! time scaling and pausing, which is gameplay policy rather than scripting policy.
    void Update(World& world, float deltaTimeSeconds);

    //! Calls the on_create hook for an entity. Called by whoever creates the entity from a scripted context so the
    //! script can initialise itself; deliberately not automatic, because deciding "this entity is new" inside the
    //! system would require tracking every creation path in the world (including scene load and prefab instantiation)
    //! and would fire for entities that are only half-initialised.
    void NotifyCreate(World& world, const Entity& entity);

    //! Calls the on_destroy hook. Must run **before** destruction: once the entity is gone its components (including
    //! the script name) are unreadable, and a script's cleanup — release a reservation, decrement a counter, spawn an
    //! explosion — needs the entity to still be queryable.
    void NotifyDestroy(World& world, const Entity& entity);

    //! Number of hook invocations since the last ResetCallCounters(), for tests and for a debug HUD that shows script
    //! activity per frame (a script silently not running looks identical to one running at zero rate otherwise).
    [[nodiscard]] std::uint64_t GetUpdateCallCount() const noexcept
    {
        return m_UpdateCalls;
    }

    [[nodiscard]] std::uint64_t GetErrorCount() const noexcept
    {
        return m_ErrorCalls;
    }

    void ResetCallCounters() noexcept
    {
        m_UpdateCalls = 0;
        m_ErrorCalls = 0;
    }

private:
    //! Resolves an entity's script name, or empty when it has no script. Returns by value because the string lives in a
    //! component that Lua code could legitimately change during a hook (respawn logic does exactly this).
    [[nodiscard]] std::string ResolveScriptName(const World& world, const Entity& entity) const;

    //! Shared lifecycle dispatch: resolve the entity's script, find its binding, call `hookName` with just the handle.
    void DispatchLifecycle(World& world, const Entity& entity, const char* hookName);

    //! Hook names are fixed, so they are interned once. `on_update` etc. are looked up on the bound table per call —
    //! sol2 caches nothing here, but a missing hook is one failed field lookup and that is the intended cheap path.
    struct BoundScript
    {
        std::string Name;

        //! Reference into Lua's registry (luaL_ref). A raw stack index would be invalidated by any later push, and a
        //! borrowed reference would not keep the table alive — the registry ref does both jobs in one integer.
        int LuaReference = -1;
    };

    // Small vector semantics matter more than the container choice: real projects have tens of distinct scripts, not
    // thousands, and linear search over that beats hashing while keeping the bound tables contiguous in memory.
    std::vector<BoundScript> m_BoundScripts;

    //! Names already reported as unbound, so a level with 200 entities using one missing script produces one message
    //! rather than 200 per frame. Cleared by ResetCallCounters() so tools can re-arm reporting after a reload.
    std::vector<std::string> m_ReportedMissingScripts;

    ScriptVM* m_VM = nullptr;
    std::uint64_t m_UpdateCalls = 0;
    std::uint64_t m_ErrorCalls = 0;
};

}  // namespace Hollowpoint::Scripting
