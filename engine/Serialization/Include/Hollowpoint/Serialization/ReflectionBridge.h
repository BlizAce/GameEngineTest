#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Reflection/Descriptors.h"
#include "Hollowpoint/Serialization/JsonValue.h"

namespace Hollowpoint::Serialization
{

using Reflection::Size;
using Reflection::TypeDescriptor;

// ---------------------------------------------------------------------------
// Reflection-driven encode/decode: reflected object  <->  JsonValue.
//
// This is the piece that makes "declare a field once, get saving for free" true. The scene format, prefabs and
// (later) script-visible components all go through it, so field layout knowledge exists in exactly one place —
// which is the whole reason Reflection exists. A hand-written writer per component drifts from the struct within a
// few edits; this cannot, because it reads the descriptor the inspector reads.
//
// Deliberate behaviours, each chosen for how scene files actually fail in practice:
//
//   * **Field order follows declaration order.** Output is deterministic, so a saved scene diffs cleanly and an
//     asset hash over serialised data is stable.
//   * **Fields marked DontSerialize are skipped** on both paths — a cached pointer or accumulated timer written to
//     disk is either meaningless or actively wrong on load.
//   * **A JSON field the type does not have is reported and skipped**, not fatal. That is the normal signature of a
//     scene saved by a newer build (or a field renamed during a refactor), and refusing to load the level at all is
//     a far worse outcome than losing that one value with a clear message.
//   * **A field missing from JSON keeps its default-constructed value.** This is what makes adding a field to a
//     component safe for every existing scene file: no migration, no version bump, and the new field simply gets the
//     same value a freshly created component would have.
//   * **Enums are stored by name.** A numeric enum silently reorders when someone inserts an enumerator in the
//     middle — which is exactly what names prevent, and it makes scene files readable besides. An unknown name is an
//     error rather than a fallback to zero: "LightMode::Cookie" typo'd into a default mode is a bug nobody finds.
//   * **Numbers are clamped on write with the same rules as the inspector**, so loading a value and dragging a
//     slider cannot store different bytes for the same field (see ScalarAccess.h).
//
// Errors accumulate into a path-qualified list (`entities[3].components.Mesh.material.smoothness: ...`) rather than
// throwing or aborting at the first problem: a level with twelve bad fields should produce twelve messages in one
// pass, because fixing them is otherwise a twelve-run errand.
// ---------------------------------------------------------------------------

//! Collects diagnostics during a conversion. Non-empty means the result is partially applied and worth reporting.
struct ConversionDiagnostics
{
    std::vector<std::string> Messages;

    [[nodiscard]] bool HasErrors() const noexcept
    {
        return !Messages.empty();
    }

    void Add(std::string message)
    {
        Messages.push_back(std::move(message));
    }

    //! Logs every message through the engine logger and clears the list. Called once at the boundary by whoever
    //! owns the file being read, so callers do not each invent their own reporting.
    void ReportToLog(const char* context) const;
};

// Options shared by both directions.
struct ConversionOptions
{
    //! Maximum recursion depth for nested structs and arrays-of-structs. Reflected types can in principle be
    //! recursive (a node with a child list), so without a bound a pathological value or a descriptor cycle recurses
    //! until the stack is gone.
    Size MaxDepth = 64;

    //! Include fields flagged ReadOnly. They are serialised by default because "editable only by scripts" describes
    //! the inspector, not the save file: a scripted health value must survive a save or loading restores full health.
    bool IncludeReadOnlyFields = true;
};

//! Encodes a reflected object as a JSON object (members in declaration order).
//! Returns null when `type` is unusable, so callers can distinguish "failed" from "serialised to {}".
JsonValue TypeToJson(const void* object, const TypeDescriptor& type, ConversionDiagnostics& diagnostics,
                     const ConversionOptions& options = {});

//! Decodes a JSON object into a reflected object. Fields absent from `json` keep their current values, so pass a
//! default-constructed instance for a fresh load and an existing one to patch it in place (editor "apply changes").
//! Returns true when nothing was rejected; false means the object is partially filled and `diagnostics` explains it.
bool JsonToType(const JsonValue& json, void* object, const TypeDescriptor& type, ConversionDiagnostics& diagnostics,
                const ConversionOptions& options = {});

}  // namespace Hollowpoint::Serialization
