#pragma once

#include <string_view>

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// Reflection descriptors for the engine's math types.
//
// Why this is a separate module rather than part of Math or of Reflection:
//
//   * Math must not depend on Reflection — it is the lowest-level engine module and gets linked into tools, tests and
//     benchmarks that have no interest in metadata. Adding the dependency would drag Reflection everywhere.
//   * Reflection must not know about Math either: it is a generic type-description facility, and baking engine types
//     into it means every user of Reflection carries them.
//   * So the two meet in an explicitly opt-in layer, exactly like SerializationReflection. Anything that needs to
//     serialise or inspect a Vec3 links this; anything else does not pay for it.
//
// The names are stable and lowercase-dotted ("Math.Vec3") because they appear in scene files: "Vec3" would collide with
// a user component of the same name, and a collision in the type registry produces confusing load errors far from the
// cause. Prefixing costs nothing at read time and removes that whole class of problem.
// ---------------------------------------------------------------------------

//! Registers every math type this build exposes to reflection. Idempotent: calling it repeatedly (or racing two
//! threads over it) leaves exactly one registration per type.
//!
//! Called automatically by the engine's startup path, and available for tools/tests that link only part of the engine
//! and need descriptors without pulling in full initialisation.
void RegisterMathTypes();

//! True once RegisterMathTypes has completed.
bool AreMathTypesRegistered();

//! Canonical registry name for a math type, used by scene files and the inspector.
//! Kept here so a component's reflected field type and its serialised name cannot drift apart.
const char* GetVec2TypeName() noexcept;
const char* GetVec3TypeName() noexcept;
const char* GetVec4TypeName() noexcept;
const char* GetQuatTypeName() noexcept;
const char* GetColorTypeName() noexcept;

}  // namespace Hollowpoint::Math
