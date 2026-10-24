#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// MeshRenderer — the component that makes an entity drawable.
//
// It references a mesh by ASSET PATH rather than by Guid or pointer, and that choice is worth arguing: a Guid would be
// nicer (rename-proof, hashable, already the engine's identity type for anything serialized), but there is no asset
// database yet to resolve one against, and inventing Guid plumbing before the loader exists means inventing its
// allocation policy too — an expensive-to-undo decision this project defers. A path string loads today with no
// registry (the importer can open it), stays readable in a hand-edited .hpscene, and degrades to "file not found"
// rather than "unknown asset 0x8a31…" when something is missing. When the asset database lands, this field's type
// changes and its name does not.
//
// Paths are resolved by whoever loads, against whatever base it chooses; nothing here resolves relative to the scene
// file, because an entity detached from its scene (a prefab instance, a script-spawned clone) has no answer to "which
// file did you come from". The collector reports the path verbatim so a failed load names exactly what it tried.
//
// Material parameters live inline rather than behind a material-asset pointer for the same reason in miniature: glTF
// materials arrive as a handful of PBR scalars plus texture references, and an inline block round-trips through a scene
// file with no indirection to fail. A material asset system can replace this with a reference plus overrides without
// touching anything that consumes MeshRenderer.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Game
{

using Math::Float32;
using Math::Vec3;

//! How a mesh's triangles should be culled. Mirrors the renderer's culling modes but is deliberately a separate type:
//! this header describes what an author sets in a scene and must not drag RHI enums into everything that includes it.
enum class MeshCullMode : unsigned char
{
    Back,   //!< Single-sided geometry (the default; halves fragment work versus None).
    Front,  //!< Unusual, but needed for inside-out authored meshes and cheap normals-debug views.
    None,   //!< Double-sided: leaves, cards, anything viewed from both faces.
};

//! An entity's drawable: which mesh, which material, and how to cull it.
struct HP_GAME_API MeshRenderer
{
    //! Mesh asset reference — a file path relative to the engine's working directory (see file header), e.g.
    //! "assets/meshes/sword.glb". Empty means "nothing assigned yet" (a fresh editor entity, or one awaiting its mesh
    //! from a script) and is skipped by collection rather than treated as an error.
    std::string MeshPath;

    //! Linear-space albedo, multiplied with any texture the material ends up sampling. Not sRGB: storing a gamma value
    //! here would double-apply the transfer function once a texture is bound.
    Vec3 BaseColor{1.0f, 1.0f, 1.0f};

    //! Perceptual roughness and metallic, both 0..1, in the same convention as the renderer's PBR parameters (and
    //! glTF's), so an imported asset needs no conversion at this layer.
    Float32 Roughness = 0.5f;
    Float32 Metallic = 0.0f;

    MeshCullMode Culling = MeshCullMode::Back;

    //! Cast into the scene's shadow maps once shadows exist. Per-renderer rather than global because thin double-sided
    //! geometry (the Culling::None case) self-shadows badly and wants out.
    bool CastsShadows = true;

    //! Visible at all. Distinct from an empty MeshPath so toggling visibility in the editor never discards the mesh
    //! reference — matching CameraComponent::Active and LightComponent::Active.
    bool Visible = true;

    //! True when this component should contribute geometry: visible, with something assigned to draw.
    [[nodiscard]] bool IsDrawable() const noexcept
    {
        return Visible && !MeshPath.empty();
    }
};

}  // namespace Hollowpoint::Game
