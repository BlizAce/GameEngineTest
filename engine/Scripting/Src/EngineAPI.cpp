#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Core/Time.h"
#include "Hollowpoint/ECS/Hierarchy.h"
#include "Hollowpoint/Math/Angles.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"
#include "Hollowpoint/Scripting/EngineAPI.h"
#include "Hollowpoint/Scripting/EntityHandle.h"
#include "Hollowpoint/Scripting/LuaInclude.h"
#include "Hollowpoint/Scripting/ScriptVM.h"

namespace Hollowpoint::Scripting
{

using ECS::Entity;
using ECS::World;
using Math::Quat;
using Math::Transform;
using Math::Vec3;
using Scene::LocalTransform;
using Scene::Prefab;
using Scene::TransformHierarchy;

namespace
{

// Forward uses of these are fine: they are defined at namespace scope below and only called from RegisterEngineAPI.

//! Per-VM engine context, reached by every API function through a light-userdata upvalue.
//!
//! Owned by ScriptVM and destroyed deterministically in its destructor — never by Lua. That ownership choice is the
//! reason this file uses the raw Lua C API rather than sol2's `set_function`: sol2 stores a capturing functor as Lua
//! *userdata with a `__gc` finalizer, so the functor (and everything it captures) is destroyed whenever the collector
//! decides, including during lua_close and including after the World it points at has already been destroyed. The first
//! version did that and crashed in `_Sp_counted_base::_M_release` on a control block overwritten with matrix floats —
//! order-dependent, reproducible only under some test orderings, and impossible to fix reliably from inside a lambda.
//! A light userdata carries no finalizer at all: Lua cannot destroy what it does not own.
struct ApiState
{
    explicit ApiState(const EngineAPIConfig& config)
        : WorldPointer(config.World), ResolvePrefab(config.ResolvePrefab), StartSeconds(Core::GetMonotonicSeconds())
    {}

    //! Scripts operate against exactly one world, owned by whoever created both it and the VM. Raw because there is no
    //! ownership to express: the host must keep it alive for the VM's lifetime, which is the documented contract.
    World* WorldPointer = nullptr;

    PrefabResolver ResolvePrefab;

    //! World transforms for script queries, refreshed on demand. TransformHierarchy short-circuits when nothing in the
    //! world changed, so repeated reads in one frame cost a comparison rather than a traversal. Mutating API calls set
    //! `TransformsStale` because a LocalTransform write does not bump the world's structure version, and without that
    //! flag a script reading its own position would see the value from before the write.
    TransformHierarchy Transforms;
    bool TransformsStale = true;

    double StartSeconds = 0.0;

    void RefreshTransforms()
    {
        if (TransformsStale)
        {
            Transforms.Update(*WorldPointer);
            TransformsStale = false;
        }
    }
};

//! Upvalue index holding the ApiState pointer for every registered function.
constexpr int ContextUpvalue = lua_upvalueindex(1);

[[nodiscard]] ApiState& Context(lua_State* state) noexcept
{
    return *static_cast<ApiState*>(lua_touserdata(state, ContextUpvalue));
}

//! Resolves a handle to a live entity, or returns null.
//!
//! Deliberately quiet: a script holding a stale handle across frames is routine (an entity destroyed by gameplay logic
//! while another system still references it), and logging per call would emit thousands of lines per second. Call sites
//! that have reason to think the argument *should* be alive report explicitly.
[[nodiscard]] Entity Resolve(ApiState& context, std::int64_t handle) noexcept
{
    if (context.WorldPointer == nullptr)
    {
        return Entity();
    }

    const Entity entity = DecodeEntity(handle);
    return context.WorldPointer->IsAlive(entity) ? entity : Entity();
}

//! Pushes (x, y, z, ok). A dead entity yields zeroes with ok=false rather than an error, so a script that lost its
//! entity reads a defined position instead of aborting its handler.
void PushPosition(lua_State* state, const Vec3& position, bool valid)
{
    lua_pushnumber(state, position.X);
    lua_pushnumber(state, position.Y);
    lua_pushnumber(state, position.Z);
    lua_pushboolean(state, valid ? 1 : 0);
}

//! Pushes a (possibly zero) entity handle.
void PushHandle(lua_State* state, const Entity& entity)
{
    lua_pushinteger(state, static_cast<lua_Integer>(EncodeEntity(entity)));
}

// ---------------------------------------------------------------------------
// Registered functions. Each takes its context from the upvalue and never throws: an error inside one of these would
// surface as a Lua error at the call site, which is contained by the VM's protected calls — but returning early keeps
// messages meaningful ("entity is not alive") instead of a C++ exception rendered as an opaque traceback.
// ---------------------------------------------------------------------------

int CreateEntity(lua_State* state)
{
    ApiState& context = Context(state);
    if (context.WorldPointer == nullptr)
    {
        PushHandle(state, Entity());
        return 1;
    }

    const Entity entity = context.WorldPointer->CreateEntity();

    // Every script-placeable entity gets a LocalTransform. Without one there is nothing for set_position to write and
    // the entity contributes no placement to the hierarchy, so creating it here is what makes "spawn then move" work
    // the obvious way instead of silently doing nothing.
    context.WorldPointer->AddComponent<LocalTransform>(entity);

    context.TransformsStale = true;
    PushHandle(state, entity);
    return 1;
}

int DestroyEntity(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        lua_pushboolean(state, 0);
        return 1;
    }

    // The world defers actual destruction to its flush point, so an entity destroyed mid-frame keeps valid memory for
    // the rest of that frame's other systems — which is what stops "destroy this" from invalidating a handle another
    // system holds in the same tick. IsAlive goes false immediately; the slot is not recycled until the flush.
    context.WorldPointer->DestroyEntity(entity);
    context.TransformsStale = true;

    lua_pushboolean(state, 1);
    return 1;
}

int IsAlive(lua_State* state)
{
    ApiState& context = Context(state);
    if (context.WorldPointer == nullptr)
    {
        lua_pushboolean(state, 0);
        return 1;
    }

    const Entity entity = DecodeEntity(static_cast<std::int64_t>(lua_tointeger(state, 1)));
    lua_pushboolean(state, context.WorldPointer->IsAlive(entity) ? 1 : 0);
    return 1;
}

//! hp.get_parent(e) -> handle (0 when root or invalid)
int GetParent(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        PushHandle(state, Entity());
        return 1;
    }

    // Returns 0 for "no parent", covering both a root and an invalid argument. Deliberately not distinguished: to a
    // script both mean "nothing above this", and encoding which kind of nothing arrived invites `if p then` bugs (0 is
    // truthy in Lua) for a case no caller acts on.
    PushHandle(state, context.WorldPointer->GetParent(entity));
    return 1;
}

//! First child / next sibling: enough to walk a hierarchy without exposing the world itself. A script enumerating
//! children is common (a turret listing its mounts) and needs nothing richer than this.
int GetFirstChild(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        PushHandle(state, Entity());
        return 1;
    }

    PushHandle(state, context.WorldPointer->GetFirstChild(entity));
    return 1;
}

int GetNextSibling(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        PushHandle(state, Entity());
        return 1;
    }

    PushHandle(state, context.WorldPointer->GetNextSibling(entity));
    return 1;
}

int SetParent(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        lua_pushboolean(state, 0);
        return 1;
    }

    // 0 means "detach to root", which scripts need (a picked-up object leaving its container).
    Entity parent;
    const lua_Integer parentArgument = luaL_checkinteger(state, 2);
    if (parentArgument != 0)
    {
        parent = Resolve(context, static_cast<std::int64_t>(parentArgument));
        if (parent.IsNull())
        {
            HP_LOG_WARNING("Scripting", "hp.set_parent: the requested parent handle is not alive");
            lua_pushboolean(state, 0);
            return 1;
        }
    }

    const ECS::Hierarchy::ReparentResult result = context.WorldPointer->SetParent(entity, parent);
    if (result != ECS::Hierarchy::ReparentResult::Success && result != ECS::Hierarchy::ReparentResult::Unchanged)
    {
        // Cycles in particular must be refused loudly: parenting an entity into its own subtree would otherwise hang
        // world-transform evaluation, and the reason has to be named or "why did my parenting do nothing" is
        // undebuggable.
        HP_LOG_WARNING("Scripting", "hp.set_parent rejected ({})", ECS::Hierarchy::ReparentResultName(result));
        lua_pushboolean(state, 0);
        return 1;
    }

    context.TransformsStale = true;
    lua_pushboolean(state, 1);
    return 1;
}

int GetPosition(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        PushPosition(state, Vec3(0.0f, 0.0f, 0.0f), false);
        return 4;
    }

    context.RefreshTransforms();
    PushPosition(state, context.Transforms.GetWorldMatrix(*context.WorldPointer, entity).GetTranslation(), true);
    return 4;
}

int SetPosition(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        lua_pushboolean(state, 0);
        return 1;
    }

    const auto x = static_cast<float>(luaL_checknumber(state, 2));
    const auto y = static_cast<float>(luaL_checknumber(state, 3));
    const auto z = static_cast<float>(luaL_checknumber(state, 4));

    // AddComponent returns the existing instance when present, so this patches position without resetting rotation and
    // scale — which a read-modify-write through a copy would silently do.
    LocalTransform& local = context.WorldPointer->AddComponent<LocalTransform>(entity);
    local.Position = Vec3(x, y, z);

    context.TransformsStale = true;
    lua_pushboolean(state, 1);
    return 1;
}

//! hp.get_local_position(handle) -> x, y, z, ok
/**
 * Exists because get_position returns the **world** position while set_position writes the **local** one — both choices
 * are right for their common use (a script aiming a camera or measuring a distance wants world; a script placing an
 * entity under its authored parent wants local), but together they make the single most common script operation — "read
 * my transform, move myself relative to it" — expressible only incorrectly. A parented entity bobbed through
 * get_position/set_position drifts toward world axes as fast as it moves, and nothing logs a warning. Given that pair
 * in the API, the local form is the one a caller wants when they ask for "the position I set".
 */
int GetLocalPosition(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        PushPosition(state, Vec3(0.0f, 0.0f, 0.0f), false);
        return 4;
    }

    const LocalTransform* local = context.WorldPointer->TryGetComponent<LocalTransform>(entity);
    if (local == nullptr)
    {
        // A live entity with no transform: reported the same way a dead one is, because from script side the response
        // is identical — do not move it. hp.create_entity always adds the component, so this is an entity created by
        // C++.
        PushPosition(state, Vec3(0.0f, 0.0f, 0.0f), false);
        return 4;
    }

    PushPosition(state, local->Position, true);
    return 4;
}

//! hp.set_rotation(handle, x, y, z, w) -> bool — local rotation from a quaternion.
int SetRotation(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        lua_pushboolean(state, 0);
        return 1;
    }

    const auto x = static_cast<float>(luaL_checknumber(state, 2));
    const auto y = static_cast<float>(luaL_checknumber(state, 3));
    const auto z = static_cast<float>(luaL_checknumber(state, 4));
    const auto w = static_cast<float>(luaL_checknumber(state, 5));

    // Normalised rather than rejected when off by rounding. A quaternion built in Lua from angle/axis arithmetic is
    // rarely unit length to the last bit, and Mat4::FromQuaternion on a non-unit input scales the entity — a visible
    // corruption whose cause (an unnormalised script value) is three layers away from the symptom. Normalized() maps a
    // near-zero quaternion to identity, which is the only sensible reading of "no rotation specified".
    LocalTransform& local = context.WorldPointer->AddComponent<LocalTransform>(entity);
    local.Rotation = Quat(x, y, z, w).Normalized();

    context.TransformsStale = true;
    lua_pushboolean(state, 1);
    return 1;
}

//! hp.get_rotation(handle) -> x, y, z, w, ok — local rotation as a quaternion.
/**
 * Quaternion rather than Euler angles because it is the stored form: an Euler round-trip through three numbers would
 * let a script that reads and writes unchanged rotate the entity (gimbal decomposition is not unique), and the fix on
 * the script side would be "don't do that". Scripts that want angles build a quaternion from them —
 * hp.set_rotation_euler.
 */
int GetRotation(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));

    Quat rotation = Quat::Identity();
    bool valid = false;
    if (!entity.IsNull())
    {
        if (const LocalTransform* local = context.WorldPointer->TryGetComponent<LocalTransform>(entity);
            local != nullptr)
        {
            rotation = local->Rotation;
            valid = true;
        }
    }

    lua_pushnumber(state, rotation.X);
    lua_pushnumber(state, rotation.Y);
    lua_pushnumber(state, rotation.Z);
    lua_pushnumber(state, rotation.W);
    lua_pushboolean(state, valid ? 1 : 0);
    return 5;
}

//! hp.set_rotation_euler(handle, yaw, pitch, roll) -> bool — radians, applied in the engine's Y→X→Z order.
/**
 * The convenience half of set_rotation, and the form most content is authored in. Order is EulerAngles' documented
 * Y→X→Z (yaw/pitch/roll), i.e. the same convention the editor gizmo and Math::EulerAngles use — a second convention
 * here would put two answers one call apart.
 */
int SetRotationEuler(lua_State* state)
{
    ApiState& context = Context(state);
    const Entity entity = Resolve(context, static_cast<std::int64_t>(lua_tointeger(state, 1)));
    if (entity.IsNull())
    {
        lua_pushboolean(state, 0);
        return 1;
    }

    const auto yaw = static_cast<float>(luaL_checknumber(state, 2));
    const auto pitch = static_cast<float>(luaL_checknumber(state, 3));
    const auto roll = static_cast<float>(luaL_checknumber(state, 4));

    LocalTransform& local = context.WorldPointer->AddComponent<LocalTransform>(entity);
    local.Rotation = Math::EulerAngles(yaw, pitch, roll).ToQuaternion();

    context.TransformsStale = true;
    lua_pushboolean(state, 1);
    return 1;
}

//! Shared implementation of spawn_prefab / spawn_prefab_at. `placementOffset` is null for the unpositioned form.
int SpawnPrefabImpl(lua_State* state, ApiState& context, const std::optional<Vec3>& placementPosition)
{
    if (context.WorldPointer == nullptr || !context.ResolvePrefab)
    {
        HP_LOG_WARNING("Scripting", "hp.spawn_prefab: no prefab resolver is configured");
        PushHandle(state, Entity());
        return 1;
    }

    std::size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    const std::string_view nameView(name, nameLength);

    const Prefab* prefab = context.ResolvePrefab(nameView);
    if (prefab == nullptr)
    {
        // The name is reported because the common causes are a typo or an asset that failed to import, and neither is
        // guessable from "spawn returned nil".
        HP_LOG_WARNING("Scripting", "hp.spawn_prefab: unknown prefab '{}'", nameView);
        PushHandle(state, Entity());
        return 1;
    }

    Scene::InstantiateOptions options;
    // Fresh identities always: two instances of one asset must not claim the same Guid, since references resolve by
    // Guid and would otherwise bind to an arbitrary copy.
    options.AssignFreshIdentities = true;

    if (placementPosition.has_value())
    {
        Transform placement;
        placement.Position = *placementPosition;
        options.RootTransform = placement;
    }

    const Scene::InstantiateResult result = Scene::InstantiatePrefab(*context.WorldPointer, *prefab, options);
    if (!result.Created || result.Roots.empty())
    {
        HP_LOG_WARNING("Scripting", "hp.spawn_prefab: instantiating '{}' produced no entities", nameView);
        PushHandle(state, Entity());
        return 1;
    }

    context.TransformsStale = true;

    // Only the first root is returned. A multi-root prefab (a swarm) is legitimate, and a script needing every root can
    // walk the hierarchy from here — but one handle keeps the common case's signature simple.
    PushHandle(state, result.Roots[0]);
    return 1;
}

int SpawnPrefab(lua_State* state)
{
    return SpawnPrefabImpl(state, Context(state), std::nullopt);
}

int SpawnPrefabAt(lua_State* state)
{
    const auto x = static_cast<float>(luaL_checknumber(state, 2));
    const auto y = static_cast<float>(luaL_checknumber(state, 3));
    const auto z = static_cast<float>(luaL_checknumber(state, 4));

    return SpawnPrefabImpl(state, Context(state), Vec3(x, y, z));
}

int GetTime(lua_State* state)
{
    // Monotonic: a wall-clock adjustment (DST, NTP correction) must not send gameplay timers backwards or skip them.
    lua_pushnumber(state, Core::GetMonotonicSeconds() - Context(state).StartSeconds);
    return 1;
}

int LogInfo(lua_State* state)
{
    std::size_t length = 0;
    const char* message = luaL_checklstring(state, 1, &length);
    // string_view, so an embedded NUL in the script's string is preserved rather than truncating the log line.
    HP_LOG_INFO("Script", "{}", std::string_view(message, length));
    return 0;
}

int LogWarning(lua_State* state)
{
    std::size_t length = 0;
    const char* message = luaL_checklstring(state, 1, &length);
    HP_LOG_WARNING("Script", "{}", std::string_view(message, length));
    return 0;
}

//! Reads the VM's error counter from script side, so a test or debug overlay can assert "my handlers ran clean" without
//! scraping the log. The VM pointer is a second upvalue rather than the context because the counter lives on the VM.
constexpr int VmUpvalue = lua_upvalueindex(2);

int ErrorCount(lua_State* state)
{
    const auto* vm = static_cast<const ScriptVM*>(lua_touserdata(state, VmUpvalue));
    lua_pushinteger(state, static_cast<lua_Integer>(vm != nullptr ? vm->GetErrorCount() : 0u));
    return 1;
}

struct Registration
{
    const char* Name;
    lua_CFunction Function;
};

constexpr Registration Registrations[] = {
    {"create_entity", CreateEntity},
    {"destroy_entity", DestroyEntity},
    {"is_alive", IsAlive},
    {"get_parent", GetParent},
    {"get_first_child", GetFirstChild},
    {"get_next_sibling", GetNextSibling},
    {"set_parent", SetParent},
    {"get_position", GetPosition},
    {"set_position", SetPosition},
    {"get_local_position", GetLocalPosition},
    {"get_rotation", GetRotation},
    {"set_rotation", SetRotation},
    {"set_rotation_euler", SetRotationEuler},
    {"spawn_prefab", SpawnPrefab},
    {"spawn_prefab_at", SpawnPrefabAt},
    {"time", GetTime},
    {"log_info", LogInfo},
    {"log_warning", LogWarning},
};

void RegisterEngineAPIFunctions(lua_State* state, void* opaqueContext, const ScriptVM& vm)
{
    if (state == nullptr || opaqueContext == nullptr)
    {
        HP_LOG_ERROR("Scripting", "Cannot register the engine API without a VM and a context");
        return;
    }

    // A fresh table each registration, so rebinding replaces the surface rather than merging into a stale one.
    lua_createtable(state, 0, static_cast<int>(std::size(Registrations)));

    for (const Registration& registration : Registrations)
    {
        // The context travels as a light-userdata upvalue: unreachable from script code (the debug library that could
        // read upvalues is never opened) and never finalised by the collector. See ApiState.
        lua_pushlightuserdata(state, opaqueContext);
        lua_pushcclosure(state, registration.Function, 1);
        lua_setfield(state, -2, registration.Name);
    }

    // error_count needs the VM itself, so it carries a second upvalue. The const_cast is about Lua's C API requiring a
    // non-const void* for light userdata; the function only reads through it.
    lua_pushlightuserdata(state, opaqueContext);
    lua_pushlightuserdata(state, const_cast<ScriptVM*>(&vm));
    lua_pushcclosure(state, ErrorCount, 2);
    lua_setfield(state, -2, "error_count");

    lua_setglobal(state, "hp");
}

void DestroyEngineAPIContext(void* context) noexcept
{
    delete static_cast<ApiState*>(context);
}

}  // namespace

void RegisterEngineAPI(ScriptVM& vm, const EngineAPIConfig& config)
{
    if (!vm.IsInitialized())
    {
        HP_LOG_ERROR("Scripting", "Cannot register the engine API on an uninitialized VM");
        return;
    }

    auto* context = new ApiState(config);

    // Ownership transfers to the VM before registration, so nothing leaks if registration throws: the VM destroys its
    // context deterministically, before lua_close, whatever else happens.
    vm.SetEngineAPIContext(context, &DestroyEngineAPIContext);

    RegisterEngineAPIFunctions(vm.GetLuaState(), context, vm);
}

}  // namespace Hollowpoint::Scripting
