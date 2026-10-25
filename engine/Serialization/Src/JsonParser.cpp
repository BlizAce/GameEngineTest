#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Serialization/JsonParser.h"

namespace Hollowpoint::Serialization
{

using Core::Int64;
using Core::UInt32;
using Core::UInt64;

namespace
{

//! Tracks position for error reporting. Line/column are maintained incrementally as characters are consumed, which
//! keeps them exact without a second pass over the buffer (scene files reach tens of megabytes and loading is on the
//! critical path at startup).
class Cursor
{
public:
    Cursor(std::string_view text) noexcept : m_Text(text) {}

    [[nodiscard]] Size GetOffset() const noexcept
    {
        return m_Offset;
    }

    [[nodiscard]] bool AtEnd() const noexcept
    {
        return m_Offset >= m_Text.size();
    }

    [[nodiscard]] char Peek(Size lookahead = 0) const noexcept
    {
        const Size index = m_Offset + lookahead;
        return index < m_Text.size() ? m_Text[index] : '\0';
    }

    //! Advances one character, updating line/column. Newlines inside string escapes still count here because the
    //! escape sequences are two source characters each; only real newline bytes move to a new line.
    void Advance() noexcept
    {
        if (m_Offset >= m_Text.size())
        {
            return;
        }

        const char character = m_Text[m_Offset];
        ++m_Offset;

        if (character == '\n')
        {
            ++m_Line;
            m_Column = 1;
        }
        else if (character == '\r')
        {
            // A CRLF pair counts as one newline: the \n following \r must not advance the line again. Handling this
            // explicitly matters because scene files cross Windows/Linux boundaries constantly in a team.
            if (Peek() == '\n')
            {
                ++m_Offset;
            }
            ++m_Line;
            m_Column = 1;
        }
        else
        {
            ++m_Column;
        }
    }

    void SkipWhitespace() noexcept
    {
        for (;;)
        {
            switch (Peek())
            {
                case ' ':
                case '\t':
                case '\n':
                case '\r':
                    Advance();
                    break;
                default:
                    return;
            }
        }
    }

    [[nodiscard]] std::string_view GetText() const noexcept
    {
        return m_Text;
    }

    [[nodiscard]] Size GetLine() const noexcept
    {
        return m_Line;
    }

    [[nodiscard]] Size GetColumn() const noexcept
    {
        return m_Column;
    }

private:
    std::string_view m_Text;
    Size m_Offset = 0;
    Size m_Line = 1;
    Size m_Column = 1;
};

//! Recursive-descent JSON parser (RFC 8259). Returns false on the first fault and fills the error, never throwing.
class Parser
{
public:
    Parser(std::string_view text, const JsonParseOptions& options) noexcept : m_Cursor(text), m_Options(options) {}

    bool Parse(JsonValue& outRoot, JsonError& outError)
    {
        m_Error = &outError;

        SkipByteOrderMark();
        m_Cursor.SkipWhitespace();

        if (m_Cursor.AtEnd())
        {
            return Fail("unexpected end of input: document is empty");
        }

        if (!ParseValue(outRoot))
        {
            return false;
        }

        // Trailing content after a complete top-level value is an error. Accepting it would let a file containing two
        // concatenated documents parse "successfully" and silently drop the second — a real failure mode when scene
        // files are produced by concatenating or appending saves.
        m_Cursor.SkipWhitespace();
        if (!m_Cursor.AtEnd())
        {
            return Fail("unexpected trailing content after the top-level value");
        }

        return true;
    }

private:
    bool Fail(std::string message)
    {
        return FailAt(m_Cursor.GetOffset(), std::move(message));
    }

    bool FailAt(Size offset, std::string message)
    {
        JsonError& error = *m_Error;
        error.Offset = offset;

        const Position position = PositionOf(offset);
        error.Line = position.Line;
        error.Column = position.Column;

        error.Message = std::move(message);
        error.Context = BuildContext(offset);
        return false;
    }

    //! Line/column for an offset that may be ahead of the cursor (the number scanner determines a token's extent
    //! before moving the cursor there). Counting from the buffer start gives the *first* character of the offending
    //! token, which is what an author wants pointed at; asking the cursor would report wherever scanning stopped.
    struct Position
    {
        Size Line = 1;
        Size Column = 1;
    };

    [[nodiscard]] Position PositionOf(Size offset) const noexcept
    {
        const std::string_view text = m_Cursor.GetText();
        if (offset > text.size())
        {
            offset = text.size();
        }

        Position position;
        for (Size index = 0; index < offset; ++index)
        {
            if (text[index] == '\n')
            {
                ++position.Line;
                position.Column = 1;
            }
            else if (text[index] == '\r')
            {
                // A CRLF pair counts as one newline, matching how the cursor advances.
                if (index + 1 < offset && text[index + 1] == '\n')
                {
                    ++index;
                }
                ++position.Line;
                position.Column = 1;
            }
            else
            {
                ++position.Column;
            }
        }
        return position;
    }

    //! The fault line with a caret underneath, so a log reader can see the problem without opening the file. This is
    //! the difference between "column 22" being actionable and being noise.
    std::string BuildContext(Size offset) const
    {
        const std::string_view text = m_Cursor.GetText();
        if (text.empty())
        {
            return {};
        }

        // Walk back to the start of the line containing the offset.
        Size lineStart = offset;
        while (lineStart > 0 && text[lineStart - 1] != '\n')
        {
            --lineStart;
        }

        Size lineEnd = offset;
        while (lineEnd < text.size() && text[lineEnd] != '\n' && text[lineEnd] != '\r')
        {
            ++lineEnd;
        }

        // Clamp the excerpt: a minified megabyte-long line must not produce a megabyte log entry.
        constexpr Size MaxContextBytes = 120;
        Size excerptStart = lineStart;
        if (lineEnd - lineStart > MaxContextBytes)
        {
            const Size caretColumn = offset - lineStart;
            excerptStart = caretColumn > MaxContextBytes / 2 ? offset - MaxContextBytes / 2 : lineStart;
        }
        const Size excerptEnd = excerptStart + MaxContextBytes < lineEnd ? excerptStart + MaxContextBytes : lineEnd;

        std::string excerpt = std::string(text.substr(excerptStart, excerptEnd - excerptStart));
        std::string caret(offset - excerptStart, ' ');

        std::string result;
        result.reserve(excerpt.size() + caret.size() + 4);
        result += excerpt;
        result += '\n';
        result += caret;
        result += '^';
        return result;
    }

    void SkipByteOrderMark() noexcept
    {
        // A UTF-8 BOM is permitted (though not part of the grammar) by RFC 8259 and Windows editors add one
        // constantly. Rejecting it would break otherwise-valid scene files, so it is skipped rather than documented
        // as unsupported. The byte literals are written via explicit casts because char is signed on x86: comparing a
        // raw '\xEF' against a signed char works, but the intent — three specific *bytes*, not characters — reads
        // clearly this way and does not depend on the execution character set for high-bit values.
        if (m_Cursor.GetOffset() + 3 <= m_Cursor.GetText().size() &&
            static_cast<unsigned char>(m_Cursor.Peek(0)) == 0xEFu &&
            static_cast<unsigned char>(m_Cursor.Peek(1)) == 0xBBu &&
            static_cast<unsigned char>(m_Cursor.Peek(2)) == 0xBFu)
        {
            m_Cursor.Advance();
            m_Cursor.Advance();
            m_Cursor.Advance();
        }
    }

    bool ParseValue(JsonValue& outValue)
    {
        // Depth is tracked here rather than at each call site so no grammar rule can forget it. Nesting depth is a
        // hard limit because this parser recurses once per open bracket: without a bound, a text file containing
        // "[[[[[..." is a stack overflow, and scene files arrive from downloads and version control.
        if (++m_Depth > m_Options.MaxDepth)
        {
            --m_Depth;
            return Fail("nesting deeper than the parser limit");
        }

        bool result = false;
        switch (m_Cursor.Peek())
        {
            case '{':
                result = ParseObject(outValue);
                break;
            case '[':
                result = ParseArray(outValue);
                break;
            case '"':
                result = ParseString(outValue);
                break;
            case 't':
                result = ParseLiteral("true", JsonValue(true), outValue);
                break;
            case 'f':
                result = ParseLiteral("false", JsonValue(false), outValue);
                break;
            case 'n':
                result = ParseLiteral("null", JsonValue(nullptr), outValue);
                break;
            case '-':
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                result = ParseNumber(outValue);
                break;
            default:
            {
                if (m_Cursor.AtEnd())
                {
                    return Fail("unexpected end of input while reading a value");
                }

                const char character = m_Cursor.Peek();
                // Show the byte as hex when it is not printable, so a stray control character or a binary file fed to
                // the parser produces something a reader can identify.
                char buffer[48];
                if (character >= 0x20 && character < 0x7F)
                {
                    std::snprintf(buffer, sizeof(buffer), "unexpected character '%c'", character);
                }
                else
                {
                    std::snprintf(buffer, sizeof(buffer), "unexpected byte 0x%02X",
                                  static_cast<unsigned char>(character));
                }
                return Fail(buffer);
            }
        }

        --m_Depth;
        return result;
    }

    bool ParseLiteral(std::string_view literal, JsonValue value, JsonValue& outValue)
    {
        const std::string_view text = m_Cursor.GetText();
        if (text.substr(m_Cursor.GetOffset(), literal.size()) != literal)
        {
            // Distinguish "tru" at end of input from "trueish": both are errors, but the message should say what was
            // actually found or that input ran out.
            return Fail(std::string("expected '") + std::string(literal) + "'");
        }

        for (Size index = 0; index < literal.size(); ++index)
        {
            m_Cursor.Advance();
        }

        outValue = std::move(value);
        return true;
    }

    bool ParseNumber(JsonValue& outValue)
    {
        const Size startOffset = m_Cursor.GetOffset();
        const std::string_view text = m_Cursor.GetText();

        // ---- Validate the grammar by hand first.
        // from_chars is lenient in ways RFC 8259 is not: it accepts a leading '+', "inf"/"nan", and (in some
        // standard libraries) a leading whitespace or hex form. Feeding it unchecked would accept files that other
        // readers reject, which is how "loads here, fails there" bugs are born. So the token boundary is determined
        // by scanning JSON's own grammar, then converted.
        Size position = startOffset;

        bool negative = false;
        if (position < text.size() && text[position] == '-')
        {
            negative = true;
            ++position;
        }

        // Integer part: '0' alone, or 1-9 followed by digits. "01" is invalid JSON.
        if (position >= text.size())
        {
            return FailAt(startOffset, "number has no digits");
        }
        if (text[position] == '0')
        {
            ++position;
        }
        else if (text[position] >= '1' && text[position] <= '9')
        {
            while (position < text.size() && text[position] >= '0' && text[position] <= '9')
            {
                ++position;
            }
        }
        else
        {
            return FailAt(startOffset, "number must start with a digit");
        }

        bool isReal = false;

        // Fraction.
        if (position < text.size() && text[position] == '.')
        {
            isReal = true;
            ++position;
            if (position >= text.size() || text[position] < '0' || text[position] > '9')
            {
                return FailAt(startOffset, "fraction has no digits after the decimal point");
            }
            while (position < text.size() && text[position] >= '0' && text[position] <= '9')
            {
                ++position;
            }
        }

        // Exponent.
        if (position < text.size() && (text[position] == 'e' || text[position] == 'E'))
        {
            isReal = true;
            ++position;
            if (position < text.size() && (text[position] == '+' || text[position] == '-'))
            {
                ++position;
            }
            if (position >= text.size() || text[position] < '0' || text[position] > '9')
            {
                return FailAt(startOffset, "exponent has no digits");
            }
            while (position < text.size() && text[position] >= '0' && text[position] <= '9')
            {
                ++position;
            }
        }

        const std::string_view token = text.substr(startOffset, position - startOffset);

        // ---- Convert. Integers keep their exactness; reals go through from_chars, which is correctly rounded and
        // does not depend on the C locale (strtod does, so a European locale would read "1,5" as "1").
        if (!isReal)
        {
            // Signed and unsigned literals are converted separately, because from_chars rejects a '-' when the target
            // type is unsigned: feeding it "-17" returns invalid_argument with the value untouched, which silently
            // produced zero here until a test caught it. Sign is known from the grammar scan, so each path converts
            // only what it can represent.
            const char* const first = token.data();
            const char* const last = token.data() + token.size();

            if (negative)
            {
                // Convert the magnitude as unsigned, then negate in a way that handles Int64's extra low value
                // (-2^63 has no positive counterpart, so "negate after cast" would be signed overflow).
                UInt64 magnitude = 0;
                const std::from_chars_result result = std::from_chars(first + 1, last, magnitude);

                if (result.ec != std::errc())
                {
                    return FailAt(startOffset, result.ec == std::errc::result_out_of_range
                                                   ? "integer literal is too small for a 64-bit number"
                                                   : "malformed integer literal");
                }

                constexpr UInt64 AbsInt64Min = static_cast<UInt64>(9223372036854775807ULL) + 1ULL;
                if (magnitude > AbsInt64Min)
                {
                    return FailAt(startOffset, "integer literal is too small for a 64-bit number");
                }

                outValue = magnitude == AbsInt64Min ? JsonValue(std::numeric_limits<Int64>::min())
                                                    : JsonValue(-static_cast<Int64>(magnitude));

                SyncTo(position);
                return true;
            }

            UInt64 magnitude = 0;
            const std::from_chars_result result = std::from_chars(first, last, magnitude);

            if (result.ec != std::errc())
            {
                // Out of range is reported rather than saturated: a silently clamped entity index or asset hash
                // corrupts data in a way no later log line can undo.
                return FailAt(startOffset, result.ec == std::errc::result_out_of_range
                                               ? "integer literal is too large for a 64-bit number"
                                               : "malformed integer literal");
            }

            if (magnitude > static_cast<UInt64>(std::numeric_limits<Int64>::max()))
            {
                // Values in (Int64Max, UInt64Max] are stored unsigned so they round-trip exactly.
                outValue = JsonValue(magnitude);
            }
            else
            {
                outValue = JsonValue(static_cast<Int64>(magnitude));
            }

            SyncTo(position);
            return true;
        }

        double realValue = 0.0;
        const std::from_chars_result realResult = std::from_chars(token.data(), token.data() + token.size(), realValue);

        if (realResult.ec != std::errc() && realResult.ec != std::errc::result_out_of_range)
        {
            return FailAt(startOffset, "malformed number literal");
        }

        if (realResult.ec == std::errc::result_out_of_range)
        {
            // Underflow to zero or overflow to infinity both mean the file cannot be represented. Silently storing
            // inf in a transform would propagate through every matrix multiply downstream and surface as NaN
            // geometry several frames later, where the cause is invisible.
            return FailAt(startOffset, "number is outside the range representable by a double");
        }

        outValue = JsonValue(realValue);
        SyncTo(position);
        return true;
    }

    //! Moves the cursor forward to an absolute offset determined during grammar scanning. Advance() keeps line and
    //! column in step, so error positions stay correct after a long number token.
    void SyncTo(Size targetOffset) noexcept
    {
        while (m_Cursor.GetOffset() < targetOffset)
        {
            m_Cursor.Advance();
        }
    }

    bool ParseString(JsonValue& outValue)
    {
        std::string result;
        if (!ReadString(result))
        {
            return false;
        }
        outValue = JsonValue(std::move(result));
        return true;
    }

    //! Reads one quoted string into `out`, decoding escapes. Shared by values and object keys.
    bool ReadString(std::string& out)
    {
        if (m_Cursor.Peek() != '"')
        {
            return Fail("expected a string");
        }
        m_Cursor.Advance();  // Opening quote.

        out.clear();

        while (true)
        {
            if (m_Cursor.AtEnd())
            {
                return Fail("unterminated string");
            }

            const char character = m_Cursor.Peek();

            if (character == '"')
            {
                m_Cursor.Advance();  // Closing quote.

                if (!ValidateUtf8(out))
                {
                    // Position was already recorded by ValidateUtf8's own Fail call.
                    return false;
                }
                return true;
            }

            if (character == '\\')
            {
                if (!ReadEscape(out))
                {
                    return false;
                }
                continue;
            }

            // Control characters must be escaped inside a string per RFC 8259. A raw newline in a string is nearly
            // always an unterminated string two lines up, and reporting it here points at the real fault instead of
            // accepting garbage.
            if (static_cast<unsigned char>(character) < 0x20)
            {
                return Fail("unescaped control character inside a string");
            }

            out.push_back(character);
            m_Cursor.Advance();
        }
    }

    bool ReadEscape(std::string& out)
    {
        const std::string_view text = m_Cursor.GetText();
        Size position = m_Cursor.GetOffset();

        if (position >= text.size() || text[position] != '\\')
        {
            return Fail("expected an escape sequence");
        }
        ++position;  // Consume the backslash.

        if (position >= text.size())
        {
            m_Cursor.Advance();
            return Fail("input ended inside an escape sequence");
        }

        const char escape = text[position];
        ++position;

        switch (escape)
        {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
            {
                char16_t codeUnit = 0;
                if (!ReadHex4(position, codeUnit))
                {
                    SyncTo(position);
                    return false;
                }

                // A high surrogate must be followed by a low surrogate forming one scalar value. Encoding each
                // half separately produces invalid UTF-8 that downstream string code (and every renderer/text path)
                // has to defend against, so it is rejected at the boundary instead.
                if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF)
                {
                    // Expect a literal \u after the high surrogate.
                    if (position + 5 > text.size() || text[position] != '\\' || text[position + 1] != 'u')
                    {
                        SyncTo(position);
                        return Fail("high surrogate is not followed by a low surrogate");
                    }
                    position += 2;

                    char16_t lowSurrogate = 0;
                    if (!ReadHex4(position, lowSurrogate))
                    {
                        SyncTo(position);
                        return false;
                    }
                    if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF)
                    {
                        SyncTo(position);
                        return Fail("high surrogate is not followed by a low surrogate");
                    }

                    const UInt32 codePoint = 0x10000u + ((static_cast<UInt32>(codeUnit) - 0xD800u) << 10) +
                                             (static_cast<UInt32>(lowSurrogate) - 0xDC00u);
                    AppendUtf8(out, codePoint);
                }
                else if (codeUnit >= 0xDC00 && codeUnit <= 0xDFFF)
                {
                    SyncTo(position);
                    return Fail("lone low surrogate in a string");
                }
                else
                {
                    AppendUtf8(out, static_cast<UInt32>(codeUnit));
                }

                break;
            }
            default:
            {
                char buffer[48];
                std::snprintf(buffer, sizeof(buffer), "invalid escape character '\\%c'", escape);
                SyncTo(position);
                return Fail(buffer);
            }
        }

        SyncTo(position);
        return true;
    }

    bool ReadHex4(Size& position, char16_t& outValue)
    {
        const std::string_view text = m_Cursor.GetText();
        if (position + 4 > text.size())
        {
            return Fail("\\u escape needs four hexadecimal digits");
        }

        UInt32 value = 0;
        for (Size digit = 0; digit < 4; ++digit)
        {
            const char character = text[position + digit];
            UInt32 nibble = 0;

            if (character >= '0' && character <= '9')
            {
                nibble = static_cast<UInt32>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                nibble = static_cast<UInt32>(character - 'a') + 10u;
            }
            else if (character >= 'A' && character <= 'F')
            {
                nibble = static_cast<UInt32>(character - 'A') + 10u;
            }
            else
            {
                // Advance past the digits consumed so far so the reported column points at the bad digit.
                position += digit;
                return Fail("\\u escape contains a non-hexadecimal digit");
            }

            value = (value << 4) | nibble;
        }

        position += 4;
        outValue = static_cast<char16_t>(value);
        return true;
    }

    //! Validates that `bytes` is well-formed UTF-8 (RFC 8259 requires it). Enforces the strict rules: no overlong
    //! encodings, no surrogate code points encoded directly, nothing above U+10FFFF. Doing this at parse time means
    //! every string in the engine can be assumed valid UTF-8, so text rendering and hashing never need their own
    //! guards — and a corrupt asset is named while its file name is still known.
    bool ValidateUtf8(std::string_view bytes)
    {
        Size index = 0;
        while (index < bytes.size())
        {
            const unsigned char lead = static_cast<unsigned char>(bytes[index]);

            if (lead < 0x80u)
            {
                ++index;
                continue;
            }

            Size length = 0;
            UInt32 minimum = 0;

            if (lead >= 0xC2u && lead <= 0xDFu)
            {
                length = 2;
                minimum = 0x80u;
            }
            else if (lead >= 0xE0u && lead <= 0xEFu)
            {
                length = 3;
                minimum = 0x800u;
            }
            else if (lead >= 0xF0u && lead <= 0xF4u)
            {
                length = 4;
                minimum = 0x10000u;
            }
            else
            {
                // 0x80-0xC1 are continuation bytes or overlong leads; 0xF5-0xFF encode values beyond Unicode.
                return Fail("invalid UTF-8 lead byte in a string");
            }

            if (index + length > bytes.size())
            {
                return Fail("UTF-8 sequence truncated at the end of a string");
            }

            UInt32 codePoint = lead & ((1u << (7u - length)) - 1u);
            for (Size continuation = 1; continuation < length; ++continuation)
            {
                const unsigned char next = static_cast<unsigned char>(bytes[index + continuation]);
                if ((next & 0xC0u) != 0x80u)
                {
                    return Fail("invalid UTF-8 continuation byte in a string");
                }
                codePoint = (codePoint << 6) | (next & 0x3Fu);
            }

            if (codePoint < minimum)
            {
                return Fail("overlong UTF-8 encoding in a string");
            }
            if (codePoint >= 0xD800u && codePoint <= 0xDFFFu)
            {
                return Fail("surrogate code point encoded directly in UTF-8");
            }

            index += length;
        }

        return true;
    }

    static void AppendUtf8(std::string& out, UInt32 codePoint)
    {
        // Encodes a Unicode scalar as UTF-8. Surrogates never reach here (rejected by the caller), so every value is
        // a real code point.
        if (codePoint < 0x80u)
        {
            out.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint < 0x800u)
        {
            out.push_back(static_cast<char>(0xC0u | ((codePoint >> 6) & 0x1Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }
        else if (codePoint < 0x10000u)
        {
            out.push_back(static_cast<char>(0xE0u | ((codePoint >> 12) & 0x0Fu)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0u | ((codePoint >> 18) & 0x07u)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }
    }

    bool ParseArray(JsonValue& outValue)
    {
        m_Cursor.Advance();  // '['
        m_Cursor.SkipWhitespace();

        JsonArray elements;

        if (m_Cursor.Peek() == ']')
        {
            m_Cursor.Advance();
            outValue = JsonValue(std::move(elements));
            return true;
        }

        while (true)
        {
            m_Cursor.SkipWhitespace();

            if (m_Cursor.AtEnd())
            {
                return Fail("unterminated array");
            }

            // A trailing comma before ']' is invalid JSON. It is rejected rather than tolerated because parsers that
            // accept it produce files other tools refuse to read, and because the mistake usually means an element
            // was deleted and left a hole the author did not notice.
            if (m_Cursor.Peek() == ']')
            {
                return Fail("trailing comma in array");
            }

            JsonValue element;
            if (!ParseValue(element))
            {
                return false;
            }
            elements.push_back(std::move(element));

            m_Cursor.SkipWhitespace();
            const char next = m_Cursor.Peek();

            if (next == ',')
            {
                m_Cursor.Advance();
                continue;
            }

            if (next == ']')
            {
                m_Cursor.Advance();
                break;
            }

            if (m_Cursor.AtEnd())
            {
                return Fail("unterminated array: expected ',' or ']'");
            }

            return Fail("expected ',' or ']' in array");
        }

        outValue = JsonValue(std::move(elements));
        return true;
    }

    bool ParseObject(JsonValue& outValue)
    {
        m_Cursor.Advance();  // '{'
        m_Cursor.SkipWhitespace();

        JsonObject members;

        if (m_Cursor.Peek() == '}')
        {
            m_Cursor.Advance();
            outValue = JsonValue(std::move(members));
            return true;
        }

        while (true)
        {
            m_Cursor.SkipWhitespace();

            if (m_Cursor.AtEnd())
            {
                return Fail("unterminated object");
            }
            if (m_Cursor.Peek() == '}')
            {
                return Fail("trailing comma in object");
            }
            if (m_Cursor.Peek() != '"')
            {
                return Fail("expected a quoted member name");
            }

            std::string key;
            if (!ReadString(key))
            {
                return false;
            }

            m_Cursor.SkipWhitespace();
            if (m_Cursor.Peek() != ':')
            {
                return Fail("expected ':' after a member name");
            }
            m_Cursor.Advance();
            m_Cursor.SkipWhitespace();

            JsonValue value;
            if (!ParseValue(value))
            {
                return false;
            }

            // Duplicate keys: RFC 8259 leaves the meaning of duplicates to the implementation. Last-one-wins matches
            // every mainstream reader, so that is the behaviour — but it is logged, because a duplicate in a scene
            // file means one of the two definitions is dead and someone will spend an afternoon finding out which.
            if (members.Contains(key))
            {
                HP_LOG_WARNING("Serialization", "JSON object has a duplicate member '{}'; the last value wins", key);
            }

            members.Set(std::move(key), std::move(value));

            m_Cursor.SkipWhitespace();
            const char next = m_Cursor.Peek();

            if (next == ',')
            {
                m_Cursor.Advance();
                continue;
            }

            if (next == '}')
            {
                m_Cursor.Advance();
                break;
            }

            if (m_Cursor.AtEnd())
            {
                return Fail("unterminated object: expected ',' or '}'");
            }

            return Fail("expected ',' or '}' in object");
        }

        outValue = JsonValue(std::move(members));
        return true;
    }

    Cursor m_Cursor;
    JsonParseOptions m_Options;
    Size m_Depth = 0;
    JsonError* m_Error = nullptr;
};

}  // namespace

std::string JsonError::Format(const std::string& sourceName) const
{
    std::string result;
    result.reserve(Message.size() + sourceName.size() + 32);

    if (!sourceName.empty())
    {
        result += sourceName;
        result += " (";
        result += std::to_string(Line);
        result += ':';
        result += std::to_string(Column);
        result += "): ";
    }
    else
    {
        result += '(';
        result += std::to_string(Line);
        result += ':';
        result += std::to_string(Column);
        result += "): ";
    }

    result += Message;
    return result;
}

JsonDocument ParseJson(std::string_view text, const JsonParseOptions& options)
{
    JsonDocument document;

    Parser parser(text, options);
    document.Success = parser.Parse(document.Root, document.Error);

    if (!document.Success)
    {
        // The tree is unspecified on failure. Clearing it makes accidental use of a half-built document a null read
        // rather than plausible-looking wrong data.
        document.Root = JsonValue(nullptr);
    }

    return document;
}

bool TryParseJson(std::string_view text, JsonValue& outRoot, JsonError* outError, const JsonParseOptions& options)
{
    JsonDocument document = ParseJson(text, options);

    if (outError != nullptr)
    {
        *outError = document.Error;
    }

    if (!document.Success)
    {
        return false;
    }

    outRoot = std::move(document.Root);
    return true;
}

}  // namespace Hollowpoint::Serialization
