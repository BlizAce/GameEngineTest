#include <algorithm>
#include <string>

#include <sol/sol.hpp>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Scripting/EntityHandle.h"
#include "Hollowpoint/Scripting/LuaInclude.h"
#include "Hollowpoint/Scripting/ScriptSystem.h"
#include "Hollowpoint/Scripting/ScriptVM.h"

namespace Hollowpoint::Scripting
{

bool ScriptSystem::BindScript(std::string_view name)
{
    if (m_VM == nullptr || !m_VM->IsInitialized() || name.empty())
    {
        return false;
    }

    lua_State* state = m_VM->GetLuaState();
    const std::string key(name);

    lua_getglobal(state, key.c_str());
    const bool isTable = lua_istable(state, -1) != 0;

    if (!isTable)
    {
        lua_pop(state, 1);
        // Reported, not asserted: the usual cause is a script file that failed to load or a global renamed during a
        // refactor, and both are invisible if binding just returns false.
        HP_LOG_WARNING("Scripting", "BindScript('{}'): no such global table; entities using it will not run", key);
        return false;
    }

    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);  // Pops the table and stores a reference to it.

    // Rebinding replaces rather than duplicates: hot reload re-runs this for the same names, and leaving the old
    // reference alive would keep the previous version of every script (and its closures) allocated forever.
    const auto existing = std::find_if(m_BoundScripts.begin(), m_BoundScripts.end(),
                                       [&key](const BoundScript& bound) { return bound.Name == key; });
    if (existing != m_BoundScripts.end())
    {
        if (existing->LuaReference != LUA_NOREF)
        {
            luaL_unref(state, LUA_REGISTRYINDEX, existing->LuaReference);
        }
        existing->LuaReference = reference;
        return true;
    }

    m_BoundScripts.push_back(BoundScript{key, reference});
    return true;
}

bool ScriptSystem::BindAndInitialize(std::string_view name, World& world)
{
    const bool wasBound = IsScriptBound(name);

    if (!BindScript(name))
    {
        return false;  // no such global table; nothing was initialised, and BindScript reported why
    }

    // Only the first bind initialises. Rebinding is hot reload, and running on_create over entities that have been
    // alive since before the reload would reset gameplay state (a script's own counters, positions it stored) with no
    // warning.
    if (wasBound)
    {
        return true;
    }

    // Snapshot first: on_create is arbitrary script code and may create or destroy entities, which walking live storage
    // cannot survive. Update() snapshots for the same reason; this is the same hazard one hook earlier.
    std::vector<Entity> candidates;
    world.ForEachEntity([&world, &name, &candidates](const Entity& entity) {
        const ScriptComponent* component = world.TryGetComponent<ScriptComponent>(entity);
        if (component != nullptr && component->ScriptName == name)
        {
            candidates.push_back(entity);
        }
    });

    for (const Entity& entity : candidates)
    {
        // Re-checked: an earlier on_create in this batch may have destroyed a later candidate.
        if (world.IsAlive(entity))
        {
            NotifyCreate(world, entity);
        }
    }
    return true;
}

bool ScriptSystem::IsScriptBound(std::string_view name) const
{
    return std::any_of(m_BoundScripts.begin(), m_BoundScripts.end(),
                       [name](const BoundScript& bound) { return bound.Name == name; });
}

void ScriptSystem::UnbindAll()
{
    if (m_VM != nullptr && m_VM->IsInitialized())
    {
        lua_State* state = m_VM->GetLuaState();
        for (const BoundScript& bound : m_BoundScripts)
        {
            if (bound.LuaReference != LUA_NOREF)
            {
                luaL_unref(state, LUA_REGISTRYINDEX, bound.LuaReference);
            }
        }
    }

    m_BoundScripts.clear();
    m_ReportedMissingScripts.clear();
}

namespace
{

//! Reports a missing script name at most once, so one unbound script used by two hundred entities produces one message
//! per reload rather than four thousand a second. The first occurrence is the useful one; repeats add nothing and bury
//! whatever else is in the log.
void ReportMissingScriptOnce(std::vector<std::string>& reported, const std::string& scriptName)
{
    if (std::find(reported.begin(), reported.end(), scriptName) != reported.end())
    {
        return;
    }

    reported.push_back(scriptName);
    HP_LOG_WARNING("Scripting", "Script '{}' is used by an entity but was never bound; it will not run", scriptName);
}

}  // namespace

std::string ScriptSystem::ResolveScriptName(const World& world, const Entity& entity) const
{
    if (!world.IsAlive(entity))
    {
        return {};
    }

    const ScriptComponent* component = world.TryGetComponent<ScriptComponent>(entity);
    if (component == nullptr || component->ScriptName.empty())
    {
        return {};
    }

    return component->ScriptName;
}

namespace
{

//! Calls `hookName` on a bound script with (handle, delta). Returns 0 when the hook is absent (normal), 1 on success,
//! -1 when the hook raised.
int InvokeHook(lua_State* state, int scriptReference, const char* hookName, std::int64_t entityHandle,
               float deltaTimeSeconds)
{
    // Push the script table from the registry reference.
    lua_geti(state, LUA_REGISTRYINDEX, scriptReference);
    if (lua_istable(state, -1) == 0)
    {
        lua_pop(state, 1);
        return 0;
    }

    lua_getfield(state, -1, hookName);
    if (lua_isfunction(state, -1) == 0)
    {
        // Absent or non-callable: the overwhelmingly common case (most scripts implement one or two hooks), so this is
        // not reported and costs two lookups.
        lua_pop(state, 2);
        return 0;
    }

    lua_pushinteger(state, static_cast<lua_Integer>(entityHandle));
    lua_pushnumber(state, static_cast<lua_Number>(deltaTimeSeconds));

    // lua_call's protected sibling: an error inside the hook must not propagate into C++, and the traceback is what
    // makes it fixable. The message Lua produces already carries chunk name and line number.
    const int status = lua_pcall(state, 2, 0, 0);
    if (status != LUA_OK)
    {
        const char* message = lua_tostring(state, -1);
        HP_LOG_ERROR("Scripting", "{}: {}", hookName, message != nullptr ? message : "(no error message)");
        lua_pop(state, 1);  // Error object.
        lua_pop(state, 1);  // Script table.
        return -1;
    }

    lua_pop(state, 1);  // Script table (the call consumed the arguments and results).
    return 1;
}

}  // namespace

void ScriptSystem::Update(World& world, float deltaTimeSeconds)
{
    // Note there is deliberately no "no scripts bound, so bail" fast path here. It looks free but it silences exactly
    // the failure worth reporting: an entity referencing a script that was never bound (a renamed behaviour, a script
    // file that failed to load) would tick zero times and say nothing when the host happens to have no other scripts
    // bound — which is how a whole level of AI goes missing with a clean log. The cheap guard is per-entity below.
    if (m_VM == nullptr || !m_VM->IsInitialized())
    {
        return;
    }

    lua_State* state = m_VM->GetLuaState();

    // The entity list is snapshotted before dispatch. A hook can create or destroy entities (spawn on death, split on
    // impact), and iterating the world's live storage while that happens would either skip entities or visit freed
    // slots. Handles in the snapshot are revalidated with IsAlive each step, so a destroyed entity is simply passed
    // over — including one destroyed by an earlier hook in this same frame.
    std::vector<Entity> tickable;
    world.ForEachEntity([&tickable](const Entity& entity) { tickable.push_back(entity); });

    for (const Entity& entity : tickable)
    {
        if (!world.IsAlive(entity))
        {
            continue;
        }

        const std::string scriptName = ResolveScriptName(world, entity);
        if (scriptName.empty())
        {
            continue;
        }

        const BoundScript* bound = nullptr;
        for (const BoundScript& candidate : m_BoundScripts)
        {
            if (candidate.Name == scriptName)
            {
                bound = &candidate;
                break;
            }
        }

        if (bound == nullptr)
        {
            ReportMissingScriptOnce(m_ReportedMissingScripts, scriptName);
            continue;
        }

        // The handle is resolved again here rather than reusing the loop variable's encoding: a hook may have destroyed
        // and recreated this entity earlier in the frame, and passing a stale generation to Lua would let it act on a
        // dead handle.
        const std::int64_t handle = EncodeEntity(entity);
        const int result = InvokeHook(state, bound->LuaReference, "on_update", handle, deltaTimeSeconds);

        if (result < 0)
        {
            ++m_ErrorCalls;
        }
        else if (result > 0)
        {
            ++m_UpdateCalls;
        }
    }
}

void ScriptSystem::NotifyCreate(World& world, const Entity& entity)
{
    DispatchLifecycle(world, entity, "on_create");
}

void ScriptSystem::NotifyDestroy(World& world, const Entity& entity)
{
    // Runs *before* destruction: the script's cleanup needs the entity still queryable (to read its position for an
    // effect, or to find the entities it registered itself with), and after destruction the ScriptComponent itself is
    // gone so there would be nothing to look up.
    DispatchLifecycle(world, entity, "on_destroy");
}

void ScriptSystem::DispatchLifecycle(World& world, const Entity& entity, const char* hookName)
{
    if (m_VM == nullptr || !m_VM->IsInitialized())
    {
        return;
    }

    const std::string scriptName = ResolveScriptName(world, entity);
    if (scriptName.empty())
    {
        return;
    }

    for (const BoundScript& bound : m_BoundScripts)
    {
        if (bound.Name != scriptName)
        {
            continue;
        }

        // Lifecycle hooks take only the entity, so deltaTime is irrelevant and passed as zero.
        const int result = InvokeHook(m_VM->GetLuaState(), bound.LuaReference, hookName, EncodeEntity(entity), 0.0f);
        if (result < 0)
        {
            ++m_ErrorCalls;
        }
        return;
    }

    ReportMissingScriptOnce(m_ReportedMissingScripts, scriptName);
}

}  // namespace Hollowpoint::Scripting
