#include <cmath>
#include <utility>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Serialization/JsonValue.h"

namespace Hollowpoint::Serialization
{

const std::string JsonValue::s_EmptyString;
const JsonArray JsonValue::s_EmptyArray;
const JsonObject JsonValue::s_EmptyObject;

const char* JsonValue::KindName(Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::Null:
            return "null";
        case Kind::Boolean:
            return "boolean";
        case Kind::Int:
            return "int";
        case Kind::UInt:
            return "uint";
        case Kind::Real:
            return "real";
        case Kind::String:
            return "string";
        case Kind::Array:
            return "array";
        case Kind::Object:
            return "object";
    }

    HP_ASSERT_MSG(false, "Kind is outside the known range");
    return "?";
}

// ---------------------------------------------------------------------------
// JsonObject.
// ---------------------------------------------------------------------------

void JsonObject::Clear() noexcept
{
    m_Keys.clear();
    m_Values.clear();
}

const std::string& JsonObject::GetKey(Size index) const
{
    HP_ASSERT_MSG(index < m_Keys.size(), "JsonObject member index out of range");
    return m_Keys[index];
}

const JsonValue* JsonObject::GetValueAt(Size index) const
{
    return index < m_Values.size() ? &m_Values[index] : nullptr;
}

JsonValue* JsonObject::GetValueAtMutable(Size index)
{
    return index < m_Values.size() ? &m_Values[index] : nullptr;
}

Size JsonObject::FindIndex(const std::string& key) const noexcept
{
    // Linear on purpose (see the class comment). The key array is contiguous and short, so this beats a tree walk.
    for (Size index = 0; index < m_Keys.size(); ++index)
    {
        if (m_Keys[index] == key)
        {
            return index;
        }
    }
    return m_Keys.size();
}

const JsonValue* JsonObject::Find(const std::string& key) const noexcept
{
    const Size index = FindIndex(key);
    return index < m_Values.size() ? &m_Values[index] : nullptr;
}

JsonValue* JsonObject::FindMutable(const std::string& key) noexcept
{
    const Size index = FindIndex(key);
    return index < m_Values.size() ? &m_Values[index] : nullptr;
}

JsonValue& JsonObject::Set(std::string key, JsonValue value)
{
    const Size index = FindIndex(key);
    if (index < m_Values.size())
    {
        // Replace in place, keeping the position: "load a scene, change one field, save" must produce a one-line
        // diff, not a rewrite of the object.
        JsonValue& existing = m_Values[index];
        existing = std::move(value);
        return existing;
    }

    m_Keys.push_back(std::move(key));
    m_Values.push_back(std::move(value));
    return m_Values.back();
}

bool JsonObject::Remove(const std::string& key) noexcept
{
    const Size index = FindIndex(key);
    if (index >= m_Values.size())
    {
        return false;
    }

    // erase (not swap-and-pop) preserves the order of the remaining members, for the same reason Set does.
    m_Keys.erase(m_Keys.begin() + static_cast<std::ptrdiff_t>(index));
    m_Values.erase(m_Values.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// ---------------------------------------------------------------------------
// JsonValue — kind-checked access.
//
// No logging on mismatch: see the class comment. Diagnostics are collected where paths are known (parser, bridge).
// ---------------------------------------------------------------------------

bool JsonValue::AsBool(bool fallback) const noexcept
{
    if (const bool* value = std::get_if<bool>(&m_Value))
    {
        return *value;
    }

    // A JSON true/false is the only boolean; integers are not coerced. Accepting 0/1 would silently read a file that
    // stored a flag as a number and then disagree with the next reader about what it meant.
    return fallback;
}

Int64 JsonValue::AsInt(Int64 fallback) const noexcept
{
    if (const Int64* value = std::get_if<Int64>(&m_Value))
    {
        return *value;
    }
    if (const UInt64* value = std::get_if<UInt64>(&m_Value))
    {
        // Values above Int64's maximum wrap rather than clamp. Callers that need the top bit check IsUInt() and use
        // AsUInt(); clamping would hide a wrong-type read behind a plausible-looking number.
        return static_cast<Int64>(*value);
    }
    if (const double* value = std::get_if<double>(&m_Value))
    {
        // Accepting a real is deliberate: writers and hand-editors produce "5" and "5.0" interchangeably, so refusing
        // the second would reject valid files. Callers that must not lose a fraction check IsInt() first.
        return static_cast<Int64>(*value);
    }

    return fallback;
}

UInt64 JsonValue::AsUInt(UInt64 fallback) const noexcept
{
    if (const UInt64* value = std::get_if<UInt64>(&m_Value))
    {
        return *value;
    }
    if (const Int64* value = std::get_if<Int64>(&m_Value))
    {
        // A negative value would wrap to a huge unsigned and propagate silently through indices and hashes, so the
        // caller's fallback is returned instead.
        return *value < 0 ? fallback : static_cast<UInt64>(*value);
    }
    if (const double* value = std::get_if<double>(&m_Value))
    {
        return *value < 0.0 ? fallback : static_cast<UInt64>(*value);
    }

    return fallback;
}

double JsonValue::AsDouble(double fallback) const noexcept
{
    if (const double* value = std::get_if<double>(&m_Value))
    {
        return *value;
    }
    // Integers widen to double exactly up to 2^53, which covers every coordinate an engine writes.
    if (const Int64* value = std::get_if<Int64>(&m_Value))
    {
        return static_cast<double>(*value);
    }
    if (const UInt64* value = std::get_if<UInt64>(&m_Value))
    {
        return static_cast<double>(*value);
    }

    return fallback;
}

const std::string& JsonValue::AsString() const noexcept
{
    if (const std::string* value = std::get_if<std::string>(&m_Value))
    {
        return *value;
    }
    return s_EmptyString;
}

const JsonArray& JsonValue::AsArray() const noexcept
{
    if (const JsonArray* value = std::get_if<JsonArray>(&m_Value))
    {
        return *value;
    }
    return s_EmptyArray;
}

const JsonObject& JsonValue::AsObject() const noexcept
{
    if (const JsonObject* value = std::get_if<JsonObject>(&m_Value))
    {
        return *value;
    }
    return s_EmptyObject;
}

JsonArray& JsonValue::AsArrayMutable() noexcept
{
    if (JsonArray* array = std::get_if<JsonArray>(&m_Value))
    {
        return *array;
    }

    m_Value = JsonArray{};
    return std::get<JsonArray>(m_Value);
}

JsonObject& JsonValue::AsObjectMutable() noexcept
{
    if (JsonObject* object = std::get_if<JsonObject>(&m_Value))
    {
        return *object;
    }

    m_Value = JsonObject{};
    return std::get<JsonObject>(m_Value);
}

const JsonValue* JsonValue::FindMember(const std::string& key) const noexcept
{
    if (const JsonObject* object = std::get_if<JsonObject>(&m_Value))
    {
        return object->Find(key);
    }
    return nullptr;
}

Size JsonValue::GetElementCount() const noexcept
{
    if (const JsonArray* array = std::get_if<JsonArray>(&m_Value))
    {
        return array->size();
    }
    if (const JsonObject* object = std::get_if<JsonObject>(&m_Value))
    {
        return object->GetCount();
    }
    if (const std::string* value = std::get_if<std::string>(&m_Value))
    {
        return value->size();
    }
    return 0;
}

const JsonValue* JsonValue::FindElement(Index index) const noexcept
{
    const JsonArray* array = std::get_if<JsonArray>(&m_Value);
    if (array == nullptr || index >= array->size())
    {
        return nullptr;
    }
    return &(*array)[index];
}

// These are out-of-line rather than inline in the header because they construct or assign a variant alternative that
// holds JsonArray/JsonObject, which requires the element type to be complete — see the note in JsonValue.h.
JsonValue::JsonValue(JsonArray value) noexcept : m_Value(std::move(value)) {}

JsonValue::JsonValue(JsonObject value) noexcept : m_Value(std::move(value)) {}

void JsonValue::SetArray(JsonArray value) noexcept
{
    m_Value = std::move(value);
}

void JsonValue::SetObject(JsonObject value) noexcept
{
    m_Value = std::move(value);
}

JsonValue MakeArray(JsonArray elements)
{
    return JsonValue(std::move(elements));
}

JsonValue MakeObject(JsonObject members)
{
    return JsonValue(std::move(members));
}

bool JsonValue::Equals(const JsonValue& other) const noexcept
{
    const Kind left = GetKind();
    const Kind right = other.GetKind();

    // Numbers compare by value across kinds, matching JSON's single number type: a file that wrote 5 and code that
    // reads back 5.0 describe the same document. Strings never equal numbers.
    if (IsNumber() && other.IsNumber())
    {
        // Same-signedness comparisons stay in integer arithmetic so values above 2^53 do not lose precision and
        // compare equal when they differ.
        if (left == Kind::Int && right == Kind::Int)
        {
            return std::get<Int64>(m_Value) == std::get<Int64>(other.m_Value);
        }
        if (left == Kind::UInt && right == Kind::UInt)
        {
            return std::get<UInt64>(m_Value) == std::get<UInt64>(other.m_Value);
        }
        if (left == Kind::Int && right == Kind::UInt)
        {
            const Int64 signedValue = std::get<Int64>(m_Value);
            return signedValue >= 0 && static_cast<UInt64>(signedValue) == std::get<UInt64>(other.m_Value);
        }
        if (left == Kind::UInt && right == Kind::Int)
        {
            const Int64 signedValue = std::get<Int64>(other.m_Value);
            return signedValue >= 0 && static_cast<UInt64>(signedValue) == std::get<UInt64>(m_Value);
        }
        return AsDouble() == other.AsDouble();
    }

    if (left != right)
    {
        return false;
    }

    switch (left)
    {
        case Kind::Null:
            return true;
        case Kind::Boolean:
            return std::get<bool>(m_Value) == std::get<bool>(other.m_Value);
        case Kind::Int:
        case Kind::UInt:
        case Kind::Real:
            // Unreachable: any pair of numbers was handled above, and differing kinds returned false. Kept explicit
            // so adding a numeric kind cannot silently fall through to "true".
            HP_ASSERT_MSG(false, "numeric kinds must be compared in the number branch above");
            return false;
        case Kind::String:
            return std::get<std::string>(m_Value) == std::get<std::string>(other.m_Value);
        case Kind::Array:
        {
            const JsonArray& leftArray = std::get<JsonArray>(m_Value);
            const JsonArray& rightArray = std::get<JsonArray>(other.m_Value);
            if (leftArray.size() != rightArray.size())
            {
                return false;
            }
            for (Size index = 0; index < leftArray.size(); ++index)
            {
                if (!leftArray[index].Equals(rightArray[index]))
                {
                    return false;
                }
            }
            return true;
        }
        case Kind::Object:
        {
            const JsonObject& leftObject = std::get<JsonObject>(m_Value);
            const JsonObject& rightObject = std::get<JsonObject>(other.m_Value);
            if (leftObject.GetCount() != rightObject.GetCount())
            {
                return false;
            }
            // Order-insensitive: two objects with the same members are equal however they were written.
            for (Size index = 0; index < leftObject.GetCount(); ++index)
            {
                const JsonValue* match = rightObject.Find(leftObject.GetKey(index));
                if (match == nullptr || !match->Equals(*leftObject.GetValueAt(index)))
                {
                    return false;
                }
            }
            return true;
        }
    }

    return false;
}

}  // namespace Hollowpoint::Serialization
