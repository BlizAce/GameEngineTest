// Tests for registering the renderable components with reflection and the scene factory.
//
// The point of these is round-tripping through an actual .hpscene document: a component that registers but does not
// survive save/load is worse than one that fails to register, because it silently drops author data. Enums are checked
// by NAME in the JSON (the bridge's documented rule — numeric enums reorder silently when an enumerator is inserted),
// and adding-a-field-must-not-break-old-files is checked by loading a document that omits fields entirely.

#include "TestSupport.h"

#include <string>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Game/RegisterGameTypes.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Serialization/JsonParser.h"
#include "Hollowpoint/Serialization/JsonWriter.h"

using Hollowpoint::ECS::Entity;
using Hollowpoint::ECS::World;
using Hollowpoint::Game::AreGameTypesRegistered;
using Hollowpoint::Game::MeshCullMode;
using Hollowpoint::Game::MeshRenderer;
using Hollowpoint::Game::RegisterGameTypes;
using Hollowpoint::Math::Vec3;
using Hollowpoint::Reflection::Registry;
using Hollowpoint::Renderer::CameraComponent;
using Hollowpoint::Renderer::LightComponent;
using Hollowpoint::Scene::ComponentFactory;
using Hollowpoint::Scene::LoadSceneFromText;
using Hollowpoint::Scene::LocalTransform;
using Hollowpoint::Scene::SceneLoadReport;
using Hollowpoint::Scene::WriteScene;

namespace
{

constexpr float kEpsilon = 1.0e-5f;

[[nodiscard]] bool Near(const Vec3& a, const Vec3& b, float epsilon = kEpsilon)
{
    return std::fabs(a.X - b.X) < epsilon && std::fabs(a.Y - b.Y) < epsilon && std::fabs(a.Z - b.Z) < epsilon;
}

//! Registration is process-global and idempotent, so one call from the first test that needs it suffices. Done lazily
//! rather than in a fixture because doctest runs cases in an order tests must not depend on.
void EnsureRegistered()
{
    RegisterGameTypes();  // Idempotent; calling every time also exercises the idempotency path.
}

[[nodiscard]] std::string SaveToString(const World& world)
{
    return Hollowpoint::Serialization::WriteJson(WriteScene(world));
}

}  // namespace

TEST_SUITE("Game/RegisterTypes")
{
    TEST_CASE("registration is idempotent and leaves every component usable")
    {
        EnsureRegistered();
        RegisterGameTypes();  // Twice explicitly: the second call must be a no-op, not a duplicate descriptor.

        CHECK(AreGameTypesRegistered());

        Registry& registry = Registry::Get();
        CHECK(registry.FindType("MeshRenderer") != nullptr);
        CHECK(registry.FindType("Camera") != nullptr);
        CHECK(registry.FindType("Light") != nullptr);

        ComponentFactory& factory = ComponentFactory::Get();
        CHECK(factory.IsRegistered("MeshRenderer"));
        CHECK(factory.IsRegistered("Camera"));
        CHECK(factory.IsRegistered("Light"));
    }

    TEST_CASE("the enum types are registered with every enumerator by name")
    {
        EnsureRegistered();

        Registry& registry = Registry::Get();

        const auto checkEnum = [&registry](const char* name, const std::vector<const char*>& values) {
            const auto* descriptor = registry.FindEnum(name);
            INFO(name);
            REQUIRE(descriptor != nullptr);

            Hollowpoint::Core::Int64 found = 0;
            for (const char* value : values)
            {
                INFO(value);
                CHECK(descriptor->FindValueByName(value, found));
            }
        };

        checkEnum("MeshCullMode", {"Back", "Front", "None"});
        checkEnum("LightType", {"Directional", "Point", "Spot"});
        checkEnum("ProjectionType", {"Perspective", "Orthographic"});
    }

    TEST_CASE("a MeshRenderer survives a save and load with every field")
    {
        EnsureRegistered();

        World source;
        const Entity entity = source.CreateEntity();
        LocalTransform transform;
        transform.Position = Vec3(1.0f, 2.0f, 3.0f);
        source.AddComponent<LocalTransform>(entity).SetFrom(transform.ToTransform());

        MeshRenderer renderer;
        renderer.MeshPath = "assets/meshes/sword.glb";
        renderer.BaseColor = Vec3(0.25f, 0.5f, 0.75f);
        renderer.Roughness = 0.125f;
        renderer.Metallic = 0.875f;
        renderer.Culling = MeshCullMode::None;
        renderer.CastsShadows = false;
        renderer.Visible = true;
        source.AddComponent<MeshRenderer>(entity) = renderer;

        const std::string text = SaveToString(source);

        // The enum must appear by NAME, not as an integer — the bridge documents this and it is the difference between
        // a scene file that survives reordering enumerators and one that silently changes culling.
        CHECK(text.find("\"None\"") != std::string::npos);
        CHECK(text.find("assets/meshes/sword.glb") != std::string::npos);

        World destination;
        const SceneLoadReport report = LoadSceneFromText(destination, text);
        report.ReportToLog("mesh round-trip");
        REQUIRE(report.Loaded);
        REQUIRE_FALSE(report.HasErrors());

        const MeshRenderer* loaded = nullptr;
        destination.ForEachEntity([&](const Entity& candidate) {
            if (const MeshRenderer* found = destination.TryGetComponent<MeshRenderer>(candidate))
            {
                loaded = found;
            }
        });
        REQUIRE(loaded != nullptr);

        CHECK(loaded->MeshPath == "assets/meshes/sword.glb");
        CHECK(Near(loaded->BaseColor, Vec3(0.25f, 0.5f, 0.75f)));
        CHECK(loaded->Roughness == doctest::Approx(0.125f));
        CHECK(loaded->Metallic == doctest::Approx(0.875f));
        CHECK(loaded->Culling == MeshCullMode::None);
        CHECK_FALSE(loaded->CastsShadows);
    }

    TEST_CASE("a Camera and a Light round-trip on the same entity set")
    {
        EnsureRegistered();

        World source;

        const Entity cameraEntity = source.CreateEntity();
        CameraComponent camera;
        camera.Position = Vec3(0.0f, 2.0f, 8.0f);
        camera.Target = Vec3(0.0f, 1.0f, 0.0f);
        camera.Projection = CameraComponent::ProjectionType::Orthographic;
        camera.VerticalFieldOfViewRadians = 0.75f;
        camera.OrthographicHalfHeight = 4.25f;
        camera.NearPlane = 0.05f;
        camera.FarPlane = 250.0f;
        camera.Exposure = 1.75f;
        source.AddComponent<CameraComponent>(cameraEntity) = camera;

        const Entity lightEntity = source.CreateEntity();
        LightComponent light;
        light.Type = LightComponent::LightType::Spot;
        light.Color = Vec3(1.0f, 0.5f, 0.25f);
        light.Intensity = 42.0f;
        light.Range = 12.5f;
        light.SpotInnerAngleRadians = 0.4f;
        light.SpotOuterAngleRadians = 0.6f;
        light.Direction = Vec3(0.0f, -1.0f, 0.0f);
        source.AddComponent<LightComponent>(lightEntity) = light;

        const std::string text = SaveToString(source);
        CHECK(text.find("\"Spot\"") != std::string::npos);
        CHECK(text.find("\"Orthographic\"") != std::string::npos);

        World destination;
        const SceneLoadReport report = LoadSceneFromText(destination, text);
        report.ReportToLog("camera and light round-trip");
        REQUIRE(report.Loaded);
        REQUIRE_FALSE(report.HasErrors());

        const CameraComponent* loadedCamera = nullptr;
        const LightComponent* loadedLight = nullptr;
        destination.ForEachEntity([&](const Entity& candidate) {
            if (const auto* found = destination.TryGetComponent<CameraComponent>(candidate))
            {
                loadedCamera = found;
            }
            if (const auto* found = destination.TryGetComponent<LightComponent>(candidate))
            {
                loadedLight = found;
            }
        });

        REQUIRE(loadedCamera != nullptr);
        CHECK(Near(loadedCamera->Position, Vec3(0.0f, 2.0f, 8.0f)));
        CHECK(Near(loadedCamera->Target, Vec3(0.0f, 1.0f, 0.0f)));
        CHECK(loadedCamera->Projection == CameraComponent::ProjectionType::Orthographic);
        CHECK(loadedCamera->OrthographicHalfHeight == doctest::Approx(4.25f));
        CHECK(loadedCamera->NearPlane == doctest::Approx(0.05f));
        CHECK(loadedCamera->FarPlane == doctest::Approx(250.0f));
        CHECK(loadedCamera->Exposure == doctest::Approx(1.75f));

        REQUIRE(loadedLight != nullptr);
        CHECK(loadedLight->Type == LightComponent::LightType::Spot);
        CHECK(Near(loadedLight->Color, Vec3(1.0f, 0.5f, 0.25f)));
        CHECK(loadedLight->Intensity == doctest::Approx(42.0f));
        CHECK(loadedLight->Range == doctest::Approx(12.5f));
        CHECK(Near(loadedLight->Direction, Vec3(0.0f, -1.0f, 0.0f)));
    }

    TEST_CASE("a document omitting newer fields loads with their defaults")
    {
        // The forward-compatibility property that makes adding a component field safe without a migration or version
        // bump: an old scene file must load, and the missing field must take the same value a fresh component has.
        EnsureRegistered();

        const std::string minimal = R"({
            "formatVersion": 1,
            "entities": [
                { "id": 0, "components": { "MeshRenderer": { "meshPath": "legacy.glb" } } }
            ]
        })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, minimal);
        report.ReportToLog("minimal mesh");
        REQUIRE(report.Loaded);
        CHECK_FALSE(report.HasErrors());

        const MeshRenderer* loaded = nullptr;
        world.ForEachEntity([&](const Entity& candidate) {
            if (const MeshRenderer* found = world.TryGetComponent<MeshRenderer>(candidate))
            {
                loaded = found;
            }
        });
        REQUIRE(loaded != nullptr);
        CHECK(loaded->MeshPath == "legacy.glb");

        // Every unspecified field matches a default-constructed component exactly.
        MeshRenderer defaults;
        CHECK(loaded->BaseColor == defaults.BaseColor);
        CHECK(loaded->Roughness == defaults.Roughness);
        CHECK(loaded->Metallic == defaults.Metallic);
        CHECK(loaded->Culling == defaults.Culling);
        CHECK(loaded->CastsShadows == defaults.CastsShadows);
        CHECK(loaded->Visible == defaults.Visible);
    }

    TEST_CASE("an unknown cull mode name is reported and leaves the field at its default")
    {
        EnsureRegistered();

        // The bridge validates enum NAMES on read (ReflectionBridge: "'X' is not a value of 'MeshCullMode'"), so an
        // unrecognised spelling cannot silently become enumerator zero. Scene's policy is that a failed field
        // conversion is a WARNING and the load continues — deliberate, so one typo in one component does not block a
        // whole level — which is why this asserts a warning rather than an error while still requiring the bad value to
        // have been named.
        const std::string bad = R"({
            "formatVersion": 1,
            "entities": [
                { "id": 0, "components": { "MeshRenderer": { "meshPath": "a.glb", "culling": "Sideways" } } }
            ]
        })";

        World world;
        const SceneLoadReport report = LoadSceneFromText(world, bad);

        // The load itself must succeed — a rejected component is not a rejected document.
        CHECK(report.Loaded);
        CHECK_FALSE(report.HasErrors());

        bool namedTheBadValue = false;
        for (const auto& warning : report.Warnings)
        {
            if (warning.Message.find("Sideways") != std::string::npos &&
                warning.Message.find("MeshCullMode") != std::string::npos)
            {
                namedTheBadValue = true;
            }
        }
        CHECK(namedTheBadValue);

        // And the field kept its default rather than taking some arbitrary value.
        const MeshRenderer* loaded = nullptr;
        world.ForEachEntity([&](const Entity& candidate) {
            if (const MeshRenderer* found = world.TryGetComponent<MeshRenderer>(candidate))
            {
                loaded = found;
            }
        });
        REQUIRE(loaded != nullptr);
        CHECK(loaded->Culling == MeshCullMode::Back);
        // The sibling field in the same object still applied: a rejected field must not abort the rest of its
        // component.
        CHECK(loaded->MeshPath == "a.glb");
    }

    TEST_CASE("scene documents still save deterministically after registration")
    {
        EnsureRegistered();

        World world;
        const Entity entity = world.CreateEntity();
        MeshRenderer renderer;
        renderer.MeshPath = "x.glb";
        world.AddComponent<MeshRenderer>(entity) = renderer;

        // Saving twice must produce identical bytes — the property that makes scene files diffable and asset hashes
        // over serialised data stable. Registration order or a map iteration in the writer could break it.
        CHECK(SaveToString(world) == SaveToString(world));
    }

    TEST_CASE("AreGameTypesRegistered reports false for an unregistered factory")
    {
        EnsureRegistered();
        REQUIRE(AreGameTypesRegistered());

        // ComponentFactory::Clear() is what hot-reload and tests use; the query must follow reality rather than a
        // once-flag. (Reflection descriptors are process-lifetime and cannot be cleared, so only the factory half can
        // flip — which is exactly why TypeIsUsable checks both.)
        ComponentFactory& factory = ComponentFactory::Get();
        const bool wasRegistered = factory.IsRegistered("MeshRenderer");
        REQUIRE(wasRegistered);

        factory.Clear();
        CHECK_FALSE(AreGameTypesRegistered());

        // Restore for any later test, and confirm re-registration works from the cleared state — the hot-reload path.
        RegisterGameTypes();
        CHECK(factory.IsRegistered("MeshRenderer"));
        CHECK(AreGameTypesRegistered());
    }
}
