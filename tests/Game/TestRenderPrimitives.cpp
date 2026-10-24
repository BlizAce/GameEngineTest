// Tests for the scene → renderer bridge: authored components in, a render plan out.
//
// These run with no GPU and no asset system — GeometryResolver is a function pointer precisely so a test can supply a
// table instead of an importer — which is the same property that makes the Renderer's CPU reference testable. The
// interesting behaviour is all conversion logic: cull-mode mapping, light filtering and rotation, and what happens when
// an entity's component or asset is missing.

#include "TestSupport.h"

#include <string>
#include <unordered_map>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Game/RenderPrimitives.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Renderer/Lights.h"

using Hollowpoint::Core::Size;
using Hollowpoint::Core::UInt32;
using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::World;
using Hollowpoint::Game::CollectInput;
using Hollowpoint::Game::MeshCullMode;
using Hollowpoint::Game::MeshRenderer;
using Hollowpoint::Game::RenderPlan;
using Hollowpoint::Game::ToGraphicsCullMode;
using Hollowpoint::Graphics::CullMode;
using Hollowpoint::Math::Mat4;
using Hollowpoint::Math::Vec3;
using Hollowpoint::Renderer::GeometryData;
using Hollowpoint::Renderer::LightComponent;

using namespace Hollowpoint::Game;

namespace
{

constexpr float kEpsilon = 1.0e-5f;

[[nodiscard]] bool Near(const Vec3& a, const Vec3& b, float epsilon = kEpsilon)
{
    return std::fabs(a.X - b.X) < epsilon && std::fabs(a.Y - b.Y) < epsilon && std::fabs(a.Z - b.Z) < epsilon;
}

[[nodiscard]] std::string Describe(const Vec3& v)
{
    return "(" + std::to_string(v.X) + ", " + std::to_string(v.Y) + ", " + std::to_string(v.Z) + ")";
}

//! A fixture standing in for the asset system: named geometries resolved by path.
struct FakeAssetTable
{
    std::unordered_map<std::string, GeometryData> Meshes;

    //! The resolver signature is a plain function pointer (no captures) so production code can point at an asset
    //! database without paying for std::function; tests route through this file-static pointer instead.
    static const GeometryData* Resolve(std::string_view path)
    {
        if (g_Table == nullptr)
        {
            return nullptr;
        }
        const auto found = g_Table->Meshes.find(std::string(path));
        return found != g_Table->Meshes.end() ? &found->second : nullptr;
    }

    static FakeAssetTable* g_Table;
};

FakeAssetTable* FakeAssetTable::g_Table = nullptr;

//! Installs a table for the duration of a test case and clears it afterwards, so a leaked pointer can never make a
//! later test resolve geometry it did not register.
class ScopedAssetTable
{
public:
    explicit ScopedAssetTable(FakeAssetTable& table)
    {
        FakeAssetTable::g_Table = &table;
    }

    ~ScopedAssetTable()
    {
        FakeAssetTable::g_Table = nullptr;
    }

    ScopedAssetTable(const ScopedAssetTable&) = delete;
    ScopedAssetTable& operator=(const ScopedAssetTable&) = delete;
};

[[nodiscard]] Entity MakeMeshEntity(World& world, const char* path)
{
    const Entity entity = world.CreateEntity();
    MeshRenderer renderer;
    renderer.MeshPath = path;
    world.AddComponent<MeshRenderer>(entity) = renderer;
    return entity;
}

[[nodiscard]] Entity MakeLightEntity(World& world, LightComponent::LightType type, const Vec3& direction)
{
    const Entity entity = world.CreateEntity();
    LightComponent light;
    light.Type = type;
    light.Direction = direction;
    world.AddComponent<LightComponent>(entity) = light;
    return entity;
}

}  // namespace

TEST_SUITE("Game/CullModeMapping")
{
    TEST_CASE("every authored cull mode maps to the matching graphics mode")
    {
        // The two enums are ordered differently (scene: Back, Front, None — graphics: None, Back, Front), so these
        // assertions fail loudly if anyone replaces ToGraphicsCullMode with a static_cast. Values spelled out rather
        // than derived from the enum's numeric order, which is exactly what changed underneath.
        CHECK(ToGraphicsCullMode(MeshCullMode::Back) == CullMode::Back);
        CHECK(ToGraphicsCullMode(MeshCullMode::Front) == CullMode::Front);
        CHECK(ToGraphicsCullMode(MeshCullMode::None) == CullMode::None);

        // Pinning the mismatch itself: if someone "fixes" one enum's order to match the other, this documents that the
        // mapping function is still correct (and could then be simplified deliberately rather than accidentally).
        CHECK(static_cast<unsigned>(MeshCullMode::Back) != static_cast<unsigned>(CullMode::Back));
    }

    TEST_CASE("an out-of-range cull mode falls back to Back, not to no culling")
    {
        // Reading past the enumerator range happens through memory corruption or a bad deserialise. Falling back to
        // None would rasterise every triangle of every mesh (slow but lossless); falling back to Back keeps geometry
        // correct for the overwhelmingly common single-sided case, which is the lesser harm. Pinned so the choice is
        // not silently reversed by someone reordering a default.
        // Both values are out of range for either enum, so these prove the fallback exists and is Back. They also
        // catch an implementation that forwards by cast rather than switching: measured under that mutation, the cast
        // preserves the input (200 comes back as 200) instead of falling through, so both assertions go red — a cast
        // does not clamp to a valid enumerator, which is worth knowing before assuming out-of-range input could only
        // ever be mishandled silently.
        CHECK(ToGraphicsCullMode(static_cast<MeshCullMode>(200)) == CullMode::Back);
        CHECK(ToGraphicsCullMode(static_cast<MeshCullMode>(3)) == CullMode::Back);
    }
}

TEST_SUITE("Game/RenderCollection")
{
    TEST_CASE("a visible mesh with resolvable geometry produces one drawable entry")
    {
        FakeAssetTable table;
        table.Meshes["a.cube"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity entity = MakeMeshEntity(world, "a.cube");

        CollectInput input;
        input.World = &world;
        input.MeshEntities = &entity;
        input.MeshCount = 1;
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Meshes.size() == 1u);

        // IsDrawable() is the renderer's own precondition — an entry that fails it would be silently skipped at draw
        // time, which is exactly the "collected fine, rendered nothing" bug this test exists to prevent.
        CHECK(plan.Meshes[0].Draw.IsDrawable());
        CHECK(plan.Meshes[0].Owner == entity);
        CHECK(plan.Meshes[0].Culling == CullMode::Back);
    }

    TEST_CASE("material parameters reach the draw item unchanged")
    {
        FakeAssetTable table;
        table.Meshes["m"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity entity = MakeMeshEntity(world, "m");
        MeshRenderer& renderer = world.AddComponent<MeshRenderer>(entity);  // already added; overwrite values below
        renderer.BaseColor = Vec3(0.25f, 0.5f, 0.75f);
        renderer.Roughness = 0.125f;
        renderer.Metallic = 0.875f;
        renderer.Culling = MeshCullMode::None;
        renderer.CastsShadows = false;

        CollectInput input;
        input.World = &world;
        input.MeshEntities = &entity;
        input.MeshCount = 1;
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Meshes.size() == 1u);

        // Linear values pass through with no conversion: a colour transform applied here would double-apply once a
        // texture is bound, so exactness is the contract.
        CHECK(Near(plan.Meshes[0].Draw.Material.BaseColor, Vec3(0.25f, 0.5f, 0.75f)));
        CHECK(plan.Meshes[0].Draw.Material.Roughness == doctest::Approx(0.125f));
        CHECK(plan.Meshes[0].Draw.Material.Metallic == doctest::Approx(0.875f));
        CHECK(plan.Meshes[0].Culling == CullMode::None);
        CHECK_FALSE(plan.Meshes[0].CastsShadows);
    }

    TEST_CASE("invisible and unassigned meshes are skipped")
    {
        FakeAssetTable table;
        table.Meshes["real"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity hidden = MakeMeshEntity(world, "real");
        world.AddComponent<MeshRenderer>(hidden).Visible = false;

        const Entity unassigned = MakeMeshEntity(world, "");  // nothing chosen yet — legal, not an error

        const Entity alive = MakeMeshEntity(world, "real");

        const std::vector<Entity> entities = {hidden, unassigned, alive};

        CollectInput input;
        input.World = &world;
        input.MeshEntities = entities.data();
        input.MeshCount = entities.size();
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Meshes.size() == 1u);
        CHECK(plan.Meshes[0].Owner == alive);
    }

    TEST_CASE("an unresolvable mesh path drops that entry and logs what it tried")
    {
        FakeAssetTable table;
        table.Meshes["good"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity good = MakeMeshEntity(world, "good");
        const Entity bad = MakeMeshEntity(world, "assets/typos.glb");

        const std::vector<Entity> entities = {bad, good};

        CollectInput input;
        input.World = &world;
        input.MeshEntities = entities.data();
        input.MeshCount = entities.size();
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        RenderPlan plan;
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            plan = CollectRenderables(input);
            // The path must appear in the log: a missing model is otherwise indistinguishable from a culling bug, and
            // this is the only place that knows which path it attempted.
            CHECK(logs.AnyMessageContains("assets/typos.glb"));
        }

        REQUIRE(plan.Meshes.size() == 1u);
        CHECK(plan.Meshes[0].Owner == good);
    }

    TEST_CASE("an empty geometry resolves to nothing drawable")
    {
        FakeAssetTable table;
        table.Meshes["empty"] = GeometryData{};  // imported a file with no primitives
        ScopedAssetTable install(table);

        World world;
        const Entity entity = MakeMeshEntity(world, "empty");

        CollectInput input;
        input.World = &world;
        input.MeshEntities = &entity;
        input.MeshCount = 1;
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        Hollowpoint::Test::ScopedLogCapture logs;
        const RenderPlan plan = CollectRenderables(input);
        // An empty mesh would draw nothing but still occupy a submission slot and a pipeline bind, so it is dropped.
        CHECK(plan.Meshes.empty());
    }

    TEST_CASE("no resolver configured means no meshes are emitted")
    {
        FakeAssetTable table;
        table.Meshes["x"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity entity = MakeMeshEntity(world, "x");

        CollectInput input;
        input.World = &world;
        input.MeshEntities = &entity;
        input.MeshCount = 1;
        input.ResolveGeometry = nullptr;  // asset system not wired yet

        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK(CollectRenderables(input).Meshes.empty());
    }

    TEST_CASE("an entity with no MeshRenderer is skipped rather than guessed at")
    {
        FakeAssetTable table;
        table.Meshes["x"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity bare = world.CreateEntity();  // in the mesh list but has no component (editor wiring bug)
        const Entity good = MakeMeshEntity(world, "x");

        const std::vector<Entity> entities = {bare, good};

        CollectInput input;
        input.World = &world;
        input.MeshEntities = entities.data();
        input.MeshCount = entities.size();
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        RenderPlan plan;
        {
            Hollowpoint::Test::ScopedLogCapture logs;
            plan = CollectRenderables(input);
            CHECK(logs.GetTotalCount() >= 1);
        }
        REQUIRE(plan.Meshes.size() == 1u);
        CHECK(plan.Meshes[0].Owner == good);
    }

    TEST_CASE("a dead entity in a cached list is skipped without asserting")
    {
        // Normal during teardown: the caller built its list last frame and an entity has since been destroyed. An
        // assertion here would crash on a legitimate frame boundary rather than dropping one entry.
        FakeAssetTable table;
        table.Meshes["x"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity doomed = MakeMeshEntity(world, "x");
        const Entity kept = MakeMeshEntity(world, "x");

        world.DestroyEntity(doomed);
        world.FlushPendingDestructions();

        const std::vector<Entity> entities = {doomed, kept};

        CollectInput input;
        input.World = &world;
        input.MeshEntities = entities.data();
        input.MeshCount = entities.size();
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Meshes.size() == 1u);
        CHECK(plan.Meshes[0].Owner == kept);
    }

    TEST_CASE("the world matrix becomes the draw item's model matrix")
    {
        FakeAssetTable table;
        table.Meshes["x"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity entity = MakeMeshEntity(world, "x");

        const Mat4 model = Mat4::Translate(Vec3(3.0f, -1.0f, 7.5f));
        const std::vector<Mat4> transforms = {model};

        CollectInput input;
        input.World = &world;
        input.MeshEntities = &entity;
        input.MeshWorldTransforms = transforms.data();
        input.MeshCount = 1;
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Meshes.size() == 1u);

        // Translation must land exactly where the caller put it — this is the value a shader multiplies vertex
        // positions by, so an identity here would render the whole scene at the origin.
        CHECK(Near(plan.Meshes[0].Draw.ModelMatrix.GetTranslation(), Vec3(3.0f, -1.0f, 7.5f)));
    }

    TEST_CASE("a null transform array means identity for every entry")
    {
        FakeAssetTable table;
        table.Meshes["x"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity first = MakeMeshEntity(world, "x");
        const Entity second = MakeMeshEntity(world, "x");
        const std::vector<Entity> entities = {first, second};

        CollectInput input;
        input.World = &world;
        input.MeshEntities = entities.data();
        input.MeshCount = entities.size();
        input.MeshWorldTransforms = nullptr;  // caller has no transforms to contribute
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Meshes.size() == 2u);
        CHECK(plan.Meshes[0].Draw.ModelMatrix == Mat4::Identity());
        CHECK(plan.Meshes[1].Draw.ModelMatrix == Mat4::Identity());
    }
}

TEST_SUITE("Game/LightCollection")
{
    TEST_CASE("active lights are collected in caller order with position and direction")
    {
        World world;
        const Entity sun = MakeLightEntity(world, LightComponent::LightType::Directional, Vec3(0.0f, -1.0f, 0.0f));
        const Entity lamp = MakeLightEntity(world, LightComponent::LightType::Point, Vec3(0.0f, -1.0f, 0.0f));

        const std::vector<Entity> lights = {sun, lamp};
        const std::vector<Mat4> transforms = {Mat4::Translate(Vec3(1.0f, 2.0f, 3.0f)),
                                              Mat4::Translate(Vec3(-4.0f, 0.5f, 0.0f))};

        CollectInput input;
        input.World = &world;
        input.LightEntities = lights.data();
        input.LightWorldTransforms = transforms.data();
        input.LightCount = lights.size();
        input.CameraPosition = Vec3(0.0f, 1.0f, 10.0f);

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Pass.Lights.size() == 2u);

        // Order preserved: ForwardDrawItem::LightIndices indexes this array, so reordering here relights objects from
        // the wrong lamp while every individual light still looks correct in isolation.
        CHECK(plan.Pass.Lights[0].Light.Type == LightComponent::LightType::Directional);
        CHECK(plan.Pass.Lights[1].Light.Type == LightComponent::LightType::Point);

        CHECK(Near(plan.Pass.Lights[0].WorldPosition, Vec3(1.0f, 2.0f, 3.0f)));
        CHECK(Near(plan.Pass.Lights[1].WorldPosition, Vec3(-4.0f, 0.5f, 0.0f)));

        // Unrotated entity: the world direction is exactly the authored value. This is the regression guard for an
        // earlier version that assumed local -Z, which silently turned a default down-pointing light into one aimed at
        // the horizon whenever the transform was identity — i.e. in every test written before this one.
        CHECK(Near(plan.Pass.Lights[0].WorldDirection, Vec3(0.0f, -1.0f, 0.0f)));

        CHECK(Near(plan.Pass.CameraPosition, Vec3(0.0f, 1.0f, 10.0f)));
    }

    TEST_CASE("an entity rotation rotates the light's authored direction")
    {
        World world;
        const Entity light = MakeLightEntity(world, LightComponent::LightType::Directional, Vec3(0.0f, -1.0f, 0.0f));

        // Rotate +90° about Z: the down axis (0,-1,0) maps to +X... verified against the matrix rather than asserted
        // from intuition, since a handedness slip here inverts every shadow in the scene.
        const Mat4 rotation = Mat4::Rotate(Vec3(0.0f, 0.0f, 1.0f), 1.5707963f);
        const Vec3 expected = rotation.GetRotationMatrix() * Vec3(0.0f, -1.0f, 0.0f);

        const std::vector<Entity> lights = {light};
        const std::vector<Mat4> transforms = {rotation};

        CollectInput input;
        input.World = &world;
        input.LightEntities = lights.data();
        input.LightWorldTransforms = transforms.data();
        input.LightCount = 1;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Pass.Lights.size() == 1u);
        INFO("expected " << Describe(expected) << " got " << Describe(plan.Pass.Lights[0].WorldDirection));
        CHECK(Near(plan.Pass.Lights[0].WorldDirection, expected));

        // And the rotation genuinely did something — an identity result would mean rotation was ignored.
        CHECK_FALSE(Near(plan.Pass.Lights[0].WorldDirection, Vec3(0.0f, -1.0f, 0.0f)));
    }

    TEST_CASE("a stretched light entity still aims where the author pointed it")
    {
        // Scale must not enter the direction: a designer scaling a light gizmo to see it better would otherwise swing
        // the beam, and a zero scale axis would produce a zero-length direction that normalises to garbage downstream.
        World world;
        const Entity light = MakeLightEntity(world, LightComponent::LightType::Spot, Vec3(0.0f, 0.0f, -1.0f));

        const std::vector<Entity> lights = {light};
        const std::vector<Mat4> transforms = {Mat4::Scale(Vec3(50.0f, 0.0f, 25.0f))};

        CollectInput input;
        input.World = &world;
        input.LightEntities = lights.data();
        input.LightWorldTransforms = transforms.data();
        input.LightCount = 1;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Pass.Lights.size() == 1u);
        CHECK(Near(plan.Pass.Lights[0].WorldDirection, Vec3(0.0f, 0.0f, -1.0f)));
    }

    TEST_CASE("inactive lights are excluded from the list entirely")
    {
        World world;
        const Entity on = MakeLightEntity(world, LightComponent::LightType::Point, Vec3(0.0f, -1.0f, 0.0f));
        const Entity off = MakeLightEntity(world, LightComponent::LightType::Point, Vec3(0.0f, -1.0f, 0.0f));
        world.AddComponent<LightComponent>(off).Active = false;

        const std::vector<Entity> lights = {on, off};

        CollectInput input;
        input.World = &world;
        input.LightEntities = lights.data();
        input.LightCount = lights.size();

        const RenderPlan plan = CollectRenderables(input);
        // Excluded rather than kept at zero intensity: an inactive light still costs shader work per fragment, and the
        // component documents that inactive means out of the list.
        REQUIRE(plan.Pass.Lights.size() == 1u);
    }

    TEST_CASE("every mesh references every active light by index")
    {
        FakeAssetTable table;
        table.Meshes["x"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        World world;
        const Entity meshA = MakeMeshEntity(world, "x");
        const Entity meshB = MakeMeshEntity(world, "x");
        const Entity lightOn = MakeLightEntity(world, LightComponent::LightType::Point, Vec3(0.0f, -1.0f, 0.0f));
        const Entity lightOff = MakeLightEntity(world, LightComponent::LightType::Point, Vec3(0.0f, -1.0f, 0.0f));
        world.AddComponent<LightComponent>(lightOff).Active = false;

        const std::vector<Entity> meshes = {meshA, meshB};
        const std::vector<Entity> lights = {lightOn, lightOff};

        CollectInput input;
        input.World = &world;
        input.MeshEntities = meshes.data();
        input.MeshCount = meshes.size();
        input.LightEntities = lights.data();
        input.LightCount = lights.size();
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        REQUIRE(plan.Meshes.size() == 2u);
        REQUIRE(plan.Pass.Lights.size() == 1u);

        // Indices must be dense over the COLLECTED lights: an index pointing past the end would read out of bounds in
        // the shader's light loop, and one that skipped the active light would render everything unlit.
        for (const auto& entry : plan.Meshes)
        {
            REQUIRE(entry.Draw.LightIndices.size() == 1u);
            CHECK(entry.Draw.LightIndices[0] == 0u);
        }
    }

    TEST_CASE("an empty scene produces an empty but valid plan")
    {
        World world;

        CollectInput input;
        input.World = &world;

        Hollowpoint::Test::ScopedLogCapture logs;
        const RenderPlan plan = CollectRenderables(input);
        CHECK(plan.Meshes.empty());
        CHECK(plan.Pass.Lights.empty());
        // Nothing is wrong with an empty scene, so nothing may be logged — a warning here would fire on every frame of
        // a booting editor.
        CHECK(logs.GetTotalCount() == 0);
    }

    TEST_CASE("a null world pointer collects nothing without dereferencing")
    {
        FakeAssetTable table;
        table.Meshes["x"] = Hollowpoint::Renderer::MakeCube();
        ScopedAssetTable install(table);

        Entity entity{};  // default-constructed, with no World to look it up in

        CollectInput input;
        input.World = nullptr;
        input.MeshEntities = &entity;
        input.MeshCount = 1;
        input.ResolveGeometry = &FakeAssetTable::Resolve;

        const RenderPlan plan = CollectRenderables(input);
        CHECK(plan.Meshes.empty());
    }
}

TEST_SUITE("Game/MeshRendererComponent")
{
    TEST_CASE("IsDrawable requires both visibility and an assigned mesh")
    {
        MeshRenderer renderer;
        // Default-constructed has no path: a fresh editor entity is not drawable, and must not be treated as an error.
        CHECK_FALSE(renderer.IsDrawable());

        renderer.MeshPath = "a.glb";
        CHECK(renderer.IsDrawable());

        renderer.Visible = false;
        CHECK_FALSE(renderer.IsDrawable());

        // Toggling visibility back must restore the mesh reference — hiding never clears the assignment.
        renderer.Visible = true;
        CHECK(renderer.IsDrawable());
        CHECK(renderer.MeshPath == "a.glb");
    }

    TEST_CASE("defaults are a sane single-sided lit surface")
    {
        MeshRenderer renderer;
        CHECK(renderer.Culling == MeshCullMode::Back);
        CHECK(renderer.Visible);
        CHECK(renderer.CastsShadows);
        // Linear albedo of exactly white: anything else would silently tint every un-textured imported mesh.
        CHECK(renderer.BaseColor == Vec3(1.0f, 1.0f, 1.0f));
    }
}
