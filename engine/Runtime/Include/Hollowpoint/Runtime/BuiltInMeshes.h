#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Game/RenderPrimitives.h"
#include "Hollowpoint/Renderer/Geometry.h"

// ---------------------------------------------------------------------------
// BuiltInMeshes — the engine's geometry source until an asset system exists.
//
// Game::CollectInput takes a GeometryResolver function pointer rather than a loader reference precisely so this seam
// could exist: RenderPrimitives.h says "a callback rather than a direct dependency on an asset system: none exists
// yet". This is that callback implemented against procedural primitives, which lets the runtime render a real scene
// with no importer, and keeps the difference between "the renderer is broken" and "the importer is broken" observable.
//
// Paths are `engine:cube`, `engine:plane` — a scheme prefix rather than a bare name, because these strings will one day
// live in saved scenes next to real asset paths (`assets/meshes/sword.glb`). A reserved prefix makes the two
// distinguishable at parse time, so an old scene still resolves after the importer lands and a mistyped file path
// produces "unknown scheme" rather than silently drawing a cube.
//
// Ownership: instances hold their meshes by value and hand out stable pointers, which is what ForwardDrawItem::Geometry
// (a non-owning `const GeometryData*`) requires. A resolver returning a pointer to a temporary would be a
// use-after-free that renders plausibly — the geometry cache in SceneRenderer keeps the address hot, so it would not
// even fault. Hence: stored, never minted per call.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Runtime
{

using Core::Size;
using Core::UInt32;

//! The scheme prefix identifying built-in meshes. Public because a scene-loading layer has to test for it to decide
//! whether a path is procedural or a file.
constexpr std::string_view kBuiltInScheme = "engine:";

//! True when `path` names a built-in mesh (starts with the reserved scheme).
[[nodiscard]] constexpr bool IsBuiltInPath(std::string_view path) noexcept
{
    return path.size() > kBuiltInScheme.size() && path.compare(0, kBuiltInScheme.size(), kBuiltInScheme) == 0;
}

//! A registry of procedural meshes, addressable by `engine:<name>`.
/**
 * Thread model: single-threaded. Registration and lookup both touch an unsynchronised map; a loader thread would need
 * one registry per thread or a lock, and nothing does that yet.
 *
 * Lifetime contract for Lookup(): the returned pointer is valid until the entry with that path is replaced or the
 * registry dies. Callers (the renderer's cache) rely on that stability, which is why Replace() below re-generates in
 * place rather than rehashing the map — see its comment.
 */
class HP_RUNTIME_API BuiltInMeshes
{
public:
    //! The default set every runtime gets: `engine:cube` and `engine:plane`, plus `engine:cube-large` used by tests to
    //! prove two paths can resolve to different geometry. Registered by RegisterDefaults(), which the runtime calls at
    //! startup.
    BuiltInMeshes();

    //! Registers (or replaces) a mesh under `engine:<name>`. Returns false when the name is empty or contains the
    //! scheme separator again, since such a path could not be parsed back unambiguously.
    bool Register(std::string_view name, const Renderer::GeometryData& geometry);

    //! Resolves a full path (`engine:cube`), or nullptr when the scheme is wrong or the name is unknown. Returning null
    //! is Game's documented "asset not available yet" signal — logged by the collector, dropping one mesh while the
    //! frame draws.
    [[nodiscard]] const Renderer::GeometryData* Lookup(std::string_view path) const noexcept;

    //! True when `path` resolves. Used by tests and by a startup check that every mesh a scene references exists.
    [[nodiscard]] bool Contains(std::string_view path) const noexcept;

    [[nodiscard]] Size GetCount() const noexcept;

    //! Replaces the default procedural set (cube, plane) with this registry's contents. Returns a pointer to the
    //! singleton so the C-function-pointer GeometryResolver can reach it without a capture — the seam takes a bare
    //! function, which cannot close over anything.
    [[nodiscard]] static BuiltInMeshes& GetSingleton();

    //! The resolver Game::CollectInput calls: resolves against the singleton. A free function pointer, matching the
    //! seam's signature exactly (no lambda, no std::function — those are not convertible to a bare function pointer).
    [[nodiscard]] static const Renderer::GeometryData* Resolve(std::string_view path) noexcept;

private:
    //! Stored by value so addresses stay stable across lookups. Note this is an unordered_map of values: inserting a
    //! *new* key can rehash and move existing elements, invalidating pointers handed out earlier. That is acceptable
    //! only because registration happens at startup, before any frame resolves geometry; Replace() on an existing key
    //! does not relocate. Stated rather than assumed, because the failure mode (a mesh silently changing identity
    //! mid-session) is nasty.
    std::unordered_map<std::string, Renderer::GeometryData> m_Meshes;
};

}  // namespace Hollowpoint::Runtime
