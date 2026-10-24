#pragma once

#include "Hollowpoint/Core/API.h"

// ---------------------------------------------------------------------------
// Registration of the engine's renderable scene components.
//
// Scene registers LocalTransform; this registers what makes an entity visible: MeshRenderer (defined here) plus the
// renderer's CameraComponent and LightComponent, which are plain value structs with no ECS dependency and so can be
// attached to entities directly. Registering them here rather than in Renderer keeps that module free of ECS — the
// reason its shading tests need no device — while still letting a scene file carry a camera or a lamp.
//
// Idempotent by querying the registry, not by a once-flag: SceneIO's latch pattern works there because nothing clears
// it, but ComponentFactory::Clear() exists (tests and hot-reload use it) and a latch that survives a clear reports
// "registered" for types that are gone. Checking the registry itself cannot go stale.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Game
{

//! Registers MeshRenderer, Camera and Light as serialisable scene components. Safe to call repeatedly.
HP_GAME_API void RegisterGameTypes();

//! False until RegisterGameTypes has run and its types are still present in the factory.
[[nodiscard]] HP_GAME_API bool AreGameTypesRegistered();

}  // namespace Hollowpoint::Game
