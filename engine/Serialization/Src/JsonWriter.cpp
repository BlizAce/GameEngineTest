#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Serialization/JsonWriter.h"

namespace Hollowpoint::Serialization
{

using Core::Int64;
using Core::UInt32;
using Core::UInt64;

std::string FormatDouble(double value)
{
    // Non-finite values have no JSON representation. NaN and Infinity are not valid JSON literals, and emitting
    // them produces files that other readers reject — but silently writing "0" instead is worse, because a NaN in a
    // transform means something upstream already broke and hiding it moves the diagnosis somewhere darker. So: log
    // once per call, write null (which every JSON reader accepts and an engine loader can detect as "not a number").
    if (!std::isfinite(value))
    {
        HP_LOG_ERROR("Serialization",
                     "Cannot write %s to JSON: the format has no representation for it; writing null instead. "
                     "A non-finite value here means something upstream produced it.",
                     std::isnan(value) ? "NaN" : "Infinity");
        return "null";
    }

    // A buffer of 40 bytes holds any double shortest form (longest is around 24 characters). Sized generously so
    // the to_chars failure path should be unreachable.
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);

    if (result.ec != std::errc())
    {
        HP_LOG_ERROR("Serialization", "Formatting the double {} failed; writing null", value);
        return "null";
    }

    std::string text(buffer, static_cast<Size>(result.ptr - buffer));

    // A double whose shortest form has no fractional or exponent part gets ".0" appended, so 3.0 writes as "3.0" and
    // not "3". Without this, saving a scene rewrites every whole-number float into an integer on the next load: the
    // numeric value survives but the kind does not, which changes what a human reading the file thinks the field is
    // and produces churn in diffs whenever a float happens to be whole. "-0" becomes "-0.0" for the same reason.
    if (text.find_first_of(".eE") == std::string::npos)
    {
        text += ".0";
    }

    return text;
}

namespace
{

class Writer
{
public:
    Writer(std::string& out, const JsonWriteOptions& options) noexcept : m_Out(out), m_Options(options) {}

    void Write(const JsonValue& value)
    {
        WriteValue(value, 0);
    }

private:
    void Indent(Size depth)
    {
        if (m_Options.IndentSpaces == 0)
        {
            return;
        }
        m_Out.append(m_Options.IndentSpaces * depth, ' ');
    }

    void Newline()
    {
        if (m_Options.IndentSpaces == 0)
        {
            return;
        }
        m_Out.push_back('\n');
    }

    void WriteValue(const JsonValue& value, Size depth)
    {
        switch (value.GetKind())
        {
            case JsonValue::Kind::Null:
                m_Out += "null";
                break;
            case JsonValue::Kind::Boolean:
                m_Out += value.AsBool() ? "true" : "false";
                break;
            case JsonValue::Kind::Int:
                // Written from the integer directly — never through a double, which would lose the low bits above
                // 2^53 and turn a stored entity index or hash into a different number.
                m_Out += std::to_string(value.AsInt());
                break;
            case JsonValue::Kind::UInt:
            {
                char buffer[32];
                const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value.AsUInt());
                if (result.ec == std::errc())
                {
                    m_Out.append(buffer, static_cast<Size>(result.ptr - buffer));
                }
                else
                {
                    HP_LOG_ERROR("Serialization", "Formatting an unsigned 64-bit value failed");
                    m_Out += "0";
                }
                break;
            }
            case JsonValue::Kind::Real:
                m_Out += FormatDouble(value.AsDouble());
                break;
            case JsonValue::Kind::String:
                WriteString(value.AsString());
                break;
            case JsonValue::Kind::Array:
                WriteArray(value, depth);
                break;
            case JsonValue::Kind::Object:
                WriteObject(value, depth);
                break;
        }
    }

    void WriteArray(const JsonValue& value, Size depth)
    {
        const JsonArray& elements = value.AsArray();

        if (elements.empty())
        {
            m_Out += "[]";
            return;
        }

        m_Out.push_back('[');
        for (Size index = 0; index < elements.size(); ++index)
        {
            if (index != 0)
            {
                m_Out.push_back(',');
            }
            Newline();
            Indent(depth + 1);
            WriteValue(elements[index], depth + 1);
        }
        Newline();
        Indent(depth);
        m_Out.push_back(']');
    }

    void WriteObject(const JsonValue& value, Size depth)
    {
        const JsonObject& members = value.AsObject();

        if (members.IsEmpty())
        {
            m_Out += "{}";
            return;
        }

        // Member order. Insertion order is the default because it preserves whatever order the loaded file had and
        // whatever declaration order the writer used, which keeps diffs minimal. Sorting exists only for golden-file
        // comparisons where two independently constructed documents must match byte-for-byte.
        // Member order. Insertion order is the default: it preserves whatever order the loaded file had and
        // whatever declaration order the writer used, which keeps diffs minimal. Sorting exists only for golden-file
        // comparisons where two independently constructed documents must match byte-for-byte.
        std::vector<Size> order;
        order.reserve(members.GetCount());
        for (Size index = 0; index < members.GetCount(); ++index)
        {
            order.push_back(index);
        }

        if (m_Options.SortObjectKeys)
        {
            std::stable_sort(order.begin(), order.end(),
                             [&members](Size lhs, Size rhs) { return members.GetKey(lhs) < members.GetKey(rhs); });
        }

        m_Out.push_back('{');
        for (Size position = 0; position < order.size(); ++position)
        {
            if (position != 0)
            {
                m_Out.push_back(',');
            }
            Newline();
            Indent(depth + 1);
            const Size memberIndex = order[position];
            WriteString(members.GetKey(memberIndex));
            // Only the colon separates key from value — the closing quote was already written by WriteString, and
            // repeating it produced invalid JSON ("a"":1) until a round-trip test caught it.
            m_Out += m_Options.IndentSpaces == 0 ? ":" : ": ";
            WriteValue(*members.GetValueAt(memberIndex), depth + 1);
        }
        Newline();
        Indent(depth);
        m_Out.push_back('}');
    }

    void WriteString(std::string_view text)
    {
        m_Out.push_back('"');

        for (Size index = 0; index < text.size(); ++index)
        {
            const unsigned char byte = static_cast<unsigned char>(text[index]);

            switch (byte)
            {
                case '"':
                    m_Out += "\\\"";
                    continue;
                case '\\':
                    m_Out += "\\\\";
                    continue;
                case '\b':
                    m_Out += "\\b";
                    continue;
                case '\f':
                    m_Out += "\\f";
                    continue;
                case '\n':
                    m_Out += "\\n";
                    continue;
                case '\r':
                    m_Out += "\\r";
                    continue;
                case '\t':
                    m_Out += "\\t";
                    continue;
                default:
                    break;
            }

            if (byte < 0x20u)
            {
                // Remaining control characters have no short escape. \\u00XX is required — a raw control byte in the
                // output would make the file invalid JSON and can also smuggle terminal escapes into logs.
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(byte));
                m_Out += buffer;
                continue;
            }

            if (byte < 0x80u)
            {
                m_Out.push_back(static_cast<char>(byte));
                continue;
            }

            // Non-ASCII: either pass the UTF-8 bytes through (default, keeps files readable) or escape each code
            // point. Decoding is needed for escaping, so it happens only in that mode.
            if (!m_Options.EscapeNonAscii)
            {
                m_Out.push_back(static_cast<char>(byte));
                continue;
            }

            Size remaining = text.size() - index;
            UInt32 codePoint = 0;
            Size sequenceLength = 0;

            if (remaining >= 2 && (byte & 0xE0u) == 0xC0u)
            {
                codePoint = byte & 0x1Fu;
                sequenceLength = 2;
            }
            else if (remaining >= 3 && (byte & 0xF0u) == 0xE0u)
            {
                codePoint = byte & 0x0Fu;
                sequenceLength = 3;
            }
            else if (remaining >= 4 && (byte & 0xF8u) == 0xF0u)
            {
                codePoint = byte & 0x07u;
                sequenceLength = 4;
            }
            else
            {
                // Invalid UTF-8 in a string the parser produced should be impossible; emit the replacement character
                // rather than propagating bytes that would corrupt the output file.
                HP_LOG_ERROR("Serialization", "Invalid UTF-8 while writing a JSON string; substituting U+FFFD");
                m_Out += "\\uFFFD";
                continue;
            }

            for (Size continuation = 1; continuation < sequenceLength; ++continuation)
            {
                codePoint = (codePoint << 6) | (static_cast<unsigned char>(text[index + continuation]) & 0x3Fu);
            }
            index += sequenceLength - 1;

            if (codePoint <= 0xFFFFu)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(codePoint));
                m_Out += buffer;
            }
            else
            {
                // Outside the BMP: JSON requires a surrogate pair.
                codePoint -= 0x10000u;
                const UInt32 high = 0xD800u + (codePoint >> 10);
                const UInt32 low = 0xDC00u + (codePoint & 0x3FFu);

                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "\\u%04X\\u%04X", static_cast<unsigned>(high),
                              static_cast<unsigned>(low));
                m_Out += buffer;
            }
        }

        m_Out.push_back('"');
    }

    std::string& m_Out;
    JsonWriteOptions m_Options;
};

}  // namespace

std::string WriteJson(const JsonValue& value, const JsonWriteOptions& options)
{
    // Reserve a rough estimate to avoid repeated reallocation on large documents. Four bytes per node is a low
    // estimate for indented output and a high one for compact arrays of small numbers; either way it removes the
    // first several doublings.
    std::string output;
    output.reserve(options.IndentSpaces == 0 ? (1 + value.GetElementCount() * 4) : (1 + value.GetElementCount() * 16));

    Writer writer(output, options);
    writer.Write(value);

    if (options.TrailingNewline && !output.empty())
    {
        output.push_back('\n');
    }

    return output;
}

std::string WriteCompactJson(const JsonValue& value)
{
    JsonWriteOptions options;
    options.IndentSpaces = 0;
    options.TrailingNewline = false;
    return WriteJson(value, options);
}

// Defined here rather than in JsonValue.cpp so there is exactly one implementation deciding how a value renders.
std::string JsonValue::ToDebugString() const
{
    // Compact and non-escaping: this goes to logs and debugger views, where readability of UTF-8 text matters more
    // than portability, and it must never allocate a second buffer just to be printable.
    return WriteCompactJson(*this);
}

}  // namespace Hollowpoint::Serialization
