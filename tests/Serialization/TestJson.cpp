#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Serialization/JsonParser.h"
#include "Hollowpoint/Serialization/JsonValue.h"
#include "Hollowpoint/Serialization/JsonWriter.h"

using namespace Hollowpoint::Serialization;
using Hollowpoint::Core::Index;
using Hollowpoint::Core::Int64;
using Hollowpoint::Core::Size;
using Hollowpoint::Core::UInt64;

namespace
{

JsonValue ParseOk(std::string_view text)
{
    JsonValue root;
    JsonError error;
    const bool ok = TryParseJson(text, root, &error);
    INFO("parse error: " << error.Format("<test>"));
    REQUIRE(ok);
    return root;
}

//! Counts bracket nesting in written JSON and checks it fully closed. Used by the deep-document test so the
//! assertion is about the produced structure rather than a byte-for-byte string.
bool WrittenMatchesExpectedDepth(const std::string& text, int expectedDepth)
{
    int depth = 0;
    int maxDepth = 0;
    for (const char character : text)
    {
        if (character == '[')
        {
            ++depth;
            maxDepth = depth > maxDepth ? depth : maxDepth;
        }
        else if (character == ']')
        {
            --depth;
        }
    }
    return depth == 0 && maxDepth == expectedDepth;
}

JsonError ParseFails(std::string_view text)
{
    JsonValue root;
    JsonError error;
    const bool ok = TryParseJson(text, root, &error);
    CHECK_FALSE(ok);
    // On failure the root must not look like usable data.
    CHECK(root.IsNull());
    return error;
}

}  // namespace

TEST_SUITE("Serialization/JsonValue")
{
    TEST_CASE("default value is null")
    {
        const JsonValue value;
        CHECK(value.GetKind() == JsonValue::Kind::Null);
        CHECK(value.IsNull());
        CHECK_FALSE(value.IsNumber());
        CHECK(value.GetElementCount() == 0u);
    }

    TEST_CASE("kinds are preserved exactly")
    {
        CHECK(JsonValue(true).IsBoolean());
        CHECK(JsonValue(42).IsInt());
        CHECK(JsonValue(42u).IsUInt());
        CHECK(JsonValue(42.5).IsReal());
        CHECK(JsonValue("text").IsString());
        CHECK(JsonValue(MakeArray()).IsArray());
        CHECK(JsonValue(MakeObject()).IsObject());
        CHECK(JsonValue(nullptr).IsNull());
    }

    TEST_CASE("integer width chooses signed or unsigned storage")
    {
        // Values above Int64's maximum must not round-trip through a double, which would change them.
        const JsonValue big{UInt64{18446744073709551615ULL}};
        CHECK(big.IsUInt());
        CHECK(big.AsUInt() == 18446744073709551615ULL);

        const JsonValue small{Int64{-1}};
        CHECK(small.IsInt());
        CHECK(small.AsInt() == -1);
    }

    TEST_CASE("kind mismatches return the caller's fallback without side effects")
    {
        const JsonValue text("hello");

        // A string is not a bool/number/container, so each accessor yields its fallback.
        CHECK(text.AsBool(true) == true);
        CHECK(text.IsString());  // ...but the value itself is still a string.
        CHECK(text.AsInt(-7) == -7);
        CHECK(text.AsUInt(9) == 9u);
        CHECK(text.AsDouble(3.5) == 3.5);
        CHECK(text.AsArray().empty());
        CHECK(text.AsObject().IsEmpty());

        // The fallbacks must be independent per call (no state carried between them).
        CHECK(text.AsInt(1) == 1);
        CHECK(text.AsInt(2) == 2);

        // Non-strings yield the shared empty string/array rather than dangling or allocating.
        const JsonValue number{Int64{5}};
        CHECK(number.AsString().empty());
        CHECK(number.AsArray().empty());
        CHECK(number.AsObject().IsEmpty());
    }

    TEST_CASE("numeric access coerces in the documented direction only")
    {
        const JsonValue integer{Int64{5}};
        CHECK(integer.AsDouble() == 5.0);  // Integer widens to real.

        const JsonValue real{5.0};
        CHECK(real.AsInt() == 5);  // Integral real reads as integer (writers emit 5 and 5.0 interchangeably).

        const JsonValue fractional{2.7};
        CHECK(fractional.AsInt(-1) == 2);  // Truncation is documented; callers check IsInt() to avoid it.

        const JsonValue negative{Int64{-5}};
        CHECK(negative.AsUInt(0) == 0u);  // Negative never wraps into a huge unsigned.
        CHECK(negative.AsUInt(77) == 77u);
    }

    TEST_CASE("object insertion order is preserved and lookup works")
    {
        JsonObject object;
        object.Set("zeta", JsonValue(1));
        object.Set("alpha", JsonValue(2));
        object.Set("mid", JsonValue(3));

        CHECK(object.GetCount() == 3u);
        CHECK(object.GetKey(0) == "zeta");
        CHECK(object.GetKey(1) == "alpha");
        CHECK(object.GetKey(2) == "mid");

        REQUIRE(object.Find("alpha") != nullptr);
        CHECK(object.Find("alpha")->AsInt() == 2);
        CHECK(object.Find("absent") == nullptr);
        CHECK(object.Contains("mid"));
        CHECK_FALSE(object.Contains("nope"));
    }

    TEST_CASE("replacing a member keeps its position")
    {
        // This is what makes "load a scene, tweak one field, save" a one-line diff.
        JsonObject object;
        object.Set("first", JsonValue(1));
        object.Set("second", JsonValue(2));
        object.Set("third", JsonValue(3));

        object.Set("second", JsonValue(99));

        CHECK(object.GetCount() == 3u);
        CHECK(object.GetKey(0) == "first");
        CHECK(object.GetKey(1) == "second");
        CHECK(object.GetKey(2) == "third");
        CHECK(object.Find("second")->AsInt() == 99);
    }

    TEST_CASE("removal keeps the order of what remains")
    {
        JsonObject object;
        object.Set("a", JsonValue(1));
        object.Set("b", JsonValue(2));
        object.Set("c", JsonValue(3));

        CHECK(object.Remove("b"));
        CHECK_FALSE(object.Remove("b"));

        CHECK(object.GetCount() == 2u);
        CHECK(object.GetKey(0) == "a");
        CHECK(object.GetKey(1) == "c");
    }

    TEST_CASE("array element access is bounds-checked")
    {
        JsonValue array = MakeArray(JsonArray{JsonValue(1), JsonValue(2), JsonValue(3)});

        REQUIRE(array.FindElement(0) != nullptr);
        CHECK(array.FindElement(0)->AsInt() == 1);
        CHECK(array.FindElement(2)->AsInt() == 3);
        CHECK(array.FindElement(3) == nullptr);
        CHECK(array.FindElement(Index{9999}) == nullptr);
        CHECK(array.GetElementCount() == 3u);
    }

    TEST_CASE("mutating access converts in place")
    {
        JsonValue value(42);
        value.AsArrayMutable().push_back(JsonValue(1));
        CHECK(value.IsArray());
        CHECK(value.AsArray().size() == 1u);

        JsonValue other("text");
        other.AsObjectMutable().Set("key", JsonValue(true));
        CHECK(other.IsObject());
        CHECK(other.FindMember("key")->AsBool() == true);
    }

    TEST_CASE("equality compares numbers by value across kinds")
    {
        CHECK(JsonValue(1) == JsonValue(1.0));
        CHECK(JsonValue(1u) == JsonValue(1));
        CHECK(JsonValue(1) == JsonValue(1u));
        CHECK_FALSE(JsonValue(-1) == JsonValue(UInt64{1}));  // Negative never equals an unsigned.

        CHECK(JsonValue("1") != JsonValue(1));   // Strings are not numbers.
        CHECK(JsonValue(true) != JsonValue(1));  // Booleans are not numbers.
    }

    TEST_CASE("equality keeps precision above 2^53")
    {
        // Comparing through double would make these two different values look equal.
        const JsonValue a{Int64{9007199254740993LL}};
        const JsonValue b{Int64{9007199254740992LL}};
        CHECK(a != b);
    }

    TEST_CASE("object equality ignores member order")
    {
        JsonObject left;
        left.Set("a", JsonValue(1));
        left.Set("b", JsonValue(2));

        JsonObject right;
        right.Set("b", JsonValue(2));
        right.Set("a", JsonValue(1));

        CHECK(JsonValue(std::move(left)) == JsonValue(std::move(right)));
    }

    TEST_CASE("objects with different member counts or values differ")
    {
        JsonObject left;
        left.Set("a", JsonValue(1));

        JsonObject missingDifferentCount;
        missingDifferentCount.Set("a", JsonValue(1));
        missingDifferentCount.Set("b", JsonValue(2));

        CHECK(JsonValue(left) != JsonValue(missingDifferentCount));

        JsonObject differentValue;
        differentValue.Set("a", JsonValue(2));
        CHECK(JsonValue(left) != JsonValue(differentValue));
    }

    TEST_CASE("nested structures compare structurally")
    {
        const std::string text = R"({"a":[1,2,{"b":"c"}],"d":{"e":[true,null]}})";
        const JsonValue parsed = ParseOk(text);

        REQUIRE(parsed.IsObject());
        const JsonValue* array = parsed.FindMember("a");
        REQUIRE(array != nullptr);
        CHECK(array->GetElementCount() == 3u);

        const JsonValue* nested = array->FindElement(2);
        REQUIRE(nested != nullptr);
        CHECK(nested->FindMember("b")->AsString() == "c");
    }

    TEST_CASE("Clear and the Set* mutators reset a node's contents")
    {
        JsonObject object;
        object.Set("a", JsonValue(1));
        object.Set("b", JsonValue(2));
        CHECK(object.GetCount() == 2u);

        object.Clear();
        CHECK(object.GetCount() == 0u);
        CHECK(object.IsEmpty());
        CHECK(object.Find("a") == nullptr);

        // Reusable afterwards.
        object.Set("c", JsonValue(3));
        CHECK(object.GetCount() == 1u);

        JsonValue arrayNode = MakeArray(JsonArray{JsonValue(1), JsonValue(2)});
        arrayNode.SetArray(JsonArray{JsonValue(9)});
        CHECK(arrayNode.GetElementCount() == 1u);
        CHECK(arrayNode.FindElement(0)->AsInt() == 9);

        arrayNode.SetObject(JsonObject{});
        CHECK(arrayNode.IsObject());
        CHECK(arrayNode.GetElementCount() == 0u);

        arrayNode.SetNull();
        CHECK(arrayNode.IsNull());
    }

    TEST_CASE("KindName covers every enumerator")
    {
        using Kind = JsonValue::Kind;
        CHECK(std::string(JsonValue::KindName(Kind::Null)) == "null");
        CHECK(std::string(JsonValue::KindName(Kind::Boolean)) == "boolean");
        CHECK(std::string(JsonValue::KindName(Kind::Int)) == "int");
        CHECK(std::string(JsonValue::KindName(Kind::UInt)) == "uint");
        CHECK(std::string(JsonValue::KindName(Kind::Real)) == "real");
        CHECK(std::string(JsonValue::KindName(Kind::String)) == "string");
        CHECK(std::string(JsonValue::KindName(Kind::Array)) == "array");
        CHECK(std::string(JsonValue::KindName(Kind::Object)) == "object");
    }

    TEST_CASE("GetElementCount reports the size of whatever container holds")
    {
        CHECK(JsonValue("abcd").GetElementCount() == 4u);
        CHECK(MakeArray(JsonArray{JsonValue(1), JsonValue(2)}).GetElementCount() == 2u);
        CHECK(JsonValue(true).GetElementCount() == 0u);
        CHECK(JsonValue(nullptr).GetElementCount() == 0u);
    }

    TEST_CASE("FindMember on a non-object returns null")
    {
        CHECK(JsonValue(5).FindMember("x") == nullptr);
        CHECK(JsonValue(MakeArray()).FindMember("x") == nullptr);
    }
}

TEST_SUITE("Serialization/JsonParser")
{
    TEST_CASE("parses every scalar type")
    {
        CHECK(ParseOk("true").AsBool() == true);
        CHECK(ParseOk("false").AsBool() == false);
        CHECK(ParseOk("null").IsNull());
        CHECK(ParseOk("42").AsInt() == 42);
        CHECK(ParseOk("-17").AsInt() == -17);
        CHECK(ParseOk("3.5").AsDouble() == 3.5);
        CHECK(ParseOk("\"hi\"").AsString() == "hi");
    }

    TEST_CASE("parses nested objects and arrays")
    {
        const JsonValue root = ParseOk(R"({"list":[1,2,3],"obj":{"k":"v"}})");
        CHECK(root.FindMember("list")->GetElementCount() == 3u);
        CHECK(root.FindMember("obj")->FindMember("k")->AsString() == "v");
    }

    TEST_CASE("empty containers parse to empty")
    {
        CHECK(ParseOk("[]").AsArray().empty());
        CHECK(ParseOk("{}").AsObject().IsEmpty());
        CHECK(ParseOk("[[]]").FindElement(0)->AsArray().empty());
    }

    TEST_CASE("whitespace and newlines are insignificant")
    {
        const std::string text = "\n\t {  \"a\" : [ 1 ,\r\n 2 ] }  \n";
        const JsonValue root = ParseOk(text);
        CHECK(root.FindMember("a")->GetElementCount() == 2u);
    }

    TEST_CASE("a UTF-8 BOM is accepted")
    {
        // Editors on Windows add one constantly; rejecting it would break otherwise-valid scene files.
        const std::string text = std::string("\xEF\xBB\xBF") + R"({"ok":true})";
        const JsonValue root = ParseOk(text);
        CHECK(root.FindMember("ok")->AsBool() == true);
    }

    TEST_CASE("CRLF line endings do not double-count lines")
    {
        // Error positions must match what an editor shows; counting \r\n as two lines points at the wrong place.
        const std::string text = "{\r\n  \"a\": 1,\r\n  bad\r\n}";
        const JsonError error = ParseFails(text);
        CHECK(error.Line == 3);
    }

    TEST_CASE("escape sequences decode correctly")
    {
        const JsonValue root = ParseOk(R"("quote:\" back:\\ slash:\/ bs:\b ff:\f nl:\n cr:\r tab:\t")");
        CHECK(root.AsString() == "quote:\" back:\\ slash:/ bs:\b ff:\f nl:\n cr:\r tab:\t");
    }

    TEST_CASE("unicode escapes decode to UTF-8")
    {
        CHECK(ParseOk(R"("\u0041")").AsString() == "A");
        CHECK(ParseOk(R"("\u20AC")").AsString() == "\xE2\x82\xAC");  // Euro sign.
        CHECK(ParseOk(R"("\u0000tail")").AsString().size() == 5u);   // A NUL inside a string is legal via escape.
    }

    TEST_CASE("surrogate pairs decode to one code point")
    {
        // U+1F600 grinning face: D83D DE00.
        const std::string decoded = ParseOk(R"("\uD83D\uDE00")").AsString();
        CHECK(decoded == "\xF0\x9F\x98\x80");
    }

    TEST_CASE("lone and mismatched surrogates are rejected")
    {
        // Encoding half a pair produces invalid UTF-8 that every consumer downstream would have to defend against.
        ParseFails(R"("\uD83D")");
        ParseFails(R"("\uDE00")");
        ParseFails(R"("\uD83D\u0041")");
        ParseFails(R"("\uD83Dx\")");
    }

    TEST_CASE("invalid escapes and hex are rejected with useful messages")
    {
        const JsonError badEscape = ParseFails(R"("\q")");
        CHECK(badEscape.Message.find("escape") != std::string::npos);

        const JsonError badHex = ParseFails(R"("\uZZZZ")");
        CHECK(badHex.Message.find("hexadecimal") != std::string::npos);

        ParseFails(R"("\u12")");
    }

    TEST_CASE("raw control characters inside strings are rejected")
    {
        // Nearly always an unterminated string two lines up; reporting it here points at the real fault.
        ParseFails(std::string("\"abc\ndef\""));
        ParseFails(std::string("\"tab\there\""));
    }

    TEST_CASE("unterminated strings and containers are reported")
    {
        CHECK(ParseFails("\"abc").Message.find("unterminated string") != std::string::npos);
        CHECK(ParseFails("[1,2").Message.find("unterminated array") != std::string::npos);
        CHECK(ParseFails("{\"a\":1").Message.find("unterminated object") != std::string::npos);
    }

    TEST_CASE("trailing commas are rejected")
    {
        // Tolerating them produces files other readers refuse, and usually means a deleted element left a hole.
        CHECK(ParseFails("[1,2,]").Message.find("trailing comma") != std::string::npos);
        CHECK(ParseFails("{\"a\":1,}").Message.find("trailing comma") != std::string::npos);
    }

    TEST_CASE("structural errors report offset line and column")
    {
        const std::string text = "{\n  \"a\": 1,\n  \"b\" 2\n}";
        const JsonError error = ParseFails(text);

        // The fault is the missing ':' — pointed at exactly where it should be, so an author can fix the line without
        // recounting columns by hand.
        CHECK(error.Line == 3);
        CHECK(error.Column == 7u);
        CHECK(error.Offset > 0u);
        CHECK_FALSE(error.Message.empty());

        // The context excerpt contains a caret under the fault so a log reader can see it without opening the file.
        CHECK(error.Context.find('^') != std::string::npos);
        CHECK(error.Context.find("\"b\"") != std::string::npos);
    }

    TEST_CASE("number errors point at the start of the offending token")
    {
        // The number scanner determines a token's extent before moving the cursor, so reporting at the cursor would
        // point past the value (or at whatever follows it). Authors need the token itself.
        const JsonError overflow = ParseFails("[\n  1e999,\n  2\n]");
        CHECK(overflow.Line == 2);
        CHECK(overflow.Column == 3u);

        const JsonError tooBig = ParseFails("{\"hash\": 99999999999999999999999}");
        CHECK(tooBig.Line == 1);
        CHECK(tooBig.Message.find("too large") != std::string::npos);
    }

    TEST_CASE("error formatting includes position when a name is given")
    {
        JsonError error;
        error.Line = 413;
        error.Column = 22;
        error.Message = "expected ','";
        CHECK(error.Format("level.scene") == "level.scene (413:22): expected ','");
        CHECK(error.Format("") == "(413:22): expected ','");
    }

    TEST_CASE("context stays short for minified input")
    {
        // A megabyte single-line file must not produce a megabyte log entry.
        std::string text = "[";
        text += std::string(5000, '1');
        text += "x]";

        const JsonError error = ParseFails(text);
        CHECK(error.Context.size() < 200u);
    }

    TEST_CASE("number grammar is strict")
    {
        ParseFails("01");   // Leading zeros are not JSON.
        ParseFails("+1");   // No leading plus.
        ParseFails(".5");   // Integer part required.
        ParseFails("1.");   // Fraction digits required.
        ParseFails("1e");   // Exponent digits required.
        ParseFails("1e+");  // Exponent digits required.
        ParseFails("-");    // No digits at all.
        ParseFails("--1");  // One sign only.
        ParseFails("1a");   // Junk after the number is trailing content.
    }

    TEST_CASE("non-standard literals are rejected")
    {
        // A parser that accepts these produces files other tools refuse to read.
        ParseFails("NaN");
        ParseFails("Infinity");
        ParseFails("-Infinity");
        ParseFails("tru");
        ParseFails("nulll");
        ParseFails("TRUE");
    }

    TEST_CASE("numbers at the representable limits")
    {
        CHECK(ParseOk("9223372036854775807").AsInt() == std::numeric_limits<Int64>::max());
        CHECK(ParseOk("-9223372036854775808").AsInt() == std::numeric_limits<Int64>::min());
        CHECK(ParseOk("18446744073709551615").AsUInt() == std::numeric_limits<UInt64>::max());

        // The value above Int64's max must be stored unsigned so it round-trips exactly.
        const JsonValue big = ParseOk("18446744073709551615");
        CHECK(big.IsUInt());

        // Beyond 64 bits is an error rather than a silent clamp: a clamped entity index or hash corrupts data in a
        // way no later log line can undo.
        const JsonError tooBig = ParseFails("99999999999999999999999");
        CHECK(tooBig.Message.find("too large") != std::string::npos);
    }

    TEST_CASE("out-of-range reals are rejected rather than turned into infinity")
    {
        // inf in a transform propagates through every matrix multiply and surfaces as NaN geometry frames later.
        const JsonError overflow = ParseFails("1e999");
        CHECK(overflow.Message.find("outside the range") != std::string::npos);

        // A subnormal that underflows to zero is representable, so it parses (the value simply is tiny).
        CHECK(ParseOk("1e-320").IsReal());
    }

    TEST_CASE("negative integers parse to their value, not zero")
    {
        // from_chars rejects a leading '-' when the target type is unsigned and leaves the output untouched. The
        // first implementation fed signed literals to an unsigned conversion and ignored invalid_argument, so every
        // negative number silently became 0 — caught only because these values were checked, not just parsed.
        CHECK(ParseOk("-1").AsInt() == -1);
        CHECK(ParseOk("-17").AsInt() == -17);
        CHECK(ParseOk("-9223372036854775808").AsInt() == std::numeric_limits<Int64>::min());
        CHECK(ParseOk("-0").IsInt());
        CHECK(ParseOk("-0").AsInt(99) == 0);
        CHECK(ParseOk("-1.5").AsDouble() == -1.5);

        // Magnitudes beyond the signed range are still errors rather than wraps.
        ParseFails("-99999999999999999999999");
    }

    TEST_CASE("integral doubles keep their kind through a save/load cycle")
    {
        // A whole-number float written as "3" comes back as an integer. The numeric value survives but the kind does
        // not, which changes how the next reader interprets the field and churns diffs on every save.
        const std::vector<double> integralReals = {0.0, 1.0, -0.0, 42.0, 1e15, -7.0};

        for (const double value : integralReals)
        {
            const std::string text = WriteCompactJson(JsonValue(value));
            INFO("written as " << text);

            const JsonValue parsed = ParseOk(text);
            CHECK(parsed.IsReal());
            CHECK(parsed.AsDouble() == value);
        }
    }

    TEST_CASE("exponent forms parse identically regardless of case and sign")
    {
        CHECK(ParseOk("1.5e2").AsDouble() == 150.0);
        CHECK(ParseOk("1.5E2").AsDouble() == 150.0);
        CHECK(ParseOk("1.5e+2").AsDouble() == 150.0);
        CHECK(ParseOk("1.5e-2").AsDouble() == 0.015);
    }

    TEST_CASE("negative zero keeps its sign")
    {
        const JsonValue value = ParseOk("-0.0");
        REQUIRE(value.IsReal());
        CHECK(std::signbit(value.AsDouble()));
    }

    TEST_CASE("nesting depth is bounded so a text file cannot exhaust the stack")
    {
        // Without a limit, "[[[[[..." from a download or a corrupt asset is a remote stack overflow.
        JsonParseOptions options;
        options.MaxDepth = 8;

        JsonValue root;
        JsonError error;
        const std::string tooDeep(50, '[');
        CHECK_FALSE(TryParseJson(tooDeep, root, &error, options));
        CHECK(error.Message.find("nesting") != std::string::npos);

        // Within the limit it parses.
        const std::string okDepth = std::string(6, '[') + std::string(6, ']');
        CHECK(TryParseJson(okDepth, root, &error, options));
    }

    TEST_CASE("trailing content after the top-level value is an error")
    {
        // Accepting it would let two concatenated documents parse "successfully" and silently drop the second.
        const JsonError trailing = ParseFails(R"({"a":1} {"b":2})");
        CHECK(trailing.Message.find("trailing content") != std::string::npos);

        ParseFails("[1,2] extra");
        ParseFails("123abc");
    }

    TEST_CASE("empty and whitespace-only input is reported as empty")
    {
        CHECK(ParseFails("").Message.find("empty") != std::string::npos);
        CHECK(ParseFails("   \n\t ").Message.find("empty") != std::string::npos);
    }

    TEST_CASE("unquoted and missing member names are rejected")
    {
        CHECK(ParseFails("{a:1}").Message.find("quoted member name") != std::string::npos);
        CHECK(ParseFails(R"({"a" 1})").Message.find("':'") != std::string::npos);
    }

    TEST_CASE("ParseJson exposes the same contract as TryParseJson")
    {
        const JsonDocument ok = ParseJson("[1,2]");
        CHECK(ok.Success);
        CHECK(ok.Root.GetElementCount() == 2u);
        CHECK(*ok == ParseOk("[1,2]"));

        const JsonDocument bad = ParseJson("[1,");
        CHECK_FALSE(bad.Success);
        CHECK(bad.Root.IsNull());
        CHECK_FALSE(bad.Error.Message.empty());
    }
}

TEST_SUITE("Serialization/JsonWriter")
{
    TEST_CASE("scalars write in JSON's own spelling")
    {
        JsonWriteOptions options;
        options.IndentSpaces = 0;
        options.TrailingNewline = false;

        CHECK(WriteJson(JsonValue(nullptr), options) == "null");
        CHECK(WriteJson(JsonValue(true), options) == "true");
        CHECK(WriteJson(JsonValue(false), options) == "false");
        CHECK(WriteJson(JsonValue(Int64{42}), options) == "42");
        CHECK(WriteJson(JsonValue(Int64{-7}), options) == "-7");
        CHECK(WriteJson(JsonValue("hi"), options) == "\"hi\"");
        CHECK(WriteJson(JsonValue(0.5), options) == "0.5");
    }

    TEST_CASE("empty containers write compactly even when indenting")
    {
        CHECK(WriteJson(MakeArray(), JsonWriteOptions{}) == "[]\n");
        CHECK(WriteJson(MakeObject(), JsonWriteOptions{}) == "{}\n");
    }

    TEST_CASE("indented output is readable and diffs per field")
    {
        JsonObject inner;
        inner.Set("x", JsonValue(1));

        JsonObject root;
        root.Set("name", JsonValue("crate"));
        root.Set("children", JsonValue(MakeArray(JsonArray{JsonValue(std::move(inner))})));

        const std::string text = WriteJson(JsonValue(std::move(root)), JsonWriteOptions{});
        CHECK(text ==
              "{\n"
              "  \"name\": \"crate\",\n"
              "  \"children\": [\n"
              "    {\n"
              "      \"x\": 1\n"
              "    }\n"
              "  ]\n"
              "}\n");
    }

    TEST_CASE("compact output has no insignificant whitespace")
    {
        JsonObject object;
        object.Set("a", JsonValue(1));
        object.Set("b", JsonValue(MakeArray(JsonArray{JsonValue(2)})));

        CHECK(WriteCompactJson(JsonValue(std::move(object))) == "{\"a\":1,\"b\":[2]}");
    }

    TEST_CASE("strings are escaped so output is always valid JSON")
    {
        const std::string tricky = "quote\" back\\ slash/ new\n tab\t cr\r bell\x01 high\x7F";
        const std::string written = WriteCompactJson(JsonValue(tricky));

        CHECK(written.find("\\\"") != std::string::npos);
        CHECK(written.find("\\\\") != std::string::npos);
        CHECK(written.find("\\n") != std::string::npos);
        CHECK(written.find("\\t") != std::string::npos);
        CHECK(written.find("\\r") != std::string::npos);
        CHECK(written.find("\\u0001") != std::string::npos);

        // A raw control byte in the output would be invalid JSON and could smuggle terminal escapes into logs, so
        // nothing below 0x20 may appear outside an escape sequence.
        for (const char character : written)
        {
            CHECK(static_cast<unsigned char>(character) >= 0x20u);
        }
    }

    TEST_CASE("non-ASCII passes through by default and escapes on request")
    {
        const std::string euro = "\xE2\x82\xAC";

        JsonWriteOptions passthrough;
        passthrough.IndentSpaces = 0;
        passthrough.TrailingNewline = false;
        CHECK(WriteJson(JsonValue(euro), passthrough) == "\"\xE2\x82\xAC\"");

        JsonWriteOptions escaped = passthrough;
        escaped.EscapeNonAscii = true;
        CHECK(WriteJson(JsonValue(euro), escaped) == "\"\\u20AC\"");
    }

    TEST_CASE("astral code points escape as surrogate pairs")
    {
        JsonWriteOptions options;
        options.IndentSpaces = 0;
        options.TrailingNewline = false;
        options.EscapeNonAscii = true;

        // U+1F600 -> D83D DE00, which is what JSON requires outside the BMP.
        CHECK(WriteJson(JsonValue("\xF0\x9F\x98\x80"), options) == "\"\\uD83D\\uDE00\"");
    }

    TEST_CASE("integers never pass through a double")
    {
        JsonWriteOptions options;
        options.IndentSpaces = 0;
        options.TrailingNewline = false;

        // Above 2^53 a double cannot represent every integer, so this only works if written from the integer.
        CHECK(WriteJson(JsonValue(Int64{9007199254740993LL}), options) == "9007199254740993");
        CHECK(WriteJson(JsonValue(UInt64{18446744073709551615ULL}), options) == "18446744073709551615");
    }

    TEST_CASE("reals write as the shortest text that parses back exactly")
    {
        JsonWriteOptions options;
        options.IndentSpaces = 0;
        options.TrailingNewline = false;

        CHECK(WriteJson(JsonValue(0.1), options) == "0.1");
        CHECK(WriteJson(JsonValue(-2.5), options) == "-2.5");
        CHECK(WriteJson(JsonValue(1.0 / 3.0), options) == "0.3333333333333333");

        // Padding every number with float noise is the failure mode this prevents; it is also what keeps saved
        // scenes from churning in version control.
        CHECK(WriteJson(JsonValue(0.1), options).find("0.10000000000000001") == std::string::npos);
    }

    TEST_CASE("non-finite values cannot be written and say so")
    {
        // JSON has no literal for these. Writing null keeps the file valid; silently writing 0 would hide an
        // upstream bug, so the writer logs instead of hiding it.
        const std::string nan = WriteCompactJson(JsonValue(std::numeric_limits<double>::quiet_NaN()));
        const std::string inf = WriteCompactJson(JsonValue(std::numeric_limits<double>::infinity()));

        CHECK(nan == "null");
        CHECK(inf == "null");
    }

    TEST_CASE("sorting keys is opt-in and stable")
    {
        JsonObject object;
        object.Set("zebra", JsonValue(1));
        object.Set("apple", JsonValue(2));
        object.Set("mango", JsonValue(3));

        JsonWriteOptions sorted;
        sorted.IndentSpaces = 0;
        sorted.TrailingNewline = false;
        sorted.SortObjectKeys = true;

        CHECK(WriteJson(JsonValue(std::move(object)), sorted) == "{\"apple\":2,\"mango\":3,\"zebra\":1}");
    }

    TEST_CASE("trailing newline is added by default for text files")
    {
        const std::string text = WriteJson(JsonValue(1), JsonWriteOptions{});
        REQUIRE_FALSE(text.empty());
        CHECK(text.back() == '\n');

        JsonWriteOptions noNewline;
        noNewline.TrailingNewline = false;
        CHECK(WriteJson(JsonValue(1), noNewline) == "1");
    }

    TEST_CASE("FormatDouble covers the tricky boundaries")
    {
        // Integral doubles keep a fractional part so the value is still read as a real after a save/load cycle:
        // writing "3" for 3.0 loses the kind, and every whole-number float in a scene would silently become an int.
        CHECK(FormatDouble(0.0) == "0.0");
        CHECK(FormatDouble(-0.0) == "-0.0");
        CHECK(FormatDouble(3.0) == "3.0");
        CHECK(FormatDouble(1e300) == "1e+300");
        CHECK(FormatDouble(std::numeric_limits<double>::max()) == "1.7976931348623157e+308");
        CHECK(FormatDouble(std::numeric_limits<double>::min()) == "2.2250738585072014e-308");
    }
}

TEST_SUITE("Serialization/RoundTrip")
{
    TEST_CASE("parse then write reproduces the same tree")
    {
        const std::vector<std::string> samples = {
            R"({"a":1,"b":[1,2,3],"c":"text","d":{"e":null,"f":true}})", "[[[[1]]]]",
            "{\"deep\":{\"deeper\":{\"deepest\":[1,\"two\",3.5,null,false]}}}", R"({"escapes":"a\\b\"c\nd\té€"})"};

        for (const std::string& sample : samples)
        {
            INFO("sample: " << sample);

            const JsonValue parsed = ParseOk(sample);
            const std::string written = WriteCompactJson(parsed);

            const JsonValue reparsed = ParseOk(written);
            CHECK(parsed == reparsed);
        }
    }

    TEST_CASE("compact and indented forms parse to the same tree")
    {
        JsonObject object;
        for (int index = 0; index < 20; ++index)
        {
            object.Set("field" + std::to_string(index), JsonValue(index * 1.5));
        }

        const JsonValue value(std::move(object));

        JsonWriteOptions compact;
        compact.IndentSpaces = 0;
        compact.TrailingNewline = false;

        const std::string compactText = WriteJson(value, compact);
        const std::string indentedText = WriteJson(value, JsonWriteOptions{});

        CHECK(compactText.find('\n') == std::string::npos);
        CHECK(indentedText.find('\n') != std::string::npos);

        CHECK(ParseOk(compactText) == ParseOk(indentedText));
    }

    TEST_CASE("writing is deterministic")
    {
        // The same tree must produce identical bytes every time, or committed scene files churn and asset hashes
        // computed from serialised data become meaningless.
        JsonObject object;
        object.Set("z", JsonValue(1.0 / 3.0));
        object.Set("a", JsonValue(MakeArray(JsonArray{JsonValue("x"), JsonValue(false)})));
        object.Set("m", JsonValue(Int64{-1234567890123LL}));

        const JsonValue value(std::move(object));
        const std::string first = WriteCompactJson(value);

        for (int attempt = 0; attempt < 10; ++attempt)
        {
            CHECK(WriteCompactJson(value) == first);
        }

        // And a load/save cycle of the same file is byte-stable.
        const JsonValue reparsed = ParseOk(first);
        CHECK(WriteCompactJson(reparsed) == first);
    }

    TEST_CASE("every double round-trips exactly")
    {
        // This is the property the number format exists to guarantee: a scene saved and loaded repeatedly must not
        // drift, even by one unit in the last place.
        std::vector<double> values = {0.0,
                                      -0.0,
                                      1.0,
                                      -1.0,
                                      0.1,
                                      0.2,
                                      0.3,
                                      1.0 / 3.0,
                                      1e-300,
                                      1e300,
                                      std::numeric_limits<double>::min(),
                                      std::numeric_limits<double>::max(),
                                      std::numeric_limits<double>::denorm_min(),
                                      std::numeric_limits<double>::epsilon()};

        // Plus a deterministic sweep of random bit patterns (NaN excluded: it has no JSON representation).
        unsigned long long state = 0xBADC0FFEULL;
        for (int index = 0; index < 20000; ++index)
        {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            double value = 0.0;
            std::memcpy(&value, &state, sizeof(value));
            if (!std::isfinite(value))
            {
                continue;
            }
            values.push_back(value);
        }

        for (const double value : values)
        {
            const std::string text = WriteCompactJson(JsonValue(value));
            const JsonValue parsed = ParseOk(text);
            REQUIRE(parsed.IsReal());
            INFO("value " << text);
            CHECK(parsed.AsDouble() == value);
        }
    }

    TEST_CASE("every integer round-trips exactly")
    {
        const std::vector<Int64> signedValues = {0,
                                                 1,
                                                 -1,
                                                 std::numeric_limits<Int64>::max(),
                                                 std::numeric_limits<Int64>::min(),
                                                 9007199254740993LL,
                                                 -9007199254740993LL};

        for (const Int64 value : signedValues)
        {
            const std::string text = WriteCompactJson(JsonValue(value));
            INFO("value " << text);
            CHECK(ParseOk(text).AsInt() == value);
        }

        const std::vector<UInt64> unsignedValues = {0, 1, std::numeric_limits<UInt64>::max(), 1ULL << 63,
                                                    9223372036854775808ULL};

        for (const UInt64 value : unsignedValues)
        {
            const std::string text = WriteCompactJson(JsonValue(value));
            INFO("value " << text);
            CHECK(ParseOk(text).AsUInt() == value);
        }
    }

    TEST_CASE("strings survive escapes and UTF-8")
    {
        const std::vector<std::string> samples = {"",
                                                  "plain",
                                                  "with \"quotes\"",
                                                  "back\\slash",
                                                  "new\nline",
                                                  "tab\tcharacter",
                                                  "control\x01\x02\x1F",
                                                  "unicode: é€中",
                                                  std::string("embedded\0nul", 13)};

        for (const std::string& sample : samples)
        {
            INFO("length " << sample.size());

            const std::string written = WriteCompactJson(JsonValue(sample));
            const JsonValue parsed = ParseOk(written);

            REQUIRE(parsed.IsString());
            CHECK(parsed.AsString() == sample);
        }
    }

    TEST_CASE("a realistic scene fragment survives a full cycle")
    {
        const std::string scene = R"({
  "formatVersion": 1,
  "name": "TestScene",
  "entities": [
    {
      "id": 0,
      "name": "DirectionalLight",
      "transform": { "position": [-10.5, 20.0, -3.25], "rotation": [0.0, 0.7071067811865476, 0.0, 0.7071067811865476], "scale": [1, 1, 1] },
      "components": { "Light": { "intensity": 3.5, "color": [1.0, 0.9, 0.8], "castShadows": true } }
    },
    {
      "id": 1,
      "name": "Player",
      "parent": 0,
      "components": { "RigidBody": { "mass": 80.5, "restitution": 0.3 } }
    }
  ]
})";

        const JsonValue parsed = ParseOk(scene);
        REQUIRE(parsed.IsObject());

        CHECK(parsed.FindMember("formatVersion")->AsInt() == 1);
        CHECK(parsed.FindMember("entities")->GetElementCount() == 2u);

        const std::string written = WriteJson(parsed, JsonWriteOptions{});
        const JsonValue reparsed = ParseOk(written);

        CHECK(parsed == reparsed);

        // Values that would be corrupted by a lossy number format stay exact.
        const JsonValue* rotation =
            parsed.FindMember("entities")->FindElement(0)->FindMember("transform")->FindMember("rotation");
        REQUIRE(rotation != nullptr);
        CHECK(WriteCompactJson(*rotation) ==
              WriteCompactJson(
                  *reparsed.FindMember("entities")->FindElement(0)->FindMember("transform")->FindMember("rotation")));
    }

    TEST_CASE("deep documents parse without trouble up to the limit")
    {
        // Well above any real scene, and below the parser's bound: proves the recursion is not accidentally
        // limited to something small.
        constexpr int depth = 200;
        const std::string text =
            std::string(static_cast<size_t>(depth), '[') + "1" + std::string(static_cast<size_t>(depth), ']');

        JsonValue root;
        JsonError error;
        REQUIRE(TryParseJson(text, root, &error));

        const std::string written = WriteCompactJson(root);
        CHECK(WrittenMatchesExpectedDepth(written, depth));
    }
}
