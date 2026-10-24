#include <cstddef>
#include <string>
#include <vector>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/RenderPrimitives.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Renderer/Lights.h"

namespace Hollowpoint::Game
{

using Core::UInt32;
using ECS::Entity;
using ECS::World;
using Renderer::ForwardLight;
using Renderer::GeometryData;
using Renderer::LightComponent;

Graphics::CullMode ToGraphicsCullMode(MeshCullMode mode) noexcept
{
    // Exhaustive switch with no default: adding an enumerator to MeshCullMode without deciding its graphics meaning
    // fails the build here, which is the point. The two enums' values do not line up (Back is 0 on one side and 1 on
    // the other), so this cannot be a cast however tempting it looks — see the header.
    switch (mode)
    {
        case MeshCullMode::Back:
            return Graphics::CullMode::Back;
        case MeshCullMode::Front:
            return Graphics::CullMode::Front;
        case MeshCullMode::None:
            return Graphics::CullMode::None;
    }

    // Unreachable for a valid enumerator; reached only if something wrote an out-of-range value into the component
    // (corruption, or a deserialiser that bypassed enum-name validation). Back is the fallback because it is correct
    // for the overwhelmingly common single-sided mesh. None — cull nothing — would be the safer-sounding choice on
    // paper since it can never lose geometry, but it achieves that by rasterising both faces of everything, and doing
    // so for a value that arrives here by accident rather than by an author asking for double-sided rendering.
    return Graphics::CullMode::Back;
}

namespace
{

//! World position from a world matrix's translation column.
[[nodiscard]] Vec3 ExtractTranslation(const Mat4& worldMatrix) noexcept
{
    return Vec3(worldMatrix[3][0], worldMatrix[3][1], worldMatrix[3][2]);
}

//! Rotates a light's authored direction into world space.
/**
 * The component's own Direction field is the input, NOT an assumed local -Z axis. Getting this wrong is easy and
 * invisible: with an identity transform an assumed -Z silently turns a default light (authored Direction 0,-1,0 —
 * straight down) into one pointing at the horizon, so every test using default-constructed lights disagrees with the
 * editor. Rotating the authored value means "what you wrote is what you get" whenever the entity is unrotated, and a
 * sane rotation otherwise.
 *
 * Scale is stripped via GetRotationMatrix so a stretched light-gizmo cannot change where the light aims, nor produce a
 * zero-length direction from a collapsed axis. Not normalised: LightComponent::Direction documents that consumers do
 * that, and doing it here too would double-normalise against the CPU reference's own convention.
 */
[[nodiscard]] Vec3 RotateLightDirection(const Mat4& worldMatrix, const Vec3& authoredDirection) noexcept
{
    return worldMatrix.GetRotationMatrix() * authoredDirection;
}

//! Resolves one entry's world matrix, treating a missing array as identity — which is what "this entity has no
//! transform contribution" means, and better than forcing every caller to build an identity-filled array.
[[nodiscard]] Mat4 WorldMatrixAt(const Mat4* transforms, Core::Size index) noexcept
{
    return transforms != nullptr ? transforms[index] : Mat4::Identity();
}

}  // namespace

RenderPlan CollectRenderables(const CollectInput& input)
{
    RenderPlan plan;
    plan.Pass.CameraPosition = input.CameraPosition;

    // --- Lights first: meshes index into the resolved light list, so it must exist before any draw item is built. --
    if (input.LightCount > 0 && input.LightEntities != nullptr)
    {
        plan.Pass.Lights.reserve(input.LightCount);

        for (Core::Size index = 0; index < input.LightCount; ++index)
        {
            const Entity& entity = input.LightEntities[index];
            if (input.World == nullptr || !input.World->IsAlive(entity))
            {
                // A dead entity in a caller's cached list is normal during teardown (the list was built last frame);
                // skipping rather than asserting keeps collection safe against the frame boundary.
                continue;
            }

            const LightComponent* component = input.World->TryGetComponent<LightComponent>(entity);
            if (component == nullptr)
            {
                HP_LOG_WARNING("GAME", "light entity {} has no LightComponent; skipped", index);
                continue;
            }
            if (!component->Active)
            {
                // Skipped entirely rather than kept with zero intensity: a light that contributes nothing still costs
                // shader work per fragment, and the engine's stated contract is that inactive means out of the list.
                continue;
            }

            const Mat4 worldMatrix = WorldMatrixAt(input.LightWorldTransforms, index);

            ForwardLight entry;
            entry.Light = *component;
            entry.WorldPosition = ExtractTranslation(worldMatrix);
            entry.WorldDirection = RotateLightDirection(worldMatrix, component->Direction);
            plan.Pass.Lights.push_back(entry);
        }
    }

    // Every mesh lights from every active light: no per-object light culling yet, so this is the whole list each time.
    // The vectors hold indices into plan.Pass.Lights rather than copies of the lights themselves, which bounds the cost
    // of attaching one light to N meshes at N integers — but note the index vector itself is still copied per mesh
    // below, so a scene with many lights and many draw calls pays O(meshes × lights) here. That is the price of a plan
    // whose entries are self-contained; a shared/owned list is the fix if this ever shows up in a profile.
    std::vector<UInt32> allLightIndices;
    allLightIndices.reserve(plan.Pass.Lights.size());
    for (Core::Size lightIndex = 0; lightIndex < plan.Pass.Lights.size(); ++lightIndex)
    {
        allLightIndices.push_back(static_cast<UInt32>(lightIndex));
    }

    // --- Meshes -----------------------------------------------------------------------------------------------
    if (input.MeshCount > 0 && input.MeshEntities != nullptr)
    {
        plan.Meshes.reserve(input.MeshCount);

        for (Core::Size index = 0; index < input.MeshCount; ++index)
        {
            const Entity& entity = input.MeshEntities[index];
            if (input.World == nullptr || !input.World->IsAlive(entity))
            {
                continue;
            }

            const MeshRenderer* component = input.World->TryGetComponent<MeshRenderer>(entity);
            if (component == nullptr)
            {
                HP_LOG_WARNING("GAME", "mesh entity {} has no MeshRenderer; skipped", index);
                continue;
            }
            if (!component->IsDrawable())
            {
                // Invisible or unassigned: not an error, and deliberately not logged — this fires every frame for
                // every hidden object, and a per-frame log is how a useful channel becomes an unreadable one.
                continue;
            }

            const GeometryData* geometry =
                input.ResolveGeometry != nullptr ? input.ResolveGeometry(component->MeshPath) : nullptr;
            if (geometry == nullptr || geometry->IsEmpty())
            {
                // Logged once per collection rather than silently dropped: a missing model looks exactly like a culling
                // bug from the outside, and this is the only place that knows which path it tried.
                HP_LOG_WARNING("GAME", "mesh '{}' could not be resolved; entity skipped", component->MeshPath);
                continue;
            }

            RenderMeshEntry entry;
            entry.Owner = entity;
            entry.Draw.ModelMatrix = WorldMatrixAt(input.MeshWorldTransforms, index);
            entry.Draw.Material.BaseColor = component->BaseColor;
            entry.Draw.Material.Roughness = component->Roughness;
            entry.Draw.Material.Metallic = component->Metallic;
            entry.Draw.Geometry = geometry;  // not owned: the asset layer outlives the frame's plan
            entry.Draw.LightIndices = allLightIndices;
            entry.Culling = ToGraphicsCullMode(component->Culling);
            entry.CastsShadows = component->CastsShadows;

            plan.Meshes.push_back(entry);
        }
    }

    return plan;
}

}  // namespace Hollowpoint::Game
