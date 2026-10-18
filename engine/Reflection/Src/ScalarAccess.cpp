#include <cmath>
#include <string>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Reflection/Detail/ScalarConvert.h"
#include "Hollowpoint/Reflection/ScalarAccess.h"

namespace Hollowpoint::Reflection
{

using Core::Size;

// ---------------------------------------------------------------------------
// Raw scalar access.
//
// Reads are exact — no conversion happens on the way in, so a float field round-trips bit-for-bit and an
// int64 keeps all 64 bits. Writes go through the shared clamping rules (Detail/ScalarConvert.h), which are
// the same ones the array element writers use, so the two paths can never store different bytes for the
// same value.
// ---------------------------------------------------------------------------

bool ReadScalar(const void* address, ValueKind kind, ScalarValue& outValue)
{
    if (address == nullptr)
    {
        return false;
    }

    switch (kind)
    {
        case ValueKind::Boolean:
            outValue = ScalarValue::FromBool(*static_cast<const bool*>(address));
            return true;

        case ValueKind::Int8:
            outValue = ScalarValue::FromSigned(kind, *static_cast<const Int8*>(address));
            return true;
        case ValueKind::Int16:
            outValue = ScalarValue::FromSigned(kind, *static_cast<const Int16*>(address));
            return true;
        case ValueKind::Int32:
            outValue = ScalarValue::FromSigned(kind, *static_cast<const Int32*>(address));
            return true;
        case ValueKind::Int64:
            outValue = ScalarValue::FromSigned(kind, *static_cast<const Int64*>(address));
            return true;

        case ValueKind::UInt8:
            outValue = ScalarValue::FromUnsigned(kind, *static_cast<const UInt8*>(address));
            return true;
        case ValueKind::UInt16:
            outValue = ScalarValue::FromUnsigned(kind, *static_cast<const UInt16*>(address));
            return true;
        case ValueKind::UInt32:
            outValue = ScalarValue::FromUnsigned(kind, *static_cast<const UInt32*>(address));
            return true;
        case ValueKind::UInt64:
            outValue = ScalarValue::FromUnsigned(kind, *static_cast<const UInt64*>(address));
            return true;

        case ValueKind::Float32:
            outValue = ScalarValue::FromReal(kind, *static_cast<const float*>(address));
            return true;
        case ValueKind::Float64:
            outValue = ScalarValue::FromReal(kind, *static_cast<const double*>(address));
            return true;

        case ValueKind::String:
        case ValueKind::Struct:
        case ValueKind::Array:
        case ValueKind::Enum:
        case ValueKind::Invalid:
            break;
    }

    // Not a plain scalar. Enum needs its field (the underlying integer width is only known there); String,
    // Struct and Array have their own entry points. Returning false beats reading garbage out of a
    // std::string's buffer.
    return false;
}

bool WriteScalar(void* address, ValueKind kind, const ScalarValue& value)
{
    if (address == nullptr)
    {
        return false;
    }

    // Both sides must be numeric-or-boolean. Checking the *source* here turns a type mismatch in a scene file
    // into a clean failure the loader can log and skip, instead of an assert firing deep inside a conversion
    // while someone is trying to load their work.
    if (!ScalarValue::IsNumericKind(kind) && kind != ValueKind::Boolean)
    {
        return false;
    }
    if (!ScalarValue::IsNumericKind(value.Kind) && value.Kind != ValueKind::Boolean)
    {
        return false;
    }

    // A non-finite source cannot be stored in an integer field, and the zero it degrades to would otherwise be
    // indistinguishable from a legitimate zero. Worth one log line on a load path that is never hot.
    if (ScalarValue::IsIntegerKind(kind) && Detail::IsRealSource(value) && std::isnan(value.AsReal))
    {
        HP_LOG_WARNING("REFLECT", "NaN written to integer field; stored as 0");
    }

    switch (kind)
    {
        case ValueKind::Boolean:
        {
            // The destination must end up a canonical 0 or 1 — any other byte pattern is undefined to read as a
            // bool, and ASan/UBSan builds would flag it. Any non-zero source counts as true, including "3.5".
            *static_cast<bool*>(address) = Detail::ToSigned(value) != 0;
            return true;
        }

        case ValueKind::Int8:
            *static_cast<Int8*>(address) = Detail::ClampSignedTo<Int8>(value);
            return true;
        case ValueKind::Int16:
            *static_cast<Int16*>(address) = Detail::ClampSignedTo<Int16>(value);
            return true;
        case ValueKind::Int32:
            *static_cast<Int32*>(address) = Detail::ClampSignedTo<Int32>(value);
            return true;
        case ValueKind::Int64:
            // Widest signed kind, so ToSigned already handled every source.
            *static_cast<Int64*>(address) = Detail::ToSigned(value);
            return true;

        case ValueKind::UInt8:
            *static_cast<UInt8*>(address) = Detail::ClampUnsignedTo<UInt8>(value);
            return true;
        case ValueKind::UInt16:
            *static_cast<UInt16*>(address) = Detail::ClampUnsignedTo<UInt16>(value);
            return true;
        case ValueKind::UInt32:
            *static_cast<UInt32*>(address) = Detail::ClampUnsignedTo<UInt32>(value);
            return true;
        case ValueKind::UInt64:
            *static_cast<UInt64*>(address) = Detail::ToUnsigned(value);
            return true;

        case ValueKind::Float32:
            *static_cast<float*>(address) = Detail::ClampRealTo<float>(value);
            return true;
        case ValueKind::Float64:
            // Widest real kind; only NaN is filtered (to zero), so no clamping is needed.
            *static_cast<double*>(address) = Detail::IsRealSource(value) ? value.AsReal : Detail::ToDouble(value);
            return true;

        default:
            return false;
    }
}

bool ReadString(const void* address, std::string_view& outText)
{
    if (address == nullptr)
    {
        return false;
    }
    outText = *static_cast<const std::string*>(address);
    return true;
}

bool WriteString(void* address, std::string_view text)
{
    if (address == nullptr)
    {
        return false;
    }
    static_cast<std::string*>(address)->assign(text);
    return true;
}

// ---------------------------------------------------------------------------
// Field access — kind dispatch plus the enum case, whose storage width only the field knows.
// ---------------------------------------------------------------------------

bool ReadFieldValue(const void* object, const FieldDescriptor& field, ScalarValue& outValue)
{
    if (object == nullptr)
    {
        return false;
    }

    if (field.GetKind() == ValueKind::Enum)
    {
        Int64 raw = 0;
        if (!field.ReadEnumAt(field.GetAddress(object), raw))
        {
            return false;
        }
        outValue = ScalarValue::FromSigned(ValueKind::Enum, raw);
        return true;
    }

    return ReadScalar(field.GetAddress(object), field.GetKind(), outValue);
}

bool WriteFieldValue(void* object, const FieldDescriptor& field, const ScalarValue& value)
{
    if (object == nullptr)
    {
        return false;
    }

    // ReadOnly is an editor concept and is enforced by the UI, not here: scripts are deliberately allowed to
    // write read-only fields, and the serializer must be able to load them.
    if (field.GetKind() == ValueKind::Enum)
    {
        return field.WriteEnumAt(field.GetAddress(object), Detail::ToSigned(value));
    }

    return WriteScalar(field.GetAddress(object), field.GetKind(), value);
}

bool ReadStringField(const void* object, const FieldDescriptor& field, std::string_view& outText)
{
    if (field.GetKind() != ValueKind::String)
    {
        HP_LOG_ERROR("REFLECT", "ReadStringField called on a non-string field ('{}')",
                     field.GetName() != nullptr ? field.GetName() : "?");
        return false;
    }
    return ReadString(field.GetAddress(object), outText);
}

bool WriteStringField(void* object, const FieldDescriptor& field, std::string_view text)
{
    if (field.GetKind() != ValueKind::String)
    {
        HP_LOG_ERROR("REFLECT", "WriteStringField called on a non-string field ('{}')",
                     field.GetName() != nullptr ? field.GetName() : "?");
        return false;
    }
    return WriteString(field.GetAddress(object), text);
}

// ---------------------------------------------------------------------------
// Array access.
// ---------------------------------------------------------------------------

Size GetArrayCount(const void* object, const FieldDescriptor& field)
{
    if (object == nullptr || field.GetKind() != ValueKind::Array)
    {
        return 0;
    }

    const ArrayOps& ops = field.GetArrayOps();
    if (ops.Count == nullptr)
    {
        HP_LOG_ERROR("REFLECT", "Reflected array field has no count operation");
        return 0;
    }
    return ops.Count(field.GetAddress(object));
}

bool ReadArrayElementScalar(const void* object, const FieldDescriptor& field, Size elementIndex, ScalarValue& outValue)
{
    if (object == nullptr || field.GetKind() != ValueKind::Array)
    {
        return false;
    }

    const ArrayOps& ops = field.GetArrayOps();
    if (ops.ReadElementScalar == nullptr)
    {
        return false;
    }
    return ops.ReadElementScalar(field.GetAddress(object), elementIndex, outValue);
}

bool WriteArrayElementScalar(void* object, const FieldDescriptor& field, Size elementIndex, const ScalarValue& value)
{
    if (object == nullptr || field.GetKind() != ValueKind::Array)
    {
        return false;
    }

    const ArrayOps& ops = field.GetArrayOps();
    if (ops.WriteElementScalar == nullptr)
    {
        return false;
    }
    return ops.WriteElementScalar(field.GetAddress(object), elementIndex, value);
}

const void* GetArrayElementAddress(const void* object, const FieldDescriptor& field, Size elementIndex)
{
    if (object == nullptr || field.GetKind() != ValueKind::Array)
    {
        return nullptr;
    }

    const ArrayOps& ops = field.GetArrayOps();
    if (ops.Count == nullptr || ops.Data == nullptr)
    {
        return nullptr;
    }

    const void* arrayAddress = field.GetAddress(object);
    if (elementIndex >= ops.Count(arrayAddress))
    {
        return nullptr;
    }

    // The stride comes from the element type captured at registration, so nested structs keep their alignment
    // without any caller having to look up a descriptor.
    if (ops.ElementSize == 0)
    {
        return nullptr;
    }

    const std::byte* base = static_cast<const std::byte*>(ops.Data(arrayAddress));
    return base + elementIndex * ops.ElementSize;
}

void* ResizeArrayField(void* object, const FieldDescriptor& field, Size newCount)
{
    if (object == nullptr || field.GetKind() != ValueKind::Array)
    {
        return nullptr;
    }

    const ArrayOps& ops = field.GetArrayOps();
    if (ops.Resize == nullptr)
    {
        HP_LOG_ERROR("REFLECT", "Reflected array field has no resize operation");
        return nullptr;
    }
    return ops.Resize(field.GetAddress(object), newCount);
}

void* PushBackArrayField(void* object, const FieldDescriptor& field)
{
    if (object == nullptr || field.GetKind() != ValueKind::Array)
    {
        return nullptr;
    }

    const ArrayOps& ops = field.GetArrayOps();
    if (ops.PushBackDefault == nullptr)
    {
        HP_LOG_ERROR("REFLECT", "Reflected array field has no push-back operation");
        return nullptr;
    }

    // The op returns the appended element's address, re-read after growth — returning data() here would hand
    // back element zero and have callers overwrite it.
    return ops.PushBackDefault(field.GetAddress(object));
}

}  // namespace Hollowpoint::Reflection
