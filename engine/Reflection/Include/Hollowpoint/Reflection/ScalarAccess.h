#pragma once

#include <string_view>

#include "Hollowpoint/Reflection/Descriptors.h"

namespace Hollowpoint::Reflection
{

// ---------------------------------------------------------------------------
// Scalar and field access — the only code that reinterprets reflected memory.
//
// Two layers: the raw kind-based entry points (used when a caller already knows the address and kind, e.g.
// deserialising into a temporary) and the field-based ones (which also handle enums, whose storage width is
// only known from the field's declaration). Keeping both here means the serializer, the inspector and the
// scripting layer cannot disagree about how a member is read or written.
// ---------------------------------------------------------------------------

// Raw access ---------------------------------------------------------------

[[nodiscard]] HP_REFLECTION_API bool ReadScalar(const void* address, ValueKind kind, ScalarValue& outValue);
[[nodiscard]] HP_REFLECTION_API bool WriteScalar(void* address, ValueKind kind, const ScalarValue& value);

[[nodiscard]] HP_REFLECTION_API bool ReadString(const void* address, std::string_view& outText);
[[nodiscard]] HP_REFLECTION_API bool WriteString(void* address, std::string_view text);

// Field access -------------------------------------------------------------

//! Reads a field as a scalar. Enum fields are widened to int64; String/Struct/Array return false and must use
//! their dedicated entry points.
[[nodiscard]] HP_REFLECTION_API bool ReadFieldValue(const void* object, const FieldDescriptor& field,
                                                    ScalarValue& outValue);
//! Writes a scalar into a field, converting and clamping as needed (see the file comment for why clamping).
[[nodiscard]] HP_REFLECTION_API bool WriteFieldValue(void* object, const FieldDescriptor& field,
                                                     const ScalarValue& value);

[[nodiscard]] HP_REFLECTION_API bool ReadStringField(const void* object, const FieldDescriptor& field,
                                                     std::string_view& outText);
[[nodiscard]] HP_REFLECTION_API bool WriteStringField(void* object, const FieldDescriptor& field,
                                                      std::string_view text);

// Array access -------------------------------------------------------------

[[nodiscard]] HP_REFLECTION_API Size GetArrayCount(const void* object, const FieldDescriptor& field);

//! Scalar access to one array element (works for arrays of scalars and enums, which have no TypeDescriptor).
[[nodiscard]] HP_REFLECTION_API bool ReadArrayElementScalar(const void* object, const FieldDescriptor& field,
                                                            Size elementIndex, ScalarValue& outValue);
[[nodiscard]] HP_REFLECTION_API bool WriteArrayElementScalar(void* object, const FieldDescriptor& field,
                                                             Size elementIndex, const ScalarValue& value);

//! Address of one element, for traversal into arrays of reflected structs. nullptr when the index is out of
//! range or the element type cannot be described.
[[nodiscard]] HP_REFLECTION_API const void* GetArrayElementAddress(const void* object, const FieldDescriptor& field,
                                                                   Size elementIndex);

//! Resizes the array (default-constructing new elements) and returns the new first element's address.
[[nodiscard]] HP_REFLECTION_API void* ResizeArrayField(void* object, const FieldDescriptor& field, Size newCount);

//! Appends one default-constructed element and returns its address (nullptr on failure).
[[nodiscard]] HP_REFLECTION_API void* PushBackArrayField(void* object, const FieldDescriptor& field);

}  // namespace Hollowpoint::Reflection
