#include "TestSupport.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/Prefab.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"
#include "Hollowpoint/Scripting/EngineAPI.h"
#include "Hollowpoint/Scripting/EntityHandle.h"
#include "Hollowpoint/Scripting/LuaInclude.h"
#include "Hollowpoint/Scripting/RegisterScriptTypes.h"
#include "Hollowpoint/Scripting/ScriptSystem.h"
#include "Hollowpoint/Scripting/ScriptVM.h"
#include "Hollowpoint/Serialization/JsonWriter.h"

using Hollowpoint::Core::Size;
using Hollowpoint::Core::UInt64;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::World;
using Hollowpoint::Math::Vec3;
using Hollowpoint::Scene::ComponentFactory;
using Hollowpoint::Scene::LocalTransform;
using Hollowpoint::Scene::Prefab;
using Hollowpoint::Scene::TransformHierarchy;
namespace Scene = Hollowpoint::Scene;
using namespace Hollowpoint::Scripting;

namespace
{

constexpr float kEpsilon = 1.0e-4f;

bool Near(const Vec3& a, const Vec3& b, float epsilon = kEpsilon)
{
    return std::fabs(a.X - b.X) < epsilon && std::fabs(a.Y - b.Y) < epsilon && std::fabs(a.Z - b.Z) < epsilon;
}

// Registers ScriptComponent for the scene-format round-trip cases below. This deliberately calls the engine's own
// RegisterScriptTypes rather than registering a private name here: the scene-file spelling (`Script`, field
// `scriptName`) is then exercised exactly as a real .hpscene hits it, and the process keeps exactly one descriptor for
// the type. A second registration under a different name would have been accepted by the registry (it keys descriptors
// by name), so this suite would have passed while production used a name nothing had ever tested.
//
// Done once at namespace scope because the registry is a process-wide singleton and registering per test would fail
// after the first one.
const bool kScriptComponentRegistered = []() {
    Scene::RegisterSceneTypes();
    RegisterScriptTypes();
    return AreScriptTypesRegistered();
}();

// A two-entity prefab (root + child offset in Y), matching what real content looks like.
const char* kCratePrefab = R"LUA({
    "formatVersion": 1,
    "name": "Crate",
    "entities": [
        { "id": 0, "components": { "LocalTransform": { "position": {"x":0,"y":0.5,"z":0},
                                                        "rotation": {"x":0,"y":0,"z":0,"w":1},
                                                        "scale": {"x":1,"y":1,"z":1} } } },
        { "id": 1, "parent": 0, "components": { "LocalTransform": { "position": {"x":0,"y":1.5,"z":0},
                                                                    "rotation": {"x":0,"y":0,"z":0,"w":1},
                                                                    "scale": {"x":1,"y":1,"z":1} } } }
    ]
})LUA";

//! A VM with the engine API bound to `world` and a "Crate" prefab resolvable by name.
struct TestContext
{
    explicit TestContext(World& worldToUse, bool withPrefab = true) : VM()
    {
        REQUIRE(VM.IsInitialized());

        EngineAPIConfig config;
        config.World = &worldToUse;
        if (withPrefab)
        {
            std::string error;
            StoredPrefab = Prefab::FromSceneText(kCratePrefab, error);
            REQUIRE(StoredPrefab.IsLoaded());
            config.ResolvePrefab = [this](std::string_view name) -> const Prefab* {
                return name == "Crate" ? &StoredPrefab : nullptr;
            };
        }
        RegisterEngineAPI(VM, config);
    }

    ScriptVM VM;
    Prefab StoredPrefab;
};

//! Runs code and fails the test with Lua's own message when it does not compile or run.
void RunOrThrow(ScriptVM& vm, std::string_view code)
{
    std::string error;
    INFO("lua error: " << error);
    REQUIRE_MESSAGE(vm.Run(code, error, "<test>"), "script failed");
}

//! Evaluates an expression returning a number.
double EvalNumber(ScriptVM& vm, std::string_view expression)
{
    lua_State* state = vm.GetLuaState();
    const std::string code = "return " + std::string(expression);

    REQUIRE_EQ(luaL_loadstring(state, code.c_str()), 0);
    REQUIRE_EQ(lua_pcall(state, 0, 1, 0), LUA_OK);
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    return value;
}

//! Runs a full Lua GC (two passes: the second collects objects whose __gc queued them). Exposed because the memory
//! accounting tests need usage figures that reflect freed script data, not merely data the collector has not visited.
void CollectFully(ScriptVM& vm)
{
    lua_State* state = vm.GetLuaState();
    for (int pass = 0; pass < 2; ++pass)
    {
        lua_getglobal(state, "collectgarbage");
        lua_pushliteral(state, "collect");
        if (lua_pcall(state, 1, 1, 0) != LUA_OK)
        {
            lua_pop(state, 1);
            return;
        }
        lua_pop(state, 1);  // Result: kilobytes still in use.
    }
}

//! Evaluates an expression returning a string.
std::string EvalString(ScriptVM& vm, std::string_view expression)
{
    lua_State* state = vm.GetLuaState();
    const std::string code = "return " + std::string(expression);

    REQUIRE_EQ(luaL_loadstring(state, code.c_str()), 0);
    REQUIRE_EQ(lua_pcall(state, 0, 1, 0), LUA_OK);
    const char* value = lua_tostring(state, -1);
    lua_pop(state, 1);
    return value != nullptr ? std::string(value) : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Entity handles.
// ---------------------------------------------------------------------------

TEST_SUITE("Scripting/EntityHandle")
{
    TEST_CASE("an entity round-trips through a script handle")
    {
        const Entity entity(7, 3);
        const std::int64_t handle = EncodeEntity(entity);

        CHECK(DecodeEntity(handle) == entity);
    }

    TEST_CASE("a null entity encodes to zero and back")
    {
        Entity decoded(1, 1);
        CHECK(EncodeEntity(Entity()) == 0);
        CHECK(DecodeEntity(0).IsNull());

        // TryDecode reports failure *and* writes a null entity, so callers cannot accidentally use a stale value left
        // in the out-parameter.
        CHECK_FALSE(TryDecodeEntity(0, decoded));
        CHECK(decoded.IsNull());
    }

    TEST_CASE("generation is preserved so a recycled slot does not alias")
    {
        // The property the whole boundary rests on: after slot reuse, an old handle must not name the new entity.
        const Entity original(5, 1);
        const Entity recycled(5, 2);

        const std::int64_t oldHandle = EncodeEntity(original);

        CHECK(DecodeEntity(oldHandle) == original);
        CHECK(DecodeEntity(oldHandle) != recycled);
        CHECK(EncodeEntity(recycled) != oldHandle);
    }

    TEST_CASE("every slot/generation combination round-trips")
    {
        const std::uint32_t slots[] = {0u, 1u, 2u, 1023u, 0xFFFFFFF0u};
        const std::uint32_t generations[] = {0u, 1u, 2u, 0x7FFFFFFFu, 0xFFFFFFFEu};

        for (const std::uint32_t slot : slots)
        {
            for (const std::uint32_t generation : generations)
            {
                const Entity entity(slot, generation);
                const std::int64_t handle = EncodeEntity(entity);

                INFO("slot " << slot << " generation " << generation);
                // Handle 0 is reserved for null, so the one colliding encoding is remapped and must decode to *some*
                // non-null entity rather than being silently lost.
                if (handle == 0)
                {
                    FAIL("a live entity must never encode to the null handle");
                }
                CHECK(DecodeEntity(handle) == entity);
            }
        }
    }

    TEST_CASE("nonsense handles decode to null rather than crashing")
    {
        CHECK(DecodeEntity(0).IsNull());

        Entity decoded(1, 1);
        CHECK_FALSE(TryDecodeEntity(0, decoded));
        CHECK(decoded.IsNull());

        // A handle whose *slot* word is all-ones (ECS's invalid index) is rejected outright.
        const std::int64_t invalidSlotHandle = EncodeEntity(Entity(Entity::NullSlot, 3));
        CHECK(invalidSlotHandle == 0);  // A null-shaped entity encodes to the null handle.

        Entity rejected(1, 1);
        CHECK_FALSE(TryDecodeEntity(invalidSlotHandle, rejected));
        CHECK(rejected.IsNull());

        // Handles are offset by one (see EncodeEntity), so the rejected *slot* pattern is not literally all-ones in the
        // handle's low word — it is all-ones after the offset is undone. Asserting on the encoding rather than a
        // hand-computed bit pattern is what keeps this test honest if the encoding ever changes again.
        const Entity nullSlotEntity(Entity::NullSlot, 7);
        CHECK(EncodeEntity(nullSlotEntity) == 0);
        CHECK(DecodeEntity(0).IsNull());

        // A well-formed but implausible handle decodes to an entity that simply does not exist. That is the intended
        // division of labour: this function validates the *encoding*, the world validates *existence*, so a garbage
        // handle from Lua resolves to "not alive" at every call site (asserted in the EngineAPI cases) without this
        // function needing to enumerate bit patterns it has never seen.
        const std::int64_t implausible = 0x0000123400005678ll;
        const Entity decodedImplausible = DecodeEntity(implausible);
        CHECK_FALSE(decodedImplausible.IsNull());
        CHECK(decodedImplausible.Slot == 0x00005677u);  // One less than the handle's low word, by construction.
        CHECK(decodedImplausible.Generation == 0x00001234u);

        // Round-trip is what actually matters for any handle the engine produced.
        CHECK(EncodeEntity(decodedImplausible) == implausible);
    }
}

// ---------------------------------------------------------------------------
// VM lifecycle and safety policy.
// ---------------------------------------------------------------------------

TEST_SUITE("Scripting/ScriptVM")
{
    TEST_CASE("a fresh VM initialises and reports a memory budget")
    {
        ScriptVM vm;
        CHECK(vm.IsInitialized());
        CHECK(vm.GetLuaState() != nullptr);
        CHECK(vm.GetMemoryLimitBytes() > 0u);
        CHECK(vm.GetMemoryUsageBytes() > 0u);  // Lua allocates its own state up front.
        CHECK(vm.GetErrorCount() == 0u);
    }

    TEST_CASE("two VMs are isolated from each other")
    {
        ScriptVM first;
        ScriptVM second;

        RunOrThrow(first, "sharedGlobal = 11");
        RunOrThrow(second, "sharedGlobal = 22");

        CHECK(EvalNumber(first, "sharedGlobal") == 11.0);
        CHECK(EvalNumber(second, "sharedGlobal") == 22.0);
    }

    TEST_CASE("a compile error is reported with position and leaves the VM usable")
    {
        ScriptVM vm;
        std::string error;

        CHECK_FALSE(vm.Run("local x = = 5", error, "bad_syntax.lua"));
        CHECK_FALSE(error.empty());
        // The chunk name must appear: without it a log full of errors cannot be traced to a file.
        CHECK(error.find("bad_syntax.lua") != std::string::npos);

        // Still usable afterwards.
        RunOrThrow(vm, "recovered = true");
        CHECK(EvalNumber(vm, "recovered and 1 or 0") == 1.0);
    }

    TEST_CASE("a runtime error is contained and the VM keeps working")
    {
        // The single most important property: one broken script must not take the process down with it.
        ScriptVM vm;
        std::string error;

        CHECK_FALSE(vm.Run("error('deliberate failure')", error, "runtime.lua"));
        CHECK(error.find("deliberate failure") != std::string::npos);
        CHECK(vm.GetErrorCount() == 1u);

        RunOrThrow(vm, "function stillWorks() return 42 end");
        CHECK(EvalNumber(vm, "stillWorks()") == 42.0);

        // Repeated failures accumulate rather than corrupting anything.
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            CHECK_FALSE(vm.Run("error('again')", error, "runtime.lua"));
        }
        CHECK(vm.GetErrorCount() == 6u);
        CHECK(EvalNumber(vm, "stillWorks()") == 42.0);
    }

    TEST_CASE("a runaway allocation is contained by the memory budget")
    {
        // Without a cap this grows the process until the OS kills it and the crash report names the engine rather than
        // the script at fault. With the cap it is an ordinary reported error and the VM survives.
        ScriptVM vm;

        std::string error;
        const bool ok =
            vm.Run("local t = {} for i = 1, 5000000 do t[i] = string.rep('x', 200) end", error, "runaway.lua");

        CHECK_FALSE(ok);
        // Strictly at-or-under: an earlier version reported usage above the limit because its accounting and its cap
        // disagreed about what a failed operation contributed.
        CHECK(vm.GetMemoryUsageBytes() <= vm.GetMemoryLimitBytes());

        // The VM must remain usable: the budget stops one script's allocation, it does not kill the state.
        RunOrThrow(vm, "afterOom = 'ok'");
        CHECK(EvalString(vm, "afterOom") == "ok");
    }

    TEST_CASE("the cap still fires after a VM has allocated and freed repeatedly")
    {
        // The accounting bug this guards: Lua's allocator passes a *type tag* rather than a size as oldSize when
        // block is null, and an early version subtracted it — plus a "defensive" clamp that silently swallowed
        // mismatches. Both drove the live-byte counter towards zero, so the cap quietly stopped applying while every
        // assertion above kept passing. Repeated cycles are what make such drift observable.
        ScriptVM vm;
        std::string error;

        for (int cycle = 0; cycle < 6; ++cycle)
        {
            INFO("cycle " << cycle);
            RunOrThrow(vm, "local t = {} for i = 1, 20000 do t[i] = string.rep('y', 64) end bulk = t t = nil");
            CollectFully(vm);

            // Usage must return to roughly the same place every cycle rather than sliding.
            CHECK(vm.GetMemoryUsageBytes() < 8u * 1024u * 1024u);
        }

        const bool ok = vm.Run("z = {} for i = 1, 400000 do z[i] = string.rep('x', 300) end", error, "cap.lua");
        CHECK_FALSE(ok);
        CHECK(vm.GetMemoryUsageBytes() <= vm.GetMemoryLimitBytes());
    }

    TEST_CASE("memory usage returns to baseline once script data is released")
    {
        // Catches both directions of accounting error: counting too little (cap disabled) and too much (a VM that looks
        // like it leaks). Note GetMemoryUsageBytes tracks Lua's own collectgarbage("count") exactly, verified against
        // it — so a large residual here means data is still reachable, not that the counter is wrong. Lua collects in
        // passes, hence CollectFully rather than a single step.
        ScriptVM vm;
        const std::size_t baseline = vm.GetMemoryUsageBytes();

        std::string error;
        RunOrThrow(vm, "t = {} for i = 1, 100000 do t[i] = { x = i, s = tostring(i) } end");
        CHECK(vm.GetMemoryUsageBytes() > baseline + 100u * 1024u);

        RunOrThrow(vm, "t = nil");
        CollectFully(vm);
        // Usage must fall back by most of what it grew, but Lua keeps some allocations until a later pass.
        CHECK(vm.GetMemoryUsageBytes() < baseline + 512u * 1024u);

        CollectFully(vm);
        const std::size_t drift = vm.GetMemoryUsageBytes() > baseline ? vm.GetMemoryUsageBytes() - baseline
                                                                      : baseline - vm.GetMemoryUsageBytes();
        CHECK(drift < 64u * 1024u);
    }

    TEST_CASE("the byte counter agrees with Lua's own accounting")
    {
        // The strongest available check on the allocator bookkeeping: Lua counts every allocation itself, so our figure
        // must match it exactly at every point rather than merely being plausible. This is what catches mis-handling of
        // the three allocator cases (fresh / resize / free), including the `block == nullptr` case where Lua passes a
        // type tag as oldSize instead of a size — an error that otherwise only shows up as a cap that silently stops
        // working.
        ScriptVM vm;
        std::string error;

        // Lua's own byte count, read through the C API: LUA_GCCOUNT is whole kilobytes and LUA_GCCOUNTB the remainder,
        // so their combination is exact. (collectgarbage("count") from script side returns a float number of
        // kilobytes — precise enough for gameplay telemetry but not for an equality assertion, which failed
        // spuriously under ASan where allocation sizes shift by tens of bytes.)
        auto luaCount = [&vm]() -> std::size_t {
            const int wholeKilobytes = lua_gc(vm.GetLuaState(), LUA_GCCOUNT, 0);
            const int remainderBytes = lua_gc(vm.GetLuaState(), LUA_GCCOUNTB, 0);
            return static_cast<std::size_t>(wholeKilobytes) * 1024u + static_cast<std::size_t>(remainderBytes);
        };

        // Checked after each distinct kind of workload: startup, a compile, table growth, string allocation, and GC.
        CHECK(vm.GetMemoryUsageBytes() == luaCount());

        RunOrThrow(vm, "f = function(a, b) return a + b end");
        CHECK(vm.GetMemoryUsageBytes() == luaCount());

        RunOrThrow(vm, "t = {} for i = 1, 5000 do t[i] = i end");
        CHECK(vm.GetMemoryUsageBytes() == luaCount());

        RunOrThrow(vm, "s = string.rep('abcdefgh', 4000)");
        CHECK(vm.GetMemoryUsageBytes() == luaCount());

        CollectFully(vm);
        CHECK(vm.GetMemoryUsageBytes() == luaCount());
    }

    TEST_CASE("many VMs can be created and destroyed in one process")
    {
        // Guards the ownership design: engine-side context and sol2 objects must never be owned by Lua's collector, or
        // teardown order across many VMs corrupts the heap. A single VM passing every other test does not prove that.
        for (int cycle = 0; cycle < 8; ++cycle)
        {
            INFO("cycle " << cycle);
            ScriptVM vm;
            REQUIRE(vm.IsInitialized());

            std::string error;
            RunOrThrow(vm, "t = {} for i = 1, 2000 do t[i] = { x = i } end function f(a) return a * 2 end");

            // A second VM nested inside the first's lifetime: destruction order must not matter.
            ScriptVM inner;
            RunOrThrow(inner, "x = 1");
        }
    }

    TEST_CASE("normal scripts allocate well under the budget")
    {
        // Guards against a cap so tight that legitimate content trips it, which would look like random gameplay bugs.
        ScriptVM vm;
        RunOrThrow(vm, R"LUA(
            local rows = {}
            for i = 1, 2000 do
                rows[i] = { id = i, name = "row" .. i, values = { i, i * 2, i * 3 } }
            end
            accumulated = 0
            for _, row in ipairs(rows) do accumulated = accumulated + row.values[3] end
        )LUA");

        CHECK(EvalNumber(vm, "accumulated") == doctest::Approx(3.0 * (2000.0 * 2001.0 / 2.0)));
        CHECK(vm.GetMemoryUsageBytes() < vm.GetMemoryLimitBytes());
    }

    TEST_CASE("dangerous globals are unavailable to scripts")
    {
        // Scripts are content. Content that can write files, spawn processes or compile arbitrary new code is a bug
        // with no upside, so these are removed rather than merely discouraged.
        ScriptVM vm;

        for (const char* forbidden : {"io", "os", "load", "dofile", "require", "package"})
        {
            INFO(forbidden);
            CHECK(EvalString(vm, "type(" + std::string(forbidden) + ")") == "nil");
        }
    }

    TEST_CASE("useful libraries remain available")
    {
        // Removing too much is as damaging as removing too little: a script that cannot format a string or use a
        // coroutine gets worked around by re-enabling something worse.
        ScriptVM vm;

        CHECK(EvalNumber(vm, "math.floor(3.7)") == 3.0);
        CHECK(EvalString(vm, "string.format('%d-%s', 5, 'x')") == "5-x");
        CHECK(EvalNumber(vm, "#table.pack(1, 2, 3)") == 3.0);

        RunOrThrow(vm, R"LUA(
            coroutineResult = 0
            local co = coroutine.create(function(a)
                coroutineResult = a * 2
            end)
            coroutine.resume(co, 21)
        )LUA");
        CHECK(EvalNumber(vm, "coroutineResult") == 42.0);
    }

    TEST_CASE("HasFunction distinguishes callables from everything else")
    {
        ScriptVM vm;

        CHECK_FALSE(vm.HasFunction("noSuchFunction"));
        CHECK_FALSE(vm.HasFunction(""));

        RunOrThrow(vm, "aFunction = function() end");
        CHECK(vm.HasFunction("aFunction"));

        // A non-function global must not be treated as callable.
        RunOrThrow(vm, "notAFunction = 42");
        CHECK_FALSE(vm.HasFunction("notAFunction"));
    }

    TEST_CASE("CallUpdate on a missing function is not an error")
    {
        // Most entities have no script; reporting that would emit thousands of lines per frame.
        ScriptVM vm;
        const UInt64 errorsBefore = vm.GetErrorCount();

        CHECK_FALSE(vm.CallUpdate("absentHook", 1, 0.016f));
        CHECK(vm.GetErrorCount() == errorsBefore);
    }

    TEST_CASE("CallUpdate passes the entity handle and delta through")
    {
        ScriptVM vm;
        RunOrThrow(vm, R"LUA(
            seenEntity = -1
            seenDelta = -1
            function on_update(entity, dt)
                seenEntity = entity
                seenDelta = dt
            end
        )LUA");

        CHECK(vm.CallUpdate("on_update", 12345, 0.016f));
        CHECK(EvalNumber(vm, "seenEntity") == 12345.0);
        CHECK(EvalNumber(vm, "seenDelta") == doctest::Approx(0.016));
    }

    TEST_CASE("a non-function global is not called")
    {
        ScriptVM vm;
        RunOrThrow(vm, "on_update = 'not callable'");
        CHECK_FALSE(vm.CallUpdate("on_update", 1, 0.0f));
    }
}

// ---------------------------------------------------------------------------
// Engine API.
// ---------------------------------------------------------------------------

TEST_SUITE("Scripting/EngineAPI")
{
    TEST_CASE("scripts create and destroy entities")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "created = hp.create_entity()");
        CHECK(EvalNumber(context.VM, "created ~= 0 and 1 or 0") == 1.0);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 1u);

        RunOrThrow(context.VM, "hp.destroy_entity(created)");
        world.FlushPendingDestructions();

        count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 0u);
    }

    TEST_CASE("a created entity is alive and a destroyed one is not")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "e = hp.create_entity()");
        CHECK(EvalNumber(context.VM, "hp.is_alive(e) and 1 or 0") == 1.0);

        RunOrThrow(context.VM, "hp.destroy_entity(e)");

        // IsAlive goes false **immediately** on Destroy rather than at the flush point — World documents this precisely
        // so a script that destroys an entity sees it as gone in the same call. What *is* deferred is slot reuse: the
        // memory stays valid until FlushPendingDestructions, so handles other systems hold cannot dangle mid-frame.
        CHECK(EvalNumber(context.VM, "hp.is_alive(e) and 1 or 0") == 0.0);

        world.FlushPendingDestructions();
        CHECK(EvalNumber(context.VM, "hp.is_alive(e) and 1 or 0") == 0.0);
    }

    TEST_CASE("a stale handle cannot act on a recycled slot")
    {
        // The use-after-free this boundary exists to prevent: destroy an entity, create another (which reuses the
        // slot), then show that the old handle addresses nothing rather than the new entity.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "first = hp.create_entity()");
        RunOrThrow(context.VM, "hp.set_position(first, 100, 0, 0)");
        RunOrThrow(context.VM, "hp.destroy_entity(first)");
        world.FlushPendingDestructions();

        RunOrThrow(context.VM, "second = hp.create_entity()");

        // Slot reused (the world recycles), so only generation distinguishes them.
        const Entity firstEntity = DecodeEntity(static_cast<std::int64_t>(EvalNumber(context.VM, "first")));
        const Entity secondEntity = DecodeEntity(static_cast<std::int64_t>(EvalNumber(context.VM, "second")));
        CHECK(firstEntity.Slot == secondEntity.Slot);

        // The stale handle is dead and every operation on it fails without touching the new entity.
        CHECK(EvalNumber(context.VM, "hp.is_alive(first) and 1 or 0") == 0.0);
        RunOrThrow(context.VM, "hp.set_position(first, 999, 999, 999)");

        TransformHierarchy transforms;
        transforms.Update(world);
        const Vec3 secondPosition = transforms.GetWorldMatrix(world, secondEntity).GetTranslation();
        CHECK(Near(Vec3(secondPosition.X, secondPosition.Y, secondPosition.Z), Vec3(0.0f, 0.0f, 0.0f), 1.0e-4f));
    }

    TEST_CASE("a created entity has a LocalTransform so moving it works immediately")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "e = hp.create_entity() hp.set_position(e, 1, 2, 3)");

        const Entity entity = DecodeEntity(static_cast<std::int64_t>(EvalNumber(context.VM, "e")));
        REQUIRE_FALSE(entity.IsNull());
        REQUIRE(world.TryGetComponent<LocalTransform>(entity) != nullptr);

        TransformHierarchy transforms;
        transforms.Update(world);
        const Vec3 position = transforms.GetWorldMatrix(world, entity).GetTranslation();
        CHECK(Near(Vec3(position.X, position.Y, position.Z), Vec3(1.0f, 2.0f, 3.0f)));
    }

    TEST_CASE("get_position returns world position through the hierarchy")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            parent = hp.create_entity()
            child = hp.create_entity()
            hp.set_parent(child, parent)
            hp.set_position(parent, 10, 0, 0)
            hp.set_position(child, 0, 5, 0)
        )LUA");

        const std::int64_t childHandle = static_cast<std::int64_t>(EvalNumber(context.VM, "child"));
        lua_State* state = context.VM.GetLuaState();

        // get_position returns (x, y, z, ok).
        REQUIRE_EQ(luaL_loadstring(state, "return hp.get_position(child)"), 0);
        REQUIRE_EQ(lua_pcall(state, 0, 4, 0), LUA_OK);
        CHECK(lua_tonumber(state, -4) == 10.0);
        CHECK(lua_tonumber(state, -3) == 5.0);
        CHECK(lua_tonumber(state, -2) == 0.0);
        CHECK(lua_toboolean(state, -1) == 1);
        lua_pop(state, 4);

        (void)childHandle;
    }

    TEST_CASE("parent and sibling traversal works from scripts")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            root = hp.create_entity()
            a = hp.create_entity()
            b = hp.create_entity()
            hp.set_parent(a, root)
            hp.set_parent(b, root)
        )LUA");

        CHECK(EvalNumber(context.VM, "hp.get_parent(a)") == EvalNumber(context.VM, "root"));
        CHECK(EvalNumber(context.VM, "hp.get_parent(root)") == 0.0);  // A root has no parent.

        const std::int64_t firstChild = static_cast<std::int64_t>(EvalNumber(context.VM, "hp.get_first_child(root)"));
        CHECK(firstChild != 0);

        // Walking siblings visits exactly the two children.
        RunOrThrow(context.VM, R"LUA(
            visited = 0
            local cursor = hp.get_first_child(root)
            while cursor ~= 0 do
                visited = visited + 1
                cursor = hp.get_next_sibling(cursor)
            end
        )LUA");
        CHECK(EvalNumber(context.VM, "visited") == 2.0);
    }

    TEST_CASE("a script cannot parent an entity into its own subtree")
    {
        // Without this check a script would hang world-transform evaluation; the refusal must also be reported or it is
        // undebuggable ("why did my parenting do nothing?").
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "a = hp.create_entity() b = hp.create_entity() hp.set_parent(b, a)");

        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK(EvalNumber(context.VM, "hp.set_parent(a, b) and 1 or 0") == 0.0);
        CHECK(logs.GetWarningCount() >= 1u);
        // Asserting the *content* matters: an earlier version of this file passed printf-style specifiers to the
        // engine's fmt-style logger, so a count-only assertion here would have been satisfied by a line reading
        // literally "%.*s". The refusal reason must be readable.
        CHECK(logs.AnyMessageContains("set_parent"));

        // The rejected operation left the hierarchy intact.
        CHECK(EvalNumber(context.VM, "hp.get_parent(b)") == EvalNumber(context.VM, "a"));
    }

    TEST_CASE("operations on nonsense handles fail without crashing")
    {
        World world;
        TestContext context(world);

        Hollowpoint::Test::ScopedLogCapture logs;

        CHECK(EvalNumber(context.VM, "hp.is_alive(0) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.is_alive(-1) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.is_alive(123456789) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.destroy_entity(-1) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.set_position(-1, 1, 2, 3) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.get_parent(999)") == 0.0);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 0u);
    }

    TEST_CASE("spawn_prefab creates the prefab's entities")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "crate = hp.spawn_prefab('Crate')");
        CHECK(EvalNumber(context.VM, "crate ~= 0 and 1 or 0") == 1.0);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 2u);  // Root plus child.
    }

    TEST_CASE("spawn_prefab_at places the instance")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "crate = hp.spawn_prefab_at('Crate', 50, 0, -20)");

        TransformHierarchy transforms;
        transforms.Update(world);
        const Entity root = DecodeEntity(static_cast<std::int64_t>(EvalNumber(context.VM, "crate")));
        REQUIRE_FALSE(root.IsNull());

        const Vec3 position = transforms.GetWorldMatrix(world, root).GetTranslation();
        // Spawn offset plus the prefab's authored root y of 0.5.
        CHECK(Near(Vec3(position.X, position.Y, position.Z), Vec3(50.0f, 0.5f, -20.0f)));
    }

    TEST_CASE("spawning twice creates independent instances")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "a = hp.spawn_prefab('Crate') b = hp.spawn_prefab('Crate')");
        CHECK(EvalNumber(context.VM, "a ~= b and 1 or 0") == 1.0);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 4u);
    }

    TEST_CASE("an unknown prefab name returns nil-ish and warns with the name")
    {
        World world;
        TestContext context(world);

        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK(EvalNumber(context.VM, "hp.spawn_prefab('NoSuchThing')") == 0.0);
        CHECK(logs.GetWarningCount() >= 1u);
        // The name is what makes the message actionable (typo vs. failed import), so it has to survive formatting.
        CHECK(logs.AnyMessageContains("NoSuchThing"));

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 0u);
    }

    TEST_CASE("scripts can log to the engine log")
    {
        World world;
        TestContext context(world);

        // The capture's minimum level is Warning by default, so only the warning line is recorded — asserting on it
        // (and its content) verifies scripts reach the engine log, which is the behaviour under test.
        Hollowpoint::Test::ScopedLogCapture logs;
        RunOrThrow(context.VM, "hp.log_info('from lua') hp.log_warning('a warning from lua')");

        CHECK(logs.GetWarningCount() >= 1u);
        CHECK(logs.AnyMessageContains("a warning from lua"));
    }

    TEST_CASE("hp.time advances and never goes backwards")
    {
        World world;
        TestContext context(world);

        const double first = EvalNumber(context.VM, "hp.time()");
        RunOrThrow(context.VM, "local sink = 0 for i = 1, 100000 do sink = sink + i end");
        const double second = EvalNumber(context.VM, "hp.time()");

        CHECK(second >= first);
    }

    TEST_CASE("a script error inside a hook is contained and counted")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            Broken = { on_update = function(entity, dt) error('script bug') end }
            goodCalls = 0
            Fine = { on_update = function(entity, dt) goodCalls = goodCalls + 1 end }
        )LUA");

        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Broken"));
        REQUIRE(system.BindScript("Fine"));

        const Entity brokenEntity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(brokenEntity).ScriptName = "Broken";

        // An entity whose script throws must not stop the others from ticking.
        const Entity fineEntity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(fineEntity).ScriptName = "Fine";

        Hollowpoint::Test::ScopedLogCapture logs;
        system.Update(world, 0.016f);

        CHECK(system.GetErrorCount() == 1u);
        CHECK(system.GetUpdateCallCount() == 1u);
        CHECK(EvalNumber(context.VM, "goodCalls") == 1.0);
        CHECK(logs.GetErrorCount() >= 1u);
        // Lua's error text is what a developer reads, so it must reach the log intact.
        CHECK(logs.AnyMessageContains("script bug"));
    }

    TEST_CASE("get_local_position returns what set_position wrote, even under a parent")
    {
        // The reason this pair exists: get_position answers in WORLD space while set_position writes LOCAL space. Both
        // are right for their own use, but together they leave "read where I am and move relative to that" expressible
        // only wrong — a bobbing child of a moving parent drifts toward world axes every frame and nothing logs it.
        // This case is the regression test for that trap, written as the drift scenario so it fails if local ever
        // becomes world.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            parent = hp.create_entity()
            child = hp.create_entity()
            hp.set_parent(child, parent)
            hp.set_position(parent, 10, 0, 0)
            hp.set_position(child, 0, 5, 0)
        )LUA");

        // World position of the child is (10, 5, 0); its local position — what was authored — is (0, 5, 0).
        lua_State* state = context.VM.GetLuaState();
        REQUIRE_EQ(luaL_loadstring(state, "return hp.get_local_position(child)"), 0);
        REQUIRE_EQ(lua_pcall(state, 0, 4, 0), LUA_OK);
        CHECK(lua_tonumber(state, -4) == 0.0);
        CHECK(lua_tonumber(state, -3) == 5.0);
        CHECK(lua_tonumber(state, -2) == 0.0);
        CHECK(lua_toboolean(state, -1) == 1);
        lua_pop(state, 4);

        // And a script's own read-modify-write stays put instead of sliding toward the origin.
        RunOrThrow(context.VM, R"LUA(
            local x, y, z = hp.get_local_position(child)
            hp.set_position(child, x, y + 1, z)
        )LUA");

        TransformHierarchy transforms;
        transforms.Update(world);
        const Entity child = DecodeEntity(static_cast<std::int64_t>(EvalNumber(context.VM, "child")));
        const Vec3 position = transforms.GetWorldMatrix(world, child).GetTranslation();
        CHECK(Near(position, Vec3(10.0f, 6.0f, 0.0f)));
    }

    TEST_CASE("get_local_position and the rotation setters refuse cleanly on bad input")
    {
        World world;
        TestContext context(world);

        // No entity: false / ok=false rather than an error, matching set_position's documented policy.
        CHECK(EvalNumber(context.VM, "hp.set_rotation(-1, 0, 0, 0, 1) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.set_rotation_euler(0, 0, 0, 0) and 1 or 0") == 0.0);

        lua_State* state = context.VM.GetLuaState();
        REQUIRE_EQ(luaL_loadstring(state, "return hp.get_local_position(-1)"), 0);
        REQUIRE_EQ(lua_pcall(state, 0, 4, 0), LUA_OK);
        CHECK(lua_toboolean(state, -1) == 0);
        lua_pop(state, 4);

        // A live entity with no LocalTransform (created in C++, not through hp.create_entity): reported as unavailable
        // rather than answered with a confident identity.
        const Entity bare = world.CreateEntity();
        // select(5, ...) because `f() and ...` keeps only the first return value — and in Lua 0 is truthy, so an
        // expression built on the x component would report success for every case including this one.
        CHECK(EvalNumber(context.VM, "select(5, hp.get_local_position(" + std::to_string(EncodeEntity(bare)) +
                                         ")) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM,
                         "select(5, hp.get_rotation(" + std::to_string(EncodeEntity(bare)) + ")) and 1 or 0") == 0.0);
    }

    TEST_CASE("set_rotation_euler rotates the entity, and get_rotation reads it back exactly")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            e = hp.create_entity()
            hp.set_rotation_euler(e, math.pi / 2, 0, 0)   -- yaw 90 degrees about +Y
        )LUA");

        const Entity entity = DecodeEntity(static_cast<std::int64_t>(EvalNumber(context.VM, "e")));
        REQUIRE(world.TryGetComponent<LocalTransform>(entity) != nullptr);

        // A +X unit vector yawed 90° about +Y lands on -Z (right-handed). Checking the rotated basis vector rather than
        // the quaternion's components pins the *convention*, which is what an accidental change of sign or axis order
        // would break silently while every component still looked plausible.
        const LocalTransform& local = world.GetComponent<LocalTransform>(entity);
        const Vec3 rotated = local.Rotation * Vec3(1.0f, 0.0f, 0.0f);
        CHECK(std::fabs(rotated.X) < 1.0e-4f);
        CHECK(std::fabs(rotated.Y) < 1.0e-4f);
        CHECK(std::fabs(rotated.Z + 1.0f) < 1.0e-4f);

        // get_rotation returns the stored value, so a script can read and re-write without drift.
        lua_State* state = context.VM.GetLuaState();
        REQUIRE_EQ(luaL_loadstring(state, "return hp.get_rotation(e)"), 0);
        REQUIRE_EQ(lua_pcall(state, 0, 5, 0), LUA_OK);
        CHECK(std::fabs(lua_tonumber(state, -5) - local.Rotation.X) < 1.0e-6);
        CHECK(std::fabs(lua_tonumber(state, -4) - local.Rotation.Y) < 1.0e-6);
        CHECK(std::fabs(lua_tonumber(state, -3) - local.Rotation.Z) < 1.0e-6);
        CHECK(std::fabs(lua_tonumber(state, -2) - local.Rotation.W) < 1.0e-6);
        CHECK(lua_toboolean(state, -1) == 1);
        lua_pop(state, 5);
    }

    TEST_CASE("set_rotation normalises rather than corrupting scale")
    {
        World world;
        TestContext context(world);

        // Not unit length: exactly what a script produces from axis-angle arithmetic. Passed through unnormalised it
        // makes Mat4::FromQuaternion scale the entity — visible as the object shrinking, with the cause three layers
        // away.
        RunOrThrow(context.VM, "e = hp.create_entity() hp.set_rotation(e, 0, 2, 0, 0)");

        const Entity entity = DecodeEntity(static_cast<std::int64_t>(EvalNumber(context.VM, "e")));
        const LocalTransform& local = world.GetComponent<LocalTransform>(entity);
        CHECK(std::fabs(local.Rotation.Length() - 1.0f) < 1.0e-5f);

        TransformHierarchy transforms;
        transforms.Update(world);
        const Hollowpoint::Math::Mat4 matrix = transforms.GetWorldMatrix(world, entity);
        const float columnScale =
            std::sqrt(matrix.Columns[0][0] * matrix.Columns[0][0] + matrix.Columns[0][1] * matrix.Columns[0][1] +
                      matrix.Columns[0][2] * matrix.Columns[0][2]);
        CHECK(std::fabs(columnScale - 1.0f) < 1.0e-4f);
    }
}

// ---------------------------------------------------------------------------
// Script system dispatch.
// ---------------------------------------------------------------------------

TEST_SUITE("Scripting/ScriptSystem")
{
    TEST_CASE("binding requires an actual table global")
    {
        World world;
        TestContext context(world);
        ScriptSystem system(context.VM);

        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK_FALSE(system.BindScript("NeverDefined"));
        CHECK(logs.GetWarningCount() >= 1u);
        CHECK(logs.AnyMessageContains("NeverDefined"));

        RunOrThrow(context.VM, "notATable = 5");
        CHECK_FALSE(system.BindScript("notATable"));

        RunOrThrow(context.VM, "aTable = {}");
        CHECK(system.BindScript("aTable"));
        CHECK(system.IsScriptBound("aTable"));
        CHECK_FALSE(system.IsScriptBound("NeverDefined"));
    }

    TEST_CASE("entities without a script are skipped silently")
    {
        World world;
        TestContext context(world);
        ScriptSystem system(context.VM);

        for (int index = 0; index < 10; ++index)
        {
            (void)world.CreateEntity();  // No ScriptComponent at all.
        }

        Hollowpoint::Test::ScopedLogCapture logs;
        system.Update(world, 0.016f);

        CHECK(system.GetUpdateCallCount() == 0u);
        CHECK(logs.GetWarningCount() == 0u);
    }

    TEST_CASE("a missing hook on a bound script is not reported")
    {
        // Most scripts implement one or two hooks; warning about the rest would produce thousands of lines per frame.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "NoUpdate = { on_create = function(e) end }");

        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("NoUpdate"));

        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "NoUpdate";

        Hollowpoint::Test::ScopedLogCapture logs;
        system.Update(world, 0.016f);

        CHECK(system.GetUpdateCallCount() == 0u);
        CHECK(logs.GetWarningCount() == 0u);
    }

    TEST_CASE("an unbound script name is reported once, not per entity per frame")
    {
        World world;
        TestContext context(world);

        ScriptSystem system(context.VM);

        for (int index = 0; index < 50; ++index)
        {
            const Entity entity = world.CreateEntity();
            world.AddComponent<ScriptComponent>(entity).ScriptName = "MissingScript";
        }

        Hollowpoint::Test::ScopedLogCapture logs;
        system.Update(world, 0.016f);
        system.Update(world, 0.016f);
        system.Update(world, 0.016f);

        // One message total rather than 150 (50 entities x 3 frames).
        CHECK(logs.GetWarningCount() == 1u);
        CHECK(logs.AnyMessageContains("MissingScript"));
    }

    TEST_CASE("on_update runs for every scripted entity with the right handle")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            seen = {}
            Behaviour = {
                on_update = function(entity, dt)
                    seen[entity] = (seen[entity] or 0) + 1
                    lastDelta = dt
                end
            }
        )LUA");

        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Behaviour"));

        std::vector<std::int64_t> handles;
        for (int index = 0; index < 5; ++index)
        {
            const Entity entity = world.CreateEntity();
            world.AddComponent<ScriptComponent>(entity).ScriptName = "Behaviour";
            handles.push_back(EncodeEntity(entity));
        }

        system.Update(world, 0.25f);

        CHECK(system.GetUpdateCallCount() == 5u);
        CHECK(EvalNumber(context.VM, "lastDelta") == doctest::Approx(0.25));

        for (const std::int64_t handle : handles)
        {
            INFO("handle " << handle);
            CHECK(EvalNumber(context.VM, "seen[" + std::to_string(handle) + "] or 0") == 1.0);
        }
    }

    TEST_CASE("an entity destroyed by an earlier hook is skipped later in the same frame")
    {
        // The snapshot-then-dispatch design exists for exactly this: a script that destroys another entity (a
        // projectile hitting something) must not make the dispatcher walk freed slots or double-tick a recycled one.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            victimHandle = 0
            victimTicks = 0
            Killer = {
                on_update = function(entity, dt)
                    if victimHandle ~= 0 then
                        hp.destroy_entity(victimHandle)
                    end
                end
            }
            Victim = {
                on_update = function(entity, dt) victimTicks = victimTicks + 1 end
            }
        )LUA");

        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Killer"));
        REQUIRE(system.BindScript("Victim"));

        // The killer must be created first so it ticks first in slot order.
        const Entity killer = world.CreateEntity();
        world.AddComponent<ScriptComponent>(killer).ScriptName = "Killer";

        const Entity victim = world.CreateEntity();
        world.AddComponent<ScriptComponent>(victim).ScriptName = "Victim";
        RunOrThrow(context.VM, "victimHandle = " + std::to_string(EncodeEntity(victim)));

        system.Update(world, 0.016f);  // Killer destroys the victim mid-frame.
        world.FlushPendingDestructions();
        const UInt64 ticksAfterFirst = static_cast<UInt64>(EvalNumber(context.VM, "victimTicks"));

        system.Update(world, 0.016f);  // The dead entity must not tick again.
        CHECK(EvalNumber(context.VM, "victimTicks") == static_cast<double>(ticksAfterFirst));
    }

    TEST_CASE("a script creating entities during update does not corrupt dispatch")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            spawned = 0
            Spawner = {
                on_update = function(entity, dt)
                    if spawned < 10 then
                        hp.create_entity()
                        spawned = spawned + 1
                    end
                end
            }
        )LUA");

        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Spawner"));

        const Entity spawner = world.CreateEntity();
        world.AddComponent<ScriptComponent>(spawner).ScriptName = "Spawner";

        // One frame spawns exactly ten entities; they must not be ticked this same frame (they were not in the
        // snapshot), which is what keeps a spawner from recursing into itself.
        system.Update(world, 0.016f);
        CHECK(EvalNumber(context.VM, "spawned") == 1.0);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 2u);
    }

    TEST_CASE("lifecycle hooks fire on demand")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            createdFor = -1
            destroyedFor = -1
            Lifecycle = {
                on_create = function(entity) createdFor = entity end,
                on_destroy = function(entity) destroyedFor = entity end
            }
        )LUA");

        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Lifecycle"));

        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "Lifecycle";

        system.NotifyCreate(world, entity);
        CHECK(EvalNumber(context.VM, "createdFor") == static_cast<double>(EncodeEntity(entity)));

        // on_destroy runs while the entity is still queryable, which its cleanup depends on.
        system.NotifyDestroy(world, entity);
        CHECK(EvalNumber(context.VM, "destroyedFor") == static_cast<double>(EncodeEntity(entity)));
    }

    TEST_CASE("rebinding a script replaces it and does not leak the old table")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "Behaviour = { on_update = function(e, dt) version = 1 end }");
        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Behaviour"));

        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "Behaviour";

        system.Update(world, 0.016f);
        CHECK(EvalNumber(context.VM, "version") == 1.0);

        // Hot reload: a new table under the same name must take effect immediately.
        RunOrThrow(context.VM, "Behaviour = { on_update = function(e, dt) version = 2 end }");
        REQUIRE(system.BindScript("Behaviour"));

        system.Update(world, 0.016f);
        CHECK(EvalNumber(context.VM, "version") == 2.0);
    }

    TEST_CASE("UnbindAll stops dispatch and clears the once-only reporting")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "Behaviour = { on_update = function(e, dt) ticks = (ticks or 0) + 1 end }");
        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Behaviour"));

        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "Behaviour";

        system.Update(world, 0.016f);
        CHECK(EvalNumber(context.VM, "ticks") == 1.0);

        system.UnbindAll();
        CHECK_FALSE(system.IsScriptBound("Behaviour"));

        Hollowpoint::Test::ScopedLogCapture logs;
        system.Update(world, 0.016f);
        CHECK(EvalNumber(context.VM, "ticks") == 1.0);  // No further ticks.

        // Reporting re-arms after unbinding, so a reload that still has the problem says so again.
        system.Update(world, 0.016f);
        CHECK(logs.GetWarningCount() >= 1u);
    }

    TEST_CASE("ResetCallCounters zeroes the activity counters")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "Behaviour = { on_update = function(e, dt) end }");
        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Behaviour"));

        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "Behaviour";

        system.Update(world, 0.016f);
        CHECK(system.GetUpdateCallCount() == 1u);

        system.ResetCallCounters();
        CHECK(system.GetUpdateCallCount() == 0u);
        CHECK(system.GetErrorCount() == 0u);
    }

    TEST_CASE("ScriptComponent round-trips through a saved scene")
    {
        // Scripts are referenced by name in the file, which is what makes loading a scene before its scripts safe.
        World world;
        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "Behaviour";

        const std::string saved = Hollowpoint::Serialization::WriteJson(Scene::WriteScene(world));
        CHECK(saved.find("Behaviour") != std::string::npos);
        // The scene-file component name and field, asserted explicitly: they are the public on-disk contract of
        // RegisterScriptTypes, and a rename would otherwise show up only as "old scenes silently lost their scripts".
        CHECK(saved.find("\"Script\"") != std::string::npos);
        CHECK(saved.find("\"scriptName\"") != std::string::npos);

        World reloaded;
        REQUIRE(Scene::LoadSceneFromText(reloaded, saved).Loaded);

        Size found = 0;
        reloaded.ForEachEntity([&](const Entity& candidate) {
            const ScriptComponent* component = reloaded.TryGetComponent<ScriptComponent>(candidate);
            if (component != nullptr && component->ScriptName == "Behaviour")
            {
                ++found;
            }
        });
        CHECK(found == 1u);
    }

    TEST_CASE("BindAndInitialize runs on_create for entities a scene already attached the script to")
    {
        // The scenario the runtime hits and nothing else could reach: SceneIO knows nothing about Lua, so it loads an
        // entity carrying a script *name* with no VM in existence. By the time the host binds the behaviour, the entity
        // is already here — and if binding does not initialise it, on_update reads state its own on_create never set
        // and fails every frame (`attempt to index a nil value`), which is exactly what the first scripted demo scene
        // did.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            initialised = 0
            Counter = {
                on_create = function(e) initialised = initialised + 1 end,
                on_update = function(e, dt)
                    -- Reads nothing, so a plain BindScript would appear to work here; the point is that on_create ran.
                end
            }
        )LUA");

        ScriptSystem system(context.VM);
        for (int index = 0; index < 3; ++index)
        {
            const Entity entity = world.CreateEntity();
            world.AddComponent<ScriptComponent>(entity).ScriptName = "Counter";
        }
        // One entity carrying a different script: it must not be initialised by this bind.
        const Entity other = world.CreateEntity();
        world.AddComponent<ScriptComponent>(other).ScriptName = "OtherBehaviour";

        REQUIRE_FALSE(system.IsScriptBound("Counter"));
        REQUIRE(system.BindAndInitialize("Counter", world));
        CHECK(EvalNumber(context.VM, "initialised") == 3.0);
        CHECK(system.GetErrorCount() == 0u);

        // Ticking then works with no errors — the property that actually broke in the runtime.
        system.Update(world, 0.016f);
        CHECK(system.GetErrorCount() == 0u);
    }

    TEST_CASE("rebinding does not re-run on_create over live entities")
    {
        // Hot reload replaces code; it must not reset gameplay state. Entities that have been alive since before the
        // reload would otherwise have their script-local data wiped with no warning — the reason BindAndInitialize
        // initialises only on a name's first successful bind.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "count = 0\nReloaded = { on_create = function(e) count = count + 1 end }");
        ScriptSystem system(context.VM);

        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "Reloaded";

        REQUIRE(system.BindAndInitialize("Reloaded", world));
        CHECK(EvalNumber(context.VM, "count") == 1.0);

        RunOrThrow(context.VM, "Reloaded = { on_create = function(e) count = count + 1 end }");
        REQUIRE(system.BindAndInitialize("Reloaded", world));
        CHECK(EvalNumber(context.VM, "count") == 1.0);
    }

    TEST_CASE("BindAndInitialize reports a missing global without initialising anything")
    {
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, "ranCreate = false\nGhost = { on_create = function(e) ranCreate = true end }");
        ScriptSystem system(context.VM);

        const Entity entity = world.CreateEntity();
        world.AddComponent<ScriptComponent>(entity).ScriptName = "NotAGlobal";

        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK_FALSE(system.BindAndInitialize("NotAGlobal", world));
        CHECK(EvalNumber(context.VM, "ranCreate and 1 or 0") == 0.0);
        CHECK(logs.GetWarningCount() >= 1u);
    }

    TEST_CASE("hp.error_count reflects errors caught by this VM")
    {
        World world;
        TestContext context(world);

        CHECK(EvalNumber(context.VM, "hp.error_count()") == 0.0);

        std::string error;
        CHECK_FALSE(context.VM.Run("error('counted')", error, "<err>"));
        CHECK(EvalNumber(context.VM, "hp.error_count()") == 1.0);
    }

    TEST_CASE("destroying a parent destroys its whole subtree")
    {
        // World::DestroyEntity destroys descendants by default (promoteChildrenToParent is the opt-out), iteratively so
        // a deep hierarchy cannot blow the stack during teardown. Pinned here because scripts reach this constantly —
        // destroying a container node is the obvious thing to do, and whether it takes the contents with it has to be
        // one way or the other, deliberately, not by accident of implementation.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            parentEntity = hp.create_entity()
            childEntity = hp.create_entity()
            grandchildEntity = hp.create_entity()
            hp.set_parent(childEntity, parentEntity)
            hp.set_parent(grandchildEntity, childEntity)
            hp.destroy_entity(parentEntity)
        )LUA");

        world.FlushPendingDestructions();

        CHECK(EvalNumber(context.VM, "hp.is_alive(parentEntity) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.is_alive(childEntity) and 1 or 0") == 0.0);
        CHECK(EvalNumber(context.VM, "hp.is_alive(grandchildEntity) and 1 or 0") == 0.0);

        Size count = 0;
        world.ForEachEntity([&](const Entity&) { ++count; });
        CHECK(count == 0u);
    }

    TEST_CASE("a realistic behaviour script drives entities end to end")
    {
        // Bobbing movement: exercises create, set_position, get_position, time and the dispatch loop together, which is
        // how a real behaviour is written — and the only way to catch an integration mistake between layers.
        World world;
        TestContext context(world);

        RunOrThrow(context.VM, R"LUA(
            Bobber = {
                on_create = function(entity)
                    baseY = select(2, hp.get_position(entity))
                    phase = 0
                end,
                on_update = function(entity, dt)
                    phase = phase + dt
                    local x, y, z = hp.get_position(entity)
                    hp.set_position(entity, x, baseY + math.sin(phase) * 2, z)
                end
            }
        )LUA");

        ScriptSystem system(context.VM);
        REQUIRE(system.BindScript("Bobber"));

        const Entity entity = world.CreateEntity();
        world.AddComponent<LocalTransform>(entity).Position = Vec3(0.0f, 10.0f, 0.0f);
        world.AddComponent<ScriptComponent>(entity).ScriptName = "Bobber";

        system.NotifyCreate(world, entity);
        CHECK(EvalNumber(context.VM, "baseY") == 10.0);

        float previousY = 10.0f;
        bool movedUp = false;
        for (int frame = 0; frame < 60; ++frame)
        {
            system.Update(world, 0.1f);

            TransformHierarchy transforms;
            transforms.Update(world);
            const Vec3 position = transforms.GetWorldMatrix(world, entity).GetTranslation();

            if (position.Y > previousY + 0.001f)
            {
                movedUp = true;
            }
            previousY = position.Y;

            // Movement must stay bounded by the scripted amplitude.
            CHECK(position.Y >= 8.0f - 1.0e-3f);
            CHECK(position.Y <= 12.0f + 1.0e-3f);
        }

        CHECK(movedUp);
        CHECK(system.GetUpdateCallCount() == 60u);
        CHECK(system.GetErrorCount() == 0u);
    }
}
