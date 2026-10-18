#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Serialization
{

using Core::Index;
using Core::Int64;
using Core::Size;
using Core::UInt64;

class JsonValue;

//! Array element storage. C++17 (P0448) explicitly permits std::vector to be instantiated with an incomplete
//! element type, which is what lets a JSON tree be recursive without allocating every node separately.
//!
//! Consequence worth knowing before editing this header: instantiating the vector is legal, but *using* operations
//! that need the element complete (clear, erase, assignment of a whole array) is not while JsonValue is incomplete.
//! libstdc++ defers those; libc++/clang instantiate them eagerly and fail with "arithmetic on a pointer to an
//! incomplete type". So every member whose body touches JsonArray — Clear, SetArray/SetObject, the array/object
//! constructors, the builders — must be defined in the .cpp, not inline here. That is why they look unimplemented.
using JsonArray = std::vector<JsonValue>;

// ---------------------------------------------------------------------------
// JsonObject — an insertion-ordered set of members.
//
// Defined before JsonValue rather than after: JsonValue stores one in a std::variant, and unlike std::vector a
// variant requires its alternatives to be complete at the point it is declared. Keeping the object type outside the
// variant cycle is what makes the recursive definition legal without pointer indirection.
//
// Why not std::map or std::unordered_map: both destroy the order in which fields were written, and that order is
// load-bearing here. Scene files are committed to version control and reviewed by humans; a member order that changes
// between engine versions (or between runs, as unordered_map iteration order does) turns every save into a diff of the
// whole file. Insertion order also lets writers emit fields in declaration order — identity first, then components —
// which is what makes a hand-authored scene readable.
//
// Storage is two parallel arrays: keys and values. A struct holding a JsonValue by value would need JsonValue to be
// complete, which it is not at this point; two vectors of (string, JsonValue) dodge that while keeping lookups
// scanning a compact key array, which beats the pointer chasing of a node-based map on both cache behaviour and
// allocation count for the handful of members a scene object actually has. A hash index can be added behind this
// interface without touching callers if a scene with thousands of members per object ever needs one.
// ---------------------------------------------------------------------------
class JsonObject
{
public:
    JsonObject() = default;

    [[nodiscard]] Size GetCount() const noexcept
    {
        return m_Keys.size();
    }

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return m_Keys.empty();
    }

    //! Key of the member at `index`. Out-of-range is a programming error (asserted), since callers iterate from
    //! GetCount().
    [[nodiscard]] const std::string& GetKey(Size index) const;

    [[nodiscard]] const JsonValue* GetValueAt(Size index) const;
    [[nodiscard]] JsonValue* GetValueAtMutable(Size index);

    //! Position of `key`, or GetCount() when absent.
    [[nodiscard]] Size FindIndex(const std::string& key) const noexcept;

    [[nodiscard]] bool Contains(const std::string& key) const noexcept
    {
        return FindIndex(key) != GetCount();
    }

    //! Value for `key`, or nullptr when absent. Returning a pointer rather than a reference to a static default keeps
    //! "missing" impossible to mistake for "present and null" — a distinction scene loading depends on.
    [[nodiscard]] const JsonValue* Find(const std::string& key) const noexcept;

    [[nodiscard]] JsonValue* FindMutable(const std::string& key) noexcept;

    //! Inserts or replaces. Insertion appends; replacement overwrites in place, so re-saving a loaded scene does not
    //! reorder it and produces a one-line diff instead of rewriting the object.
    JsonValue& Set(std::string key, JsonValue value);

    bool Remove(const std::string& key) noexcept;

    //! Defined out-of-line: clearing JsonArray needs JsonValue complete, and clang instantiates vector operations on
    //! an incomplete element type eagerly (libstdc++ defers them), so an inline body here would not compile.
    void Clear() noexcept;

private:
    std::vector<std::string> m_Keys;
    JsonArray m_Values;
};

// ---------------------------------------------------------------------------
// JsonValue — one node of a JSON document.
//
// Numbers keep integers and reals apart (Int64 / UInt64 / Real). JSON itself has a single number type, but an engine
// cannot: entity indices, GUID halves and 64-bit asset hashes must survive a save/load cycle without ever passing
// through a float, and "0.30000000000000004" appearing in a committed scene file is a bug report waiting to happen.
// Reals are written with the shortest representation that round-trips exactly, so a value is stored as precisely as
// the format allows and no less.
//
// Accessors are **side-effect-free**: a kind mismatch yields the caller's fallback with no logging. This type has no
// idea which field it was asked for, and a global "report at most N errors" counter would make diagnostics depend on
// process history (and therefore on test order). Error reporting belongs to the parser and the reflection bridge,
// both of which know paths.
//
// This is a plain data holder: no schema knowledge, no reflection, no I/O. Decoding into engine types lives in the
// value/reflection bridge, which keeps parsing testable on its own and lets other formats reuse the same decode layer.
// ---------------------------------------------------------------------------
class JsonValue
{
public:
    enum class Kind : Core::UInt8
    {
        Null,
        Boolean,
        Int,   //!< Signed integer, exactly representable.
        UInt,  //!< Unsigned integer above Int64's maximum (asset hashes, packed GUIDs).
        Real,  //!< IEEE-754 double.
        String,
        Array,
        Object
    };

    [[nodiscard]] static const char* KindName(Kind kind) noexcept;

    JsonValue() noexcept = default;

    // Constructors are deliberately not implicit: `JsonValue(true)` vs `JsonValue(Int64{1})` picking a kind by
    // accident is exactly the class of bug that silently changes a scene file's bytes.
    JsonValue(std::nullptr_t) noexcept : m_Value(std::monostate{}) {}

    JsonValue(bool value) noexcept : m_Value(value) {}

    // Narrower integer types widen to Int64/UInt64 so callers can pass ints, indices and sizes without casts.
    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    JsonValue(T value) noexcept
    {
        if constexpr (std::is_signed_v<T>)
        {
            m_Value = static_cast<Int64>(value);
        }
        else
        {
            m_Value = static_cast<UInt64>(value);
        }
    }

    JsonValue(double value) noexcept : m_Value(value) {}

    JsonValue(float value) noexcept : m_Value(static_cast<double>(value)) {}

    JsonValue(std::string value) noexcept : m_Value(std::move(value)) {}

    JsonValue(const char* value) : m_Value(std::string(value)) {}

    //! Out-of-line: constructing this variant alternative needs JsonArray/JsonObject complete (see below).
    JsonValue(JsonArray value) noexcept;
    JsonValue(JsonObject value) noexcept;


    [[nodiscard]] Kind GetKind() const noexcept
    {
        return static_cast<Kind>(m_Value.index());
    }

    [[nodiscard]] bool IsNull() const noexcept
    {
        return std::holds_alternative<std::monostate>(m_Value);
    }

    [[nodiscard]] bool IsBoolean() const noexcept
    {
        return std::holds_alternative<bool>(m_Value);
    }

    [[nodiscard]] bool IsInt() const noexcept
    {
        return std::holds_alternative<Int64>(m_Value);
    }

    [[nodiscard]] bool IsUInt() const noexcept
    {
        return std::holds_alternative<UInt64>(m_Value);
    }

    [[nodiscard]] bool IsReal() const noexcept
    {
        return std::holds_alternative<double>(m_Value);
    }

    //! True for Int, UInt and Real — the three number kinds. Written out rather than as a range test so adding a
    //! numeric kind cannot silently make this wrong.
    [[nodiscard]] bool IsNumber() const noexcept
    {
        return IsInt() || IsUInt() || IsReal();
    }

    [[nodiscard]] bool IsString() const noexcept
    {
        return std::holds_alternative<std::string>(m_Value);
    }

    [[nodiscard]] bool IsArray() const noexcept
    {
        return std::holds_alternative<JsonArray>(m_Value);
    }

    [[nodiscard]] bool IsObject() const noexcept
    {
        return std::holds_alternative<JsonObject>(m_Value);
    }

    // Kind-checked access; each returns the caller's fallback when the kind does not match.

    [[nodiscard]] bool AsBool(bool fallback = false) const noexcept;

    [[nodiscard]] Int64 AsInt(Int64 fallback = 0) const noexcept;

    [[nodiscard]] UInt64 AsUInt(UInt64 fallback = 0) const noexcept;

    [[nodiscard]] double AsDouble(double fallback = 0.0) const noexcept;

    //! The string value, or a shared empty string when this is not a string. No fallback parameter: callers check
    //! IsString() when "absent" and "empty" must be distinguished, and an extra reference parameter here invited
    //! binding to a temporary that would outlive the call.
    [[nodiscard]] const std::string& AsString() const noexcept;

    [[nodiscard]] const JsonArray& AsArray() const noexcept;

    [[nodiscard]] const JsonObject& AsObject() const noexcept;

    //! Mutable access for building documents. Converts in place when the node holds something else: callers
    //! assembling a document know which kind they want, and requiring a pre-set kind adds ceremony without catching
    //! mistakes.
    [[nodiscard]] JsonArray& AsArrayMutable() noexcept;
    [[nodiscard]] JsonObject& AsObjectMutable() noexcept;

    //! Object convenience: member lookup, or nullptr when this is not an object or the key is absent.
    [[nodiscard]] const JsonValue* FindMember(const std::string& key) const noexcept;

    //! Number of array elements, object members, or UTF-8 code units in a string; 0 otherwise.
    [[nodiscard]] Size GetElementCount() const noexcept;

    //! Array element by index, or nullptr when out of range / not an array. Bounds-checked because scene data
    //! arriving from disk is untrusted.
    [[nodiscard]] const JsonValue* FindElement(Index index) const noexcept;

    void SetNull() noexcept
    {
        m_Value = std::monostate{};
    }

    void SetArray(JsonArray value = {}) noexcept;

    void SetObject(JsonObject value = {}) noexcept;

    //! Structural equality. Numbers compare across kinds by value (1 == 1.0), matching JSON's single number type;
    //! strings never equal numbers, and objects compare independently of member order.
    [[nodiscard]] bool Equals(const JsonValue& other) const noexcept;

    friend bool operator==(const JsonValue& lhs, const JsonValue& rhs) noexcept
    {
        return lhs.Equals(rhs);
    }

    //! Compact single-line debug rendering (log/debugger use only; no guarantees about key order).
    [[nodiscard]] std::string ToDebugString() const;

private:
    std::variant<std::monostate, bool, Int64, UInt64, double, std::string, JsonArray, JsonObject> m_Value;

    // Returned by the accessors on a kind mismatch so a miss neither allocates nor dangles. Static members are
    // allowed to have an incomplete class type at declaration, so these can be declared here even though JsonObject
    // is complete at this point anyway.
    static const std::string s_EmptyString;
    static const JsonArray s_EmptyArray;
    static const JsonObject s_EmptyObject;
};

// Builders for assembling documents (defined in the .cpp for the same completeness reason as the members above).
JsonValue MakeArray(JsonArray elements = {});
JsonValue MakeObject(JsonObject members = {});

}  // namespace Hollowpoint::Serialization
