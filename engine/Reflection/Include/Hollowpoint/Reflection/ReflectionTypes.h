#pragma once

#include <cstddef>
#include <string_view>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Hash.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Reflection
{

// Core's fixed-width aliases, re-exported so reflection code reads without a namespace qualifier on every
// line (same convention as the Math module).
using Core::HashValue;
using Core::Index;
using Core::Int16;
using Core::Int32;
using Core::Int64;
using Core::Int8;
using Core::Size;
using Core::UInt16;
using Core::UInt32;
using Core::UInt64;
using Core::UInt8;

// ---------------------------------------------------------------------------
// Reflection — runtime descriptions of engine types.
//
// Why this exists: the scene format and the editor inspector both need to read and write members of
// arbitrary component types without knowing which components they are. The alternative — a hand-written
// JSON writer and UI widget per component — duplicates layout knowledge in several places and drifts.
// With a registry, a type declares its fields once and gains saving, loading, an inspector and scripting
// bindings from the same source of truth.
//
// Constraints that shaped this design:
//   * Reading is pointer arithmetic plus a typed load/store — no allocation, no exceptions, so an
//     inspector redraw at 60 Hz stays cheap.
//   * Registration order must not matter: a field's struct/array element type is resolved lazily by name
//     on first use, so `Light` may be registered before `Color` is described.
//   * Descriptors are immutable after registration and live for the process lifetime; they are referenced
//     by pointer from anywhere. A type can never be un-registered while a scene still names it.
//   * Only data is reflected, never behaviour. Methods belong in the class or an explicit API facade.
// ---------------------------------------------------------------------------

// The shape of a value as stored in memory. Deliberately a closed set: a type that cannot be described
// by this list gets an explicit serializer hook rather than a reflection hack.
enum class ValueKind : UInt8
{
    Invalid = 0,

    Boolean,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float32,
    Float64,

    String,  //!< std::string.
    Enum,    //!< Scoped enum; the name is stored so scene files survive value reordering.
    Struct,  //!< Another reflected type, traversed recursively.
    Array    //!< std::vector<T> of a reflectable element type.
};

[[nodiscard]] HP_REFLECTION_API const char* ValueKindName(ValueKind kind);

// A scalar exchanged with the inspector and the serializer. Integers keep their signedness so a round
// trip never changes width; floats widen to double rather than losing precision at this layer.
struct ScalarValue
{
    ValueKind Kind = ValueKind::Invalid;

    bool AsBool = false;
    Int64 AsInt = 0;
    UInt64 AsUnsigned = 0;
    double AsReal = 0.0;

    [[nodiscard]] static ScalarValue FromBool(bool value);
    [[nodiscard]] static ScalarValue FromSigned(ValueKind kind, Int64 value);
    [[nodiscard]] static ScalarValue FromUnsigned(ValueKind kind, UInt64 value);
    [[nodiscard]] static ScalarValue FromReal(ValueKind kind, double value);

    //! True when `kind` is one of the numeric kinds (integer or floating point).
    [[nodiscard]] static bool IsNumericKind(ValueKind kind);
    [[nodiscard]] static bool IsIntegerKind(ValueKind kind);
};

// Authoring metadata for one field, consumed by the editor inspector. Everything is optional; the
// defaults mean "no constraint, plain widget". String members must point to static storage because
// descriptors are built at startup and never own memory.
struct FieldAttributes
{
    const char* DisplayName = nullptr;  //!< Overrides the field name in the UI.
    const char* Tooltip = nullptr;
    const char* Unit = nullptr;  //!< e.g. "kg", "m", "deg" — appended to labels.

    bool HasMin = false;
    bool HasMax = false;
    double Min = 0.0;
    double Max = 0.0;

    bool HasStep = false;
    double Step = 0.0;

    UInt32 Flags = 0;  //!< Bitwise OR of FieldFlag values.
};

enum FieldFlag : UInt32
{
    FieldFlag_None = 0,
    FieldFlag_DontSerialize = 1u << 0,  //!< Runtime-only state (cached pointers, accumulated timers).
    FieldFlag_ReadOnly = 1u << 1,       //!< Editable by scripts, not in the inspector.
    FieldFlag_Multiline = 1u << 2,      //!< Strings should use a multi-line editor.
    FieldFlag_NoSpinDrag = 1u << 3,     //!< Numeric field must not respond to drag-to-scrub.
    FieldFlag_Deprecated = 1u << 4      //!< Loaded from old scenes, hidden from the inspector.
};

// One entry of a reflected enumeration.
struct EnumEntry
{
    const char* Name = nullptr;
    Int64 Value = 0;
};

}  // namespace Hollowpoint::Reflection
