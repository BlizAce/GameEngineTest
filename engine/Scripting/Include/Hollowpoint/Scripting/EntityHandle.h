#pragma once

#include <cstdint>

#include "Hollowpoint/ECS/Entity.h"

namespace Hollowpoint::Scripting
{

// ---------------------------------------------------------------------------
// Entity handles across the scripting boundary.
//
// Lua sees an entity as a single signed 64-bit integer packing slot + generation. That encoding is what makes the
// scripting boundary safe without exposing any engine object:
//
//   * **A destroyed entity cannot be used.** Scripting keeps handles across frames; if the world recycles the slot for
//   a
//     new entity, the stale handle carries the old generation and decodes to "dead" instead of silently addressing an
//     unrelated entity. The classic symptom of getting this wrong — a script moving whatever object happens to reuse a
//     slot — is impossible rather than merely unlikely.
//   * **Nothing about engine memory leaks into Lua.** No pointers, no userdata, no finalizers: a script cannot hold a
//     reference that outlives the world or call a method on freed memory, because there is nothing to hold.
//   * **The nil entity is representable** as 0, so scripts can express "no entity" without a sentinel value that might
//     collide with a real handle. Slot 0 is never a live entity's first allocation in our world (InvalidIndex is used
//     internally), and the packing below keeps every valid handle non-zero regardless.
//
// Signed rather than unsigned because Lua 5.4 integers are signed 64-bit: pushing an unsigned value that exceeds
// INT64_MAX loses precision through a double, which would corrupt generations silently.
// ---------------------------------------------------------------------------

//! Encodes an entity as a script-visible handle. A null entity encodes to 0 ("no entity").
[[nodiscard]] std::int64_t EncodeEntity(const ECS::Entity& entity) noexcept;

//! Decodes a script-supplied handle. `entity` is left null when the handle does not decode to a well-formed entity, so
//! callers can distinguish "the script passed nonsense" from "the entity is gone".
//!
//! Note this validates only the *encoding*, not liveness: a handle for an entity destroyed long ago decodes fine and
//! must still be checked against the world. Keeping those separate means the check that matters (liveness) happens
//! against current state rather than against a snapshot taken when the handle was created.
[[nodiscard]] bool TryDecodeEntity(std::int64_t handle, ECS::Entity& outEntity) noexcept;

//! Convenience for call sites that want "handle or null".
[[nodiscard]] ECS::Entity DecodeEntity(std::int64_t handle) noexcept;

}  // namespace Hollowpoint::Scripting
