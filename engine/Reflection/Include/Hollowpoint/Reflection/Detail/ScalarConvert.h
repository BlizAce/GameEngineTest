#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Reflection/ReflectionTypes.h"

namespace Hollowpoint::Reflection::Detail
{

using Core::Int16;
using Core::Int32;
using Core::Int64;
using Core::Int8;
using Core::UInt16;
using Core::UInt32;
using Core::UInt64;
using Core::UInt8;

// ---------------------------------------------------------------------------
// Numeric conversion rules shared by every reflected write path.
//
// These live in a header rather than inside ScalarAccess.cpp because two independent entry points need the
// *identical* rules: writing a field, and writing one element of a std::vector<T> (whose ops are generated
// from the member type at registration). If they diverged, saving a scene and editing the same value in the
// inspector would produce different stored bytes — exactly the kind of bug that is invisible until someone
// notices an inspector edit changed a value they did not touch.
//
// Policy: writes clamp, never wrap. A value from a newer build or a mistyped scene file must not silently
// turn an Int8 of 300 into 44; saturating is visibly wrong in the inspector, which is far cheaper to
// diagnose than a simulation that drifts because a field wrapped around.
// ---------------------------------------------------------------------------

[[nodiscard]] inline double ToDouble(const ScalarValue& value)
{
    switch (value.Kind)
    {
        case ValueKind::Boolean:
            return value.AsBool ? 1.0 : 0.0;
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::Enum:
            return static_cast<double>(value.AsInt);
        case ValueKind::UInt8:
        case ValueKind::UInt16:
        case ValueKind::UInt32:
        case ValueKind::UInt64:
            return static_cast<double>(value.AsUnsigned);
        case ValueKind::Float32:
        case ValueKind::Float64:
            return value.AsReal;
        case ValueKind::String:
        case ValueKind::Struct:
        case ValueKind::Array:
        case ValueKind::Invalid:
            break;
    }

    HP_ASSERT_MSG(false, "ToDouble requires a numeric kind");
    return 0.0;
}

[[nodiscard]] inline Int64 ToSigned(const ScalarValue& value)
{
    switch (value.Kind)
    {
        case ValueKind::Boolean:
            return value.AsBool ? 1 : 0;
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::Enum:
            return value.AsInt;
        case ValueKind::UInt8:
        case ValueKind::UInt16:
        case ValueKind::UInt32:
        case ValueKind::UInt64:
            // A source above INT64_MAX saturates rather than wrapping negative.
            if (value.AsUnsigned > static_cast<UInt64>(std::numeric_limits<Int64>::max()))
            {
                return std::numeric_limits<Int64>::max();
            }
            return static_cast<Int64>(value.AsUnsigned);
        case ValueKind::Float32:
        case ValueKind::Float64:
            // NaN becomes 0 rather than propagating: a reflected field read as an index or count must never be
            // NaN, and losing the value is safer than poisoning every downstream computation.
            if (std::isnan(value.AsReal))
            {
                return 0;
            }
            if (value.AsReal >= static_cast<double>(std::numeric_limits<Int64>::max()))
            {
                return std::numeric_limits<Int64>::max();
            }
            if (value.AsReal <= static_cast<double>(std::numeric_limits<Int64>::lowest()))
            {
                return std::numeric_limits<Int64>::lowest();
            }
            // Truncates toward zero: 2.9 stored into an integer field becomes 2.
            return static_cast<Int64>(value.AsReal);
        case ValueKind::String:
        case ValueKind::Struct:
        case ValueKind::Array:
        case ValueKind::Invalid:
            break;
    }

    HP_ASSERT_MSG(false, "ToSigned requires a numeric kind");
    return 0;
}

[[nodiscard]] inline UInt64 ToUnsigned(const ScalarValue& value)
{
    switch (value.Kind)
    {
        case ValueKind::Boolean:
            return value.AsBool ? 1u : 0u;
        case ValueKind::UInt8:
        case ValueKind::UInt16:
        case ValueKind::UInt32:
        case ValueKind::UInt64:
            return value.AsUnsigned;
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::Enum:
            // Negative input saturates to zero instead of wrapping into a huge unsigned value.
            return value.AsInt < 0 ? 0u : static_cast<UInt64>(value.AsInt);
        case ValueKind::Float32:
        case ValueKind::Float64:
            if (std::isnan(value.AsReal) || value.AsReal <= 0.0)
            {
                return 0u;
            }
            if (value.AsReal >= static_cast<double>(std::numeric_limits<UInt64>::max()))
            {
                return std::numeric_limits<UInt64>::max();
            }
            return static_cast<UInt64>(value.AsReal);
        case ValueKind::String:
        case ValueKind::Struct:
        case ValueKind::Array:
        case ValueKind::Invalid:
            break;
    }

    HP_ASSERT_MSG(false, "ToUnsigned requires a numeric kind");
    return 0u;
}

template <typename TTarget>
[[nodiscard]] inline TTarget ClampSignedTo(const ScalarValue& value)
{
    const Int64 source = ToSigned(value);

    // Compared as int64 throughout: casting the target's lowest() through double would be inexact for wide
    // types and could let a value through that then wraps on the final cast.
    const Int64 lowest = static_cast<Int64>(std::numeric_limits<TTarget>::lowest());
    const Int64 highest = static_cast<Int64>(std::numeric_limits<TTarget>::max());

    if (source < lowest)
    {
        return std::numeric_limits<TTarget>::lowest();
    }
    if (source > highest)
    {
        return std::numeric_limits<TTarget>::max();
    }
    return static_cast<TTarget>(source);
}

template <typename TTarget>
[[nodiscard]] inline TTarget ClampUnsignedTo(const ScalarValue& value)
{
    const UInt64 source = ToUnsigned(value);
    if (source > static_cast<UInt64>(std::numeric_limits<TTarget>::max()))
    {
        return std::numeric_limits<TTarget>::max();
    }
    return static_cast<TTarget>(source);
}

template <typename TTarget>
[[nodiscard]] inline TTarget ClampRealTo(const ScalarValue& value)
{
    const double source = ToDouble(value);

    // A floating-point target keeps NaN. Reflection's job is faithful data transport: silently rewriting NaN
    // to zero destroys the only evidence of an upstream bug (a bad transform, a division by zero in a tool)
    // and turns a traceable scene into an untraceable one. Where NaN genuinely cannot be represented — integer
    // targets — ToSigned/ToUnsigned already yield zero, and WriteScalar reports it.
    if constexpr (std::is_floating_point_v<TTarget>)
    {
        if (std::isnan(source))
        {
            return std::numeric_limits<TTarget>::quiet_NaN();
        }
    }

    // Bounds are widened to double before comparison. For float targets this is exact; for double it is a
    // no-op, and saturating instead of producing infinity keeps gameplay and rendering arithmetic finite.
    const double lowest = static_cast<double>(std::numeric_limits<TTarget>::lowest());
    const double highest = static_cast<double>(std::numeric_limits<TTarget>::max());

    if (source <= lowest)
    {
        return std::numeric_limits<TTarget>::lowest();
    }
    if (source >= highest)
    {
        return std::numeric_limits<TTarget>::max();
    }
    return static_cast<TTarget>(source);
}

//! True when the source value is a floating-point one (so an integer target should truncate, not re-read AsInt).
[[nodiscard]] inline bool IsRealSource(const ScalarValue& value)
{
    return value.Kind == ValueKind::Float32 || value.Kind == ValueKind::Float64;
}

}  // namespace Hollowpoint::Reflection::Detail
