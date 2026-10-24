#include <mutex>
#include <string>

#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Game/RegisterGameTypes.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Renderer/Camera.h"
#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"

namespace Hollowpoint::Game
{

using Reflection::Registry;
using Renderer::CameraComponent;
using Renderer::LightComponent;
using Scene::ComponentFactory;

namespace
{

std::mutex g_RegistrationMutex;

[[nodiscard]] bool TypeIsUsable(Registry& registry, std::string_view typeName)
{
    // Both halves must hold: a type descriptor without a factory entry parses but cannot be instantiated on load,
    // and a factory entry without a descriptor has no fields to read. Checking only one would report success for a
    // half-registered component — which fails later, at load time, naming the scene file rather than this function.
    return registry.FindType(typeName) != nullptr && ComponentFactory::Get().IsRegistered(typeName);
}

}  // namespace

void RegisterGameTypes()
{
    const std::lock_guard<std::mutex> lock(g_RegistrationMutex);

    Registry& registry = Registry::Get();

    // Scene types first: a camera or light entity normally also carries a LocalTransform, and loading one requires
    // the transform component to be registered. RegisterSceneTypes is itself idempotent, so calling it unconditionally
    // is cheaper than duplicating its guard here — and gets us nothing new to get wrong.
    Scene::RegisterSceneTypes();

    // --- Enums by name -------------------------------------------------------
    // Serialised as strings (see ReflectionBridge's documented rule): a numeric enum reorders silently when someone
    // inserts an enumerator in the middle, which for CullMode would flip a saved mesh to culling the wrong faces.
    if (registry.FindEnum("MeshCullMode") == nullptr)
    {
        registry.RegisterEnum<MeshCullMode>("MeshCullMode")
            .Value("Back", MeshCullMode::Back)
            .Value("Front", MeshCullMode::Front)
            .Value("None", MeshCullMode::None);
    }

    if (registry.FindEnum("LightType") == nullptr)
    {
        registry.RegisterEnum<LightComponent::LightType>("LightType")
            .Value("Directional", LightComponent::LightType::Directional)
            .Value("Point", LightComponent::LightType::Point)
            .Value("Spot", LightComponent::LightType::Spot);
    }

    if (registry.FindEnum("ProjectionType") == nullptr)
    {
        registry.RegisterEnum<CameraComponent::ProjectionType>("ProjectionType")
            .Value("Perspective", CameraComponent::ProjectionType::Perspective)
            .Value("Orthographic", CameraComponent::ProjectionType::Orthographic);
    }

    // --- MeshRenderer --------------------------------------------------------
    if (registry.FindType("MeshRenderer") == nullptr)
    {
        registry.RegisterType<MeshRenderer>("MeshRenderer")
            .Field("meshPath", &MeshRenderer::MeshPath)
            .Field("baseColor", &MeshRenderer::BaseColor)
            .Field("roughness", &MeshRenderer::Roughness)
            .Field("metallic", &MeshRenderer::Metallic)
            .Field("culling", &MeshRenderer::Culling)
            .Field("castsShadows", &MeshRenderer::CastsShadows)
            .Field("visible", &MeshRenderer::Visible);
    }

    if (registry.FindType("Camera") == nullptr)
    {
        // Registered as "Camera" rather than "CameraComponent": scene files read better without the C++ type's suffix,
        // and the suffix carries no information an author wants ("component" is implied by it being in a component
        // list).
        registry.RegisterType<CameraComponent>("Camera")
            .Field("position", &CameraComponent::Position)
            .Field("target", &CameraComponent::Target)
            .Field("up", &CameraComponent::Up)
            .Field("projection", &CameraComponent::Projection)
            .Field("verticalFieldOfView", &CameraComponent::VerticalFieldOfViewRadians)
            .Field("orthographicHalfHeight", &CameraComponent::OrthographicHalfHeight)
            .Field("nearPlane", &CameraComponent::NearPlane)
            .Field("farPlane", &CameraComponent::FarPlane)
            .Field("exposure", &CameraComponent::Exposure)
            .Field("active", &CameraComponent::Active);
    }

    if (registry.FindType("Light") == nullptr)
    {
        registry.RegisterType<LightComponent>("Light")
            .Field("type", &LightComponent::Type)
            .Field("color", &LightComponent::Color)
            .Field("intensity", &LightComponent::Intensity)
            .Field("range", &LightComponent::Range)
            .Field("spotInnerAngle", &LightComponent::SpotInnerAngleRadians)
            .Field("spotOuterAngle", &LightComponent::SpotOuterAngleRadians)
            .Field("direction", &LightComponent::Direction)
            .Field("active", &LightComponent::Active);
    }

    ComponentFactory& factory = ComponentFactory::Get();
    if (!factory.IsRegistered("MeshRenderer"))
    {
        factory.Register<MeshRenderer>("MeshRenderer");
    }
    if (!factory.IsRegistered("Camera"))
    {
        factory.Register<CameraComponent>("Camera");
    }
    if (!factory.IsRegistered("Light"))
    {
        factory.Register<LightComponent>("Light");
    }
}

bool AreGameTypesRegistered()
{
    Registry& registry = Registry::Get();
    return TypeIsUsable(registry, "MeshRenderer") && TypeIsUsable(registry, "Camera") &&
           TypeIsUsable(registry, "Light");
}

}  // namespace Hollowpoint::Game
