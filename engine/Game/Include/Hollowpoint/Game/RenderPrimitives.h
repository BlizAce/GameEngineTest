#pragma once

#include <cstddef>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/ECS/Entity.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Game/MeshRenderer.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/Geometry.h"

// ---------------------------------------------------------------------------
// Scene → renderer bridge: turn what an author placed into the flat lists the forward pass consumes.
//
// This is the seam between two modules that must not know about each other's abstractions. Renderer deliberately links
// neither ECS nor Graphics — it is a CPU shading reference plus a GLSL generator, and keeping it that way is what lets
// numeric equivalence tests run with no device and full leak checking. Scene knows about entities but nothing about
// draw calls. The types that need both live here, and the conversion is one-directional: scene data in, render plan
// out, never the reverse.
//
// Why a free function over an explicit entity list rather than a World-wide ForEach:
//   * Determinism. Iterating the ECS yields slot order, which changes when an entity is destroyed mid-session — so two
//     frames with identical content can produce different light ORDER, and with it different per-draw light indices.
//     That breaks any attempt to diff a frame against a reference or replay it. The caller supplies the order it wants
//     (scene load order, editor selection order, spatial sort) and gets exactly that back.
//   * Testability without an ECS: the collector's interesting behaviour is light-type handling, cull-mode mapping and
//     active/inactive filtering — none of which need a World to construct.
//   * It composes with what comes next: per-view culling and batching will filter this same input list before it
//     reaches the renderer, and neither needs an ECS in the middle to do it.
//
// The collector takes world transforms as parameters rather than reading LocalTransform itself, because placement is
// TransformHierarchy's job (it resolves parent chains) and duplicating that here would create a second, lazier source
// of truth about where things are.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Game
{

using Math::Float32;
using Math::Mat4;
using Math::Vec3;

//! Converts an authored cull mode to the graphics-layer value.
/**
 * Explicit by necessity, not ceremony: MeshCullMode and Graphics::CullMode are unrelated enum types whose enumerators
 * are ordered differently (the scene enum leads with Back because that is the default authors see; the graphics enum
 * leads with None). A static_cast between them would compile, and would silently cull the wrong faces on whichever
 * enumerator happened to disagree — a bug that reads as "some geometry vanished" hundreds of lines away.
 */
[[nodiscard]] HP_GAME_API Graphics::CullMode ToGraphicsCullMode(MeshCullMode mode) noexcept;

//! One entity's drawable, resolved to world space and ready for submission.
struct RenderMeshEntry
{
    //! Entity this came from, so a caller can map a pick or a culling decision back to the scene.
    ECS::Entity Owner{};

    Renderer::ForwardDrawItem Draw{};

    //! Authored cull mode, converted eagerly (see ToGraphicsCullMode). Draw state, not per-draw data: a renderer groups
    //! consecutive entries sharing it so one pipeline bind covers them all.
    Graphics::CullMode Culling = Graphics::CullMode::Back;

    //! Authored shadow flag, carried through because ForwardDrawItem has no such field yet — shadows land with their
    //! own pass and will read this rather than have it dropped on the floor here.
    bool CastsShadows = true;
};

//! The per-frame plan produced from a scene: everything the forward pass needs, in a form it already accepts.
struct RenderPlan
{
    Renderer::ForwardPassParams Pass{};
    std::vector<RenderMeshEntry> Meshes;
};

//! Inputs for one collection pass. Aggregate of references — nothing here is owned or copied beyond small values.
struct CollectInput
{
    //! Entity whose LocalTransform places the mesh. Looked up in `world` by the caller-supplied transform getter.
    ECS::World* World = nullptr;  // not owned

    //! Parallel to the entities the caller wants collected, in the order they should be submitted. Kept as explicit
    //! arrays rather than a map so ordering is unambiguous and iteration cost is one pass.
    const ECS::Entity* MeshEntities = nullptr;  // not owned; length = MeshCount
    const Mat4* MeshWorldTransforms = nullptr;  // not owned; may be null, meaning identity for every entry

    const ECS::Entity* LightEntities = nullptr;  // not owned; length = LightCount
    const Mat4* LightWorldTransforms = nullptr;  // not owned; may be null, meaning identity for every entry

    //! Number of entries in the mesh / light arrays. Zero is valid and produces an empty plan (not an error): a scene
    //! with no lights is a legitimate thing to render.
    Core::Size MeshCount = 0;
    Core::Size LightCount = 0;

    //! Camera position used for view-dependent shading terms, forwarded straight to ForwardPassParams.
    Vec3 CameraPosition{};

    //! Resolves an authored mesh path to geometry for submission, or nullptr when it cannot be loaded yet.
    /**
     * A callback rather than a direct dependency on an asset system: none exists yet, and the renderer needs
     * `const GeometryData*` now. Injecting it keeps this module free of a loader while still producing a plan whose
     * entries are genuinely drawable (ForwardDrawItem::IsDrawable() is true) instead of a list of half-filled items a
     * caller must finish by hand — which is how "the scene collected but nothing drew" bugs get written.
     *
     * Returning null is not an error: an asset that has not been imported, or a path typo, must drop one mesh while the
     * rest of the frame renders. It is logged, because a silently missing model is otherwise indistinguishable from a
     * culling bug.
     */
    using GeometryResolver = const Renderer::GeometryData* (*)(std::string_view path);
    GeometryResolver ResolveGeometry = nullptr;  //!< May be null, in which case no entry gets geometry assigned.
};

//! Builds the forward-pass plan from scene components.
/**
 * Behaviour worth stating because each rule was a design choice:
 *   * Inactive lights and non-drawable meshes are skipped, and skipping means the light list stays compact — indices
 *     in ForwardDrawItem::LightIndices index into CollectInput-resolved lights, so an off-by-one here would relight
 *     every object from the wrong lamp.
 *   * Every mesh references EVERY active light. Per-object light culling is a later optimisation; until it exists the
 *     honest behaviour is "all of them", and doing it through indices (rather than copies) keeps it cheap enough that
 *     adding culling later is a change to this function alone.
 *   * An entity in the light list that turns out to have no LightComponent is skipped and logged (channel GAME), not
 *     treated as a default light: guessing would turn an editor wiring bug into a mysterious lighting change. The same
 *     applies to mesh entities without a MeshRenderer, and to meshes whose path resolves to nothing — an entry with no
 *     geometry cannot be drawn, so it is dropped rather than emitted half-filled.
 *   * A light's world direction is its AUTHORED LightComponent::Direction rotated by the entity's rotation (scale
 *     stripped) — so rotating a sun in the editor moves the shadows, while an unrotated entity yields exactly the
 *     value the author typed. Point lights read only the translation, ignoring rotation entirely.
 */
[[nodiscard]] HP_GAME_API RenderPlan CollectRenderables(const CollectInput& input);

}  // namespace Hollowpoint::Game
