#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/Transform.h"
#include "Hollowpoint/Scene/Prefab.h"

namespace Hollowpoint::Scripting
{

class ScriptVM;

// ---------------------------------------------------------------------------
// EngineAPI — the functions gameplay scripts can call.
//
// Design rules, all of them enforced in code rather than by convention:
//
//   * **Entities are handles, never objects.** Every entry point takes an int64 handle and validates it against the
//     world before use. A script cannot address freed memory because it holds no pointer to anything (see
//     EntityHandle.h for why this beats exposing usertypes).
//   * **Every mutating call is checked.** Destroying a dead entity, setting a transform on an entity that has no
//     LocalTransform, spawning into a dead parent: each returns false / reports rather than asserting. Scripts are
//     authored against game logic and get it wrong routinely; a hard assert turns a content bug into a crash for
//     whoever is playtesting.
//   * **Prefab access goes through a provider callback, not a registry owned here.** The asset system (which does
//     disk I/O, hot reload and dependency tracking) is not built yet, and the scripting layer must not guess at its
//     interface. Instead the host supplies `std::function<const Prefab*(std::string_view)>`, so scripts get
//     `spawn_prefab(name, ...)` today and the asset system plugs in behind it unchanged later. It also makes the whole
//     API testable without any filesystem.
//   * **Created entities are tracked.** Scripts create and destroy entities every frame; anything holding an Entity
//   that
//     the world recycles is a bug, so the API never caches entity handles across calls — each one is decoded and
//     revalidated from the caller's argument every time.
// ---------------------------------------------------------------------------

//! Supplied by the host to resolve a prefab by name. Returning nullptr means "unknown asset", which scripts see as a
//! nil result plus a logged warning rather than an error.
using PrefabResolver = std::function<const Scene::Prefab*(std::string_view name)>;

struct EngineAPIConfig
{
    //! World scripts operate on. Must outlive the VM (the API stores a raw pointer deliberately: there is exactly one
    //! world per VM and its lifetime is owned by whoever created both).
    ECS::World* World = nullptr;

    PrefabResolver ResolvePrefab;
};

//! Registers the engine table (`hp`) on the VM. Idempotent: calling it twice rebinds the same names rather than
//! stacking duplicate functions.
void RegisterEngineAPI(ScriptVM& vm, const EngineAPIConfig& config);

// ---------------------------------------------------------------------------
// The Lua-facing surface, documented here because that is where it is used. Registered as the global `hp` table:
//
//   hp.create_entity()                       -> handle        new entity (with a LocalTransform)
//   hp.destroy_entity(handle)                -> bool          destroy it (and its subtree)
//   hp.is_alive(handle)                      -> bool          still present in the world
//   hp.get_parent(handle)                    -> handle (0 when root or invalid)
//   hp.get_first_child(handle)               -> handle (0 when none)
//   hp.get_next_sibling(handle)              -> handle (0 when none)
//   hp.set_parent(handle, parent)            -> bool          parent 0 detaches to root
//   hp.get_position(handle)                  -> x, y, z, ok   world position (ok false when the entity is gone)
//   hp.set_position(handle, x, y, z)         -> bool          local position (adds the component if absent)
//   hp.spawn_prefab(name)                    -> handle        first root of a new instance (0 on failure)
//   hp.spawn_prefab_at(name, x, y, z)        -> handle        ... placed at that world position (0 on failure)
//   hp.time()                                -> number        monotonic seconds since the VM started
//   hp.log_info(text) / hp.log_warning(text) -> nothing       engine log from scripts
//   hp.error_count()                         -> integer       errors caught by this VM so far
//
// Absent/failed results are the handle **0**, not Lua nil: 0 is truthy in Lua, so scripts must test
// `if h ~= 0 then` rather than `if h then`. That is a deliberate trade — it keeps every entity reference the same type
// and makes an uninitialised variable (`local e = 0`) mean "no entity" without a nil check at every use.
//
// Lifecycle hooks a script may define (all optional, called by the host's update dispatch):
//
//   function on_create(entity)               called once when the entity is created
//   function on_update(entity, delta_time)   called every frame
//   function on_destroy(entity)              called before destruction
// ---------------------------------------------------------------------------

}  // namespace Hollowpoint::Scripting
