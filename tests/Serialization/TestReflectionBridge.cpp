#include "TestSupport.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Serialization/JsonParser.h"
#include "Hollowpoint/Serialization/JsonValue.h"
#include "Hollowpoint/Serialization/JsonWriter.h"
#include "Hollowpoint/Serialization/ReflectionBridge.h"

using Hollowpoint::Core::UInt64;
using Hollowpoint::Reflection::FieldAttributes;
using Hollowpoint::Reflection::FieldFlag_Deprecated;
using Hollowpoint::Reflection::FieldFlag_DontSerialize;
using Hollowpoint::Reflection::FieldFlag_ReadOnly;
using Hollowpoint::Reflection::Registry;
using Hollowpoint::Reflection::Size;
using Hollowpoint::Reflection::TypeDescriptor;
using namespace Hollowpoint::Serialization;

namespace
{

// ---------------------------------------------------------------------------
// Fixtures, registered exactly as a real component type would be. Names are Bridge-prefixed because the registry is
// a process-wide singleton: a collision with another suite's registration would make failures depend on which tests
// ran first.
// ---------------------------------------------------------------------------

struct BridgeNested
{
    float X = 1.0f;
    int Count = 2;
};

enum class BridgeMode : unsigned char
{
    Point = 0,
    Directional = 1,
    Spot = 2
};

struct BridgeComponent
{
    bool Enabled = true;
    int Layers = 0;
    float Smoothness = 0.5f;
    double PrecisionValue = 0.0;
    std::string Label = "default";
    BridgeMode Mode = BridgeMode::Point;
    BridgeNested Nested;
    std::vector<int> Indices;
    std::vector<float> Weights;
    std::vector<BridgeNested> Items;

    //! Stands in for runtime-only state (a cached pointer, an accumulated timer). Reflected so the inspector can see
    //! it, flagged DontSerialize so the serializer must skip it — a void* is not reflectable, hence an int.
    int RuntimeJunk = 0;

    //! Script-editable but inspector-read-only: unlike DontSerialize this *must* survive a save, or loading a scene
    //! would reset scripted state (health, ammo) to defaults.
    int ScriptState = 0;

    //! Renamed-away field kept for old files: hidden from the UI but still loaded.
    int LegacyValue = 0;
};

// A three-level nest, to exercise recursion and the depth limit without needing a self-referential type.
struct BridgeLevel3
{
    int Value = 3;
};
struct BridgeLevel2
{
    BridgeLevel3 Inner;
    int Value = 2;
};
struct BridgeLevel1
{
    BridgeLevel2 Inner;
    int Value = 1;
};

// A base + derived pair, to prove inherited fields are serialised. TypeDescriptor reports own fields only (so
// counts never double-count and the inspector can group them), which means a consumer that forgets to walk
// GetBaseType() silently saves a derived object without its base's data — valid-looking output, missing fields.
struct BridgeBase
{
    int BaseValue = 1;
    std::string Name = "base";
};

struct BridgeDerived : BridgeBase
{
    float OwnValue = 2.5f;
};

struct BridgeDerivedWithSkip : BridgeBase
{
    int OwnValue = 0;
    int Skipped = 0;
};

FieldAttributes WithFlag(unsigned int flag)
{
    FieldAttributes attributes;
    attributes.Flags = flag;
    return attributes;
}

// Registration is done once, through the builder, because the fixture needs mixed field attributes. Returning true
// from a lambda initialiser keeps it at namespace scope (the same place real components register).
const bool kBridgeTypesRegistered = []() {
    Registry::Get()
        .RegisterType<BridgeNested>("BridgeNested")
        .Field("x", &BridgeNested::X)
        .Field("count", &BridgeNested::Count);

    Registry::Get()
        .RegisterEnum<BridgeMode>("BridgeMode")
        .Value("Point", BridgeMode::Point)
        .Value("Directional", BridgeMode::Directional)
        .Value("Spot", BridgeMode::Spot);

    Registry::Get()
        .RegisterType<BridgeComponent>("BridgeComponent")
        .Field("enabled", &BridgeComponent::Enabled)
        .Field("layers", &BridgeComponent::Layers)
        .Field("smoothness", &BridgeComponent::Smoothness)
        .Field("precision", &BridgeComponent::PrecisionValue)
        .Field("label", &BridgeComponent::Label)
        .Field("mode", &BridgeComponent::Mode)
        .Field("nested", &BridgeComponent::Nested)
        .Field("indices", &BridgeComponent::Indices)
        .Field("weights", &BridgeComponent::Weights)
        .Field("items", &BridgeComponent::Items)
        .Field("runtimeJunk", &BridgeComponent::RuntimeJunk, WithFlag(FieldFlag_DontSerialize))
        .Field("scriptState", &BridgeComponent::ScriptState, WithFlag(FieldFlag_ReadOnly))
        .Field("legacyValue", &BridgeComponent::LegacyValue, WithFlag(FieldFlag_Deprecated));

    Registry::Get()
        .RegisterType<BridgeBase>("BridgeBase")
        .Field("baseValue", &BridgeBase::BaseValue)
        .Field("name", &BridgeBase::Name);

    Registry::Get()
        .RegisterType<BridgeDerived>("BridgeDerived", "BridgeBase")
        .Field("ownValue", &BridgeDerived::OwnValue);

    Registry::Get()
        .RegisterType<BridgeDerivedWithSkip>("BridgeDerivedWithSkip", "BridgeBase")
        .Field("ownValue", &BridgeDerivedWithSkip::OwnValue)
        .Field("skipped", &BridgeDerivedWithSkip::Skipped, WithFlag(FieldFlag_DontSerialize));

    Registry::Get().RegisterType<BridgeLevel3>("BridgeLevel3").Field("value", &BridgeLevel3::Value);
    Registry::Get()
        .RegisterType<BridgeLevel2>("BridgeLevel2")
        .Field("inner", &BridgeLevel2::Inner)
        .Field("value", &BridgeLevel2::Value);
    Registry::Get()
        .RegisterType<BridgeLevel1>("BridgeLevel1")
        .Field("inner", &BridgeLevel1::Inner)
        .Field("value", &BridgeLevel1::Value);

    return true;
}();

const TypeDescriptor& GetComponentType()
{
    const TypeDescriptor* type = Registry::Get().FindType("BridgeComponent");
    REQUIRE(type != nullptr);
    return *type;
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

JsonValue Encode(const BridgeComponent& component, ConversionDiagnostics& diagnostics)
{
    return TypeToJson(&component, GetComponentType(), diagnostics);
}

bool Decode(const JsonValue& json, BridgeComponent& component, ConversionDiagnostics& diagnostics)
{
    return JsonToType(json, &component, GetComponentType(), diagnostics);
}

BridgeComponent RoundTrip(const BridgeComponent& source)
{
    ConversionDiagnostics encodeErrors;
    const JsonValue json = Encode(source, encodeErrors);
    REQUIRE_FALSE(encodeErrors.HasErrors());

    BridgeComponent destination;
    ConversionDiagnostics decodeErrors;
    REQUIRE(Decode(json, destination, decodeErrors));
    CHECK_FALSE(decodeErrors.HasErrors());

    return destination;
}

JsonValue ParseOrThrow(std::string_view text)
{
    JsonValue value;
    JsonError error;
    const bool ok = TryParseJson(text, value, &error);
    INFO("parse failure: " << error.Format("<test>"));
    REQUIRE(ok);
    return value;
}

const char* FindFirst(const ConversionDiagnostics& diagnostics, const char* needle)
{
    for (const std::string& message : diagnostics.Messages)
    {
        if (message.find(needle) != std::string::npos)
        {
            return message.c_str();
        }
    }
    return nullptr;
}

}  // namespace

TEST_SUITE("Serialization/ReflectionBridge")
{
    TEST_CASE("the fixtures are registered and resolvable")
    {
        CHECK(Registry::Get().FindType("BridgeComponent") != nullptr);
        CHECK(Registry::Get().FindType("BridgeNested") != nullptr);
        CHECK(Registry::Get().FindType("NoSuchFixture") == nullptr);
    }

    TEST_CASE("a default component encodes with every serialisable field present")
    {
        BridgeComponent component;
        ConversionDiagnostics diagnostics;

        const JsonValue json = Encode(component, diagnostics);
        CHECK_FALSE(diagnostics.HasErrors());
        REQUIRE(json.IsObject());

        CHECK(json.FindMember("enabled")->AsBool() == true);
        CHECK(json.FindMember("layers")->AsInt() == 0);
        CHECK(json.FindMember("smoothness")->AsDouble() == 0.5);
        CHECK(json.FindMember("label")->AsString() == "default");
    }

    TEST_CASE("DontSerialize fields never reach the wire")
    {
        // A cached pointer or accumulated timer written to disk is at best meaningless and at worst restored as a
        // live-but-wrong value on load, so this must be skipped on both paths.
        BridgeComponent component;
        component.RuntimeJunk = 4242;

        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(component, diagnostics);

        CHECK(json.FindMember("runtimeJunk") == nullptr);
    }

    TEST_CASE("DontSerialize fields are not clobbered on decode either")
    {
        BridgeComponent source;
        source.RuntimeJunk = 111;

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(source, encodeErrors);

        // Even a file that somehow contains the key must not change the live value.
        json.AsObjectMutable().Set("runtimeJunk", JsonValue(999));

        BridgeComponent destination;
        destination.RuntimeJunk = 222;
        ConversionDiagnostics decodeErrors;
        REQUIRE(Decode(json, destination, decodeErrors));

        CHECK(destination.RuntimeJunk == 222);
    }

    TEST_CASE("ReadOnly fields are saved by default (scripted state must survive)")
    {
        // "Read only" describes the inspector, not the save file: dropping these would silently reset health/ammo on
        // every load.
        BridgeComponent component;
        component.ScriptState = 77;

        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(component, diagnostics);

        REQUIRE(json.FindMember("scriptState") != nullptr);
        CHECK(json.FindMember("scriptState")->AsInt() == 77);

        BridgeComponent destination;
        ConversionDiagnostics decodeErrors;
        REQUIRE(Decode(json, destination, decodeErrors));
        CHECK(destination.ScriptState == 77);
    }

    TEST_CASE("ReadOnly fields can be excluded on request")
    {
        BridgeComponent component;
        component.ScriptState = 77;

        ConversionOptions options;
        options.IncludeReadOnlyFields = false;

        ConversionDiagnostics diagnostics;
        const JsonValue json = TypeToJson(&component, GetComponentType(), diagnostics, options);
        CHECK(json.FindMember("scriptState") == nullptr);
    }

    TEST_CASE("Deprecated fields are still saved and loaded (hidden from UI, not from disk)")
    {
        BridgeComponent component;
        component.LegacyValue = 55;

        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(component, diagnostics);

        REQUIRE(json.FindMember("legacyValue") != nullptr);
        CHECK(json.FindMember("legacyValue")->AsInt() == 55);

        BridgeComponent destination;
        ConversionDiagnostics decodeErrors;
        REQUIRE(Decode(json, destination, decodeErrors));
        CHECK(destination.LegacyValue == 55);
    }

    TEST_CASE("every scalar kind round-trips")
    {
        BridgeComponent source;
        source.Enabled = false;
        source.Layers = -123456;
        source.Smoothness = 0.123f;
        source.PrecisionValue = 3.14159265358979;
        source.Label = "a label with \\ backslash, \" quotes and \n newline";
        source.Mode = BridgeMode::Spot;

        const BridgeComponent destination = RoundTrip(source);

        CHECK(destination.Enabled == false);
        CHECK(destination.Layers == -123456);
        CHECK(destination.Smoothness == doctest::Approx(0.123));
        CHECK(destination.PrecisionValue == doctest::Approx(3.14159265358979));
        CHECK(destination.Label == source.Label);
        CHECK(destination.Mode == BridgeMode::Spot);
    }

    TEST_CASE("enums are stored by name, not number")
    {
        BridgeComponent source;
        source.Mode = BridgeMode::Directional;

        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(source, diagnostics);

        REQUIRE(json.FindMember("mode") != nullptr);
        CHECK(json.FindMember("mode")->IsString());
        CHECK(json.FindMember("mode")->AsString() == "Directional");
    }

    TEST_CASE("an unknown enum name is an error rather than a silent default")
    {
        // "Direcional" silently becoming Point is the kind of bug nobody finds; it must be reported and rejected.
        BridgeComponent destination;
        destination.Mode = BridgeMode::Spot;

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(BridgeComponent{}, encodeErrors);
        json.AsObjectMutable().Set("mode", JsonValue("Direcional"));

        ConversionDiagnostics decodeErrors;
        CHECK_FALSE(Decode(json, destination, decodeErrors));
        REQUIRE(FindFirst(decodeErrors, "not a value of") != nullptr);

        // The existing value is untouched rather than zeroed.
        CHECK(destination.Mode == BridgeMode::Spot);
    }

    TEST_CASE("a numeric enum value still loads (hand-written files, older builds)")
    {
        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(BridgeComponent{}, encodeErrors);
        json.AsObjectMutable().Set("mode", JsonValue(1));

        BridgeComponent destination;
        ConversionDiagnostics decodeErrors;
        REQUIRE(Decode(json, destination, decodeErrors));
        CHECK(destination.Mode == BridgeMode::Directional);
    }

    TEST_CASE("nested structs encode as nested objects and round-trip")
    {
        BridgeComponent source;
        source.Nested.X = 2.5f;
        source.Nested.Count = 9;

        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(source, diagnostics);

        const JsonValue* nested = json.FindMember("nested");
        REQUIRE(nested != nullptr);
        CHECK(nested->IsObject());
        CHECK(nested->FindMember("x")->AsDouble() == doctest::Approx(2.5));
        CHECK(nested->FindMember("count")->AsInt() == 9);

        const BridgeComponent destination = RoundTrip(source);
        CHECK(destination.Nested.X == 2.5f);
        CHECK(destination.Nested.Count == 9);
    }

    TEST_CASE("arrays of scalars round-trip")
    {
        BridgeComponent source;
        source.Indices = {1, 2, 3, 4000000, -7};
        source.Weights = {0.0f, 0.5f, 1.0f, 0.25f};

        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(source, diagnostics);

        CHECK(json.FindMember("indices")->GetElementCount() == 5u);
        CHECK(json.FindMember("indices")->FindElement(3)->AsInt() == 4000000);

        const BridgeComponent destination = RoundTrip(source);
        CHECK(destination.Indices == source.Indices);
        REQUIRE(destination.Weights.size() == source.Weights.size());
        for (Size index = 0; index < source.Weights.size(); ++index)
        {
            CHECK(destination.Weights[index] == source.Weights[index]);
        }
    }

    TEST_CASE("arrays of structs round-trip")
    {
        BridgeComponent source;
        source.Items = {BridgeNested{1.5f, 10}, BridgeNested{2.5f, 20}, BridgeNested{-3.5f, 30}};

        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(source, diagnostics);

        const JsonValue* items = json.FindMember("items");
        REQUIRE(items != nullptr);
        CHECK(items->GetElementCount() == 3u);
        CHECK(items->FindElement(1)->FindMember("count")->AsInt() == 20);

        const BridgeComponent destination = RoundTrip(source);
        REQUIRE(destination.Items.size() == 3u);
        CHECK(destination.Items[2].X == -3.5f);
        CHECK(destination.Items[2].Count == 30);
    }

    TEST_CASE("arrays resize down when the file is shorter")
    {
        BridgeComponent source;
        source.Indices = {1, 2};

        BridgeComponent destination;
        destination.Indices = {9, 9, 9, 9, 9};  // Longer than what will be loaded.

        ConversionDiagnostics encodeErrors;
        const JsonValue json = Encode(source, encodeErrors);

        ConversionDiagnostics decodeErrors;
        REQUIRE(Decode(json, destination, decodeErrors));
        CHECK(destination.Indices.size() == 2u);
    }

    TEST_CASE("empty arrays survive")
    {
        BridgeComponent source;  // All arrays empty.

        const BridgeComponent destination = RoundTrip(source);
        CHECK(destination.Indices.empty());
        CHECK(destination.Items.empty());
    }

    TEST_CASE("output member order follows field declaration order")
    {
        // Deterministic output is what keeps saved scenes diffable and asset hashes stable.
        BridgeComponent component;
        ConversionDiagnostics diagnostics;
        const JsonValue json = Encode(component, diagnostics);

        const JsonObject& object = json.AsObject();
        REQUIRE(object.GetCount() >= 10u);

        CHECK(object.GetKey(0) == "enabled");
        CHECK(object.GetKey(1) == "layers");
        CHECK(object.GetKey(2) == "smoothness");
        CHECK(object.GetKey(3) == "precision");
        CHECK(object.GetKey(4) == "label");
        CHECK(object.GetKey(5) == "mode");
        CHECK(object.GetKey(6) == "nested");
    }

    TEST_CASE("encoding is deterministic across repeated calls")
    {
        BridgeComponent component;
        component.Label = "determinism";
        component.Weights = {0.1f, 0.2f, 0.3f};

        ConversionDiagnostics firstErrors;
        const std::string first = WriteCompactJson(Encode(component, firstErrors));

        for (int attempt = 0; attempt < 5; ++attempt)
        {
            ConversionDiagnostics moreErrors;
            CHECK(WriteCompactJson(Encode(component, moreErrors)) == first);
        }
    }

    TEST_CASE("a field absent from JSON keeps its current value")
    {
        // This is what makes adding a field to a component safe for every existing scene file: no migration and no
        // version bump — the new field simply gets the same default a freshly constructed component would have.
        BridgeComponent source;

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(source, encodeErrors);
        json.AsObjectMutable().Remove("smoothness");
        json.AsObjectMutable().Remove("label");

        BridgeComponent destination;
        destination.Smoothness = 0.75f;  // Pre-set value that must survive.
        destination.Label = "kept";

        ConversionDiagnostics decodeErrors;
        REQUIRE(Decode(json, destination, decodeErrors));
        CHECK_FALSE(decodeErrors.HasErrors());

        CHECK(destination.Smoothness == 0.75f);
        CHECK(destination.Label == "kept");
    }

    TEST_CASE("an unknown JSON member is reported and skipped, not fatal")
    {
        // The ordinary signature of a scene saved by a newer build or a renamed field. Losing one value with a clear
        // message beats refusing to load the level — and every field the type *does* know must still load normally.
        BridgeComponent source;
        source.Layers = 5;
        source.Label = "from the file";

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(source, encodeErrors);
        json.AsObjectMutable().Set("fieldFromTheFuture", JsonValue(1));

        BridgeComponent destination;
        ConversionDiagnostics decodeErrors;
        CHECK(Decode(json, destination, decodeErrors));  // Succeeds despite the unknown key.
        REQUIRE(FindFirst(decodeErrors, "has no field named 'fieldFromTheFuture'") != nullptr);

        // Everything the type understands still loaded.
        CHECK(destination.Layers == 5);
        CHECK(destination.Label == "from the file");
    }

    TEST_CASE("type mismatches are reported with a path and leave the field untouched")
    {
        BridgeComponent destination;
        destination.Layers = 111;
        destination.Enabled = true;
        destination.Label = "original";

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(BridgeComponent{}, encodeErrors);
        json.AsObjectMutable().Set("layers", JsonValue("not a number"));
        json.AsObjectMutable().Set("enabled", JsonValue(3));
        json.AsObjectMutable().Set("label", JsonValue(17));

        ConversionDiagnostics decodeErrors;
        CHECK_FALSE(Decode(json, destination, decodeErrors));

        CHECK(destination.Layers == 111);
        CHECK(destination.Enabled == true);
        CHECK(destination.Label == "original");

        // Messages name the field so a twelve-field file produces twelve actionable lines.
        CHECK(FindFirst(decodeErrors, "layers") != nullptr);
        CHECK(FindFirst(decodeErrors, "enabled") != nullptr);
        CHECK(FindFirst(decodeErrors, "label") != nullptr);
    }

    TEST_CASE("out-of-range integers are clamped rather than wrapped")
    {
        // Same rule the inspector uses, so loading and dragging cannot store different bytes. Wrapping a count to a
        // negative would corrupt data silently.
        BridgeComponent destination;
        destination.Layers = 0;

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(BridgeComponent{}, encodeErrors);
        json.AsObjectMutable().Set("layers", JsonValue(Hollowpoint::Core::Int64{2147483648LL}));  // INT32_MAX + 1.

        ConversionDiagnostics decodeErrors;
        Decode(json, destination, decodeErrors);

        CHECK(destination.Layers == std::numeric_limits<int>::max());
    }

    TEST_CASE("floats keep every bit they can through a full text round-trip")
    {
        // Encode -> write -> parse -> decode. A lossy number format would show up here as drift, and drift in a scene
        // file means values changing on every save.
        const float samples[] = {0.1f, 1.0f / 3.0f, 1.0f, -7.5f, 1e-20f, 3.4e38f};

        for (const float sample : samples)
        {
            BridgeComponent source;
            source.Smoothness = sample;

            ConversionDiagnostics encodeErrors;
            const std::string text = WriteJson(Encode(source, encodeErrors));

            JsonValue reparsed;
            REQUIRE(TryParseJson(text, reparsed));

            BridgeComponent destination;
            ConversionDiagnostics decodeErrors;
            REQUIRE(Decode(reparsed, destination, decodeErrors));

            INFO("sample " << sample);
            CHECK(destination.Smoothness == sample);
        }
    }

    TEST_CASE("deeply nested structs encode and decode")
    {
        const TypeDescriptor* type = Registry::Get().FindType("BridgeLevel1");
        REQUIRE(type != nullptr);

        BridgeLevel1 source;
        source.Value = 1;
        source.Inner.Value = 2;
        source.Inner.Inner.Value = 3;

        ConversionDiagnostics encodeErrors;
        const JsonValue json = TypeToJson(&source, *type, encodeErrors);
        CHECK_FALSE(encodeErrors.HasErrors());

        REQUIRE(json.FindMember("inner") != nullptr);
        REQUIRE(json.FindMember("inner")->FindMember("inner") != nullptr);
        CHECK(json.FindMember("inner")->FindMember("inner")->FindMember("value")->AsInt() == 3);

        BridgeLevel1 destination;
        ConversionDiagnostics decodeErrors;
        REQUIRE(JsonToType(json, &destination, *type, decodeErrors));
        CHECK(destination.Inner.Inner.Value == 3);
    }

    TEST_CASE("the depth limit stops runaway recursion instead of the stack")
    {
        const TypeDescriptor* type = Registry::Get().FindType("BridgeLevel1");
        REQUIRE(type != nullptr);

        BridgeLevel1 source;
        ConversionDiagnostics encodeErrors;

        // MaxDepth 1 cannot even reach the second level, so conversion must stop and report rather than recurse.
        ConversionOptions tooShallow;
        tooShallow.MaxDepth = 1;

        const JsonValue json = TypeToJson(&source, *type, encodeErrors, tooShallow);
        CHECK(encodeErrors.HasErrors());
        CHECK(FindFirst(encodeErrors, "depth") != nullptr);

        // A generous limit succeeds.
        ConversionDiagnostics okErrors;
        const JsonValue okJson = TypeToJson(&source, *type, okErrors);
        CHECK_FALSE(okErrors.HasErrors());
        CHECK(okJson.FindMember("inner")->FindMember("inner") != nullptr);
    }

    TEST_CASE("null object input is reported rather than crashing")
    {
        ConversionDiagnostics diagnostics;
        const JsonValue json = TypeToJson(nullptr, GetComponentType(), diagnostics);

        CHECK(json.IsNull());
        CHECK(diagnostics.HasErrors());
    }

    TEST_CASE("decoding a non-object is reported")
    {
        BridgeComponent destination;
        ConversionDiagnostics decodeErrors;

        CHECK_FALSE(JsonToType(JsonValue(5), &destination, GetComponentType(), decodeErrors));
        CHECK(FindFirst(decodeErrors, "expected an object") != nullptr);

        CHECK_FALSE(JsonToType(JsonValue(MakeArray()), &destination, GetComponentType(), decodeErrors));
    }

    TEST_CASE("null into a scalar field is reported as a type mismatch")
    {
        BridgeComponent destination;
        destination.Layers = 42;

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(BridgeComponent{}, encodeErrors);
        json.AsObjectMutable().Set("layers", JsonValue(nullptr));

        ConversionDiagnostics decodeErrors;
        CHECK_FALSE(Decode(json, destination, decodeErrors));
        CHECK(destination.Layers == 42);
    }

    TEST_CASE("an array field given a non-array is reported")
    {
        BridgeComponent destination;

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(BridgeComponent{}, encodeErrors);
        json.AsObjectMutable().Set("indices", JsonValue(5));

        ConversionDiagnostics decodeErrors;
        CHECK_FALSE(Decode(json, destination, decodeErrors));
        CHECK(FindFirst(decodeErrors, "expected an array") != nullptr);
    }

    TEST_CASE("a struct field given a scalar is reported")
    {
        BridgeComponent destination;

        ConversionDiagnostics encodeErrors;
        JsonValue json = Encode(BridgeComponent{}, encodeErrors);
        json.AsObjectMutable().Set("nested", JsonValue(1.5));

        ConversionDiagnostics decodeErrors;
        CHECK_FALSE(Decode(json, destination, decodeErrors));
    }

    TEST_CASE("diagnostics can be reported to the log with context")
    {
        ConversionDiagnostics diagnostics;
        diagnostics.Add("first problem");
        diagnostics.Add("second problem");

        Hollowpoint::Test::ScopedLogCapture logs;
        diagnostics.ReportToLog("level.hpscene");

        CHECK(logs.GetErrorCount() >= 3);  // Two messages plus a summary.
    }

    TEST_CASE("empty diagnostics report nothing")
    {
        ConversionDiagnostics diagnostics;
        Hollowpoint::Test::ScopedLogCapture logs;
        diagnostics.ReportToLog("level.hpscene");
        CHECK(logs.GetErrorCount() == 0);
    }

    TEST_CASE("a large component round-trips efficiently and exactly")
    {
        // Guards the resize-once path for arrays and checks nothing truncates at a small size.
        BridgeComponent source;
        for (int index = 0; index < 5000; ++index)
        {
            source.Indices.push_back(index * 3 - 1);
            source.Weights.push_back(static_cast<float>(index) * 0.001f);

            if (index % 10 == 0)
            {
                source.Items.push_back(BridgeNested{static_cast<float>(index), index});
            }
        }

        const BridgeComponent destination = RoundTrip(source);

        CHECK(destination.Indices.size() == 5000u);
        CHECK(destination.Indices[4999] == 4999 * 3 - 1);
        CHECK(destination.Items.size() == 500u);
        CHECK(destination.Items[499].Count == 4990);
    }

    TEST_CASE("inherited fields are encoded, base-first")
    {
        const TypeDescriptor* type = Registry::Get().FindType("BridgeDerived");
        REQUIRE(type != nullptr);
        REQUIRE(type->GetBaseType() != nullptr);

        // Sanity: the descriptor really does report own fields only, which is what makes this test meaningful.
        CHECK(type->GetFieldCount() == 1u);

        BridgeDerived derived;
        derived.BaseValue = 42;
        derived.Name = "from base";
        derived.OwnValue = 3.75f;

        ConversionDiagnostics diagnostics;
        const JsonValue json = TypeToJson(&derived, *type, diagnostics);
        CHECK_FALSE(diagnostics.HasErrors());

        // All three fields present — the bug this guards against was the base's two fields vanishing entirely.
        CHECK(json.FindMember("baseValue")->AsInt() == 42);
        CHECK(json.FindMember("name")->AsString() == "from base");
        CHECK(json.FindMember("ownValue")->AsDouble() == doctest::Approx(3.75));

        // Base-first ordering: inherited members come before the derived type's own additions.
        const JsonObject& object = json.AsObject();
        REQUIRE(object.GetCount() == 3u);
        CHECK(object.GetKey(0) == "baseValue");
        CHECK(object.GetKey(1) == "name");
        CHECK(object.GetKey(2) == "ownValue");
    }

    TEST_CASE("inherited fields decode, and are not reported as unknown members")
    {
        const TypeDescriptor* type = Registry::Get().FindType("BridgeDerived");
        REQUIRE(type != nullptr);

        // An inherited key must load without the "has no field named" diagnostic — that check has to consider bases.
        const JsonValue json = ParseOrThrow(R"({"baseValue": 7, "name": "inherited", "ownValue": 1.5})");

        BridgeDerived derived;
        ConversionDiagnostics diagnostics;
        REQUIRE(JsonToType(json, &derived, *type, diagnostics));
        CHECK_FALSE(diagnostics.HasErrors());

        CHECK(derived.BaseValue == 7);
        CHECK(derived.Name == "inherited");
        CHECK(derived.OwnValue == 1.5f);
    }

    TEST_CASE("inherited fields round-trip through text")
    {
        const TypeDescriptor* type = Registry::Get().FindType("BridgeDerived");
        REQUIRE(type != nullptr);

        BridgeDerived source;
        source.BaseValue = -9;
        source.Name = "round trip";
        source.OwnValue = 0.125f;

        ConversionDiagnostics encodeErrors;
        const std::string text = WriteJson(TypeToJson(&source, *type, encodeErrors));
        CHECK_FALSE(encodeErrors.HasErrors());

        JsonValue reparsed;
        REQUIRE(TryParseJson(text, reparsed));

        BridgeDerived destination;
        ConversionDiagnostics decodeErrors;
        REQUIRE(JsonToType(reparsed, &destination, *type, decodeErrors));

        CHECK(destination.BaseValue == source.BaseValue);
        CHECK(destination.Name == source.Name);
        CHECK(destination.OwnValue == source.OwnValue);
    }

    TEST_CASE("a DontSerialize flag on an inherited field is honoured")
    {
        const TypeDescriptor* type = Registry::Get().FindType("BridgeDerivedWithSkip");
        REQUIRE(type != nullptr);

        BridgeDerivedWithSkip value;
        value.Skipped = 111;
        value.BaseValue = 222;
        value.OwnValue = 1.0f;

        ConversionDiagnostics diagnostics;
        const JsonValue json = TypeToJson(&value, *type, diagnostics);

        CHECK(json.FindMember("skipped") == nullptr);
        CHECK(json.FindMember("baseValue")->AsInt() == 222);
    }

    TEST_CASE("the bridge preserves values through text so a scene file is stable")
    {
        // Encode -> text -> parse -> decode -> encode must produce identical bytes: this is the property that makes
        // "load a level, save it unchanged" a no-op in version control.
        BridgeComponent component;
        component.Label = "crate_01";
        component.Mode = BridgeMode::Directional;
        component.Indices = {3, 1, 4, 1, 5};
        component.Nested.X = 1.25f;

        ConversionDiagnostics firstErrors;
        const std::string first = WriteJson(Encode(component, firstErrors));

        JsonValue reparsed;
        REQUIRE(TryParseJson(first, reparsed));

        BridgeComponent loaded;
        ConversionDiagnostics decodeErrors;
        REQUIRE(Decode(reparsed, loaded, decodeErrors));

        ConversionDiagnostics secondErrors;
        CHECK(WriteJson(Encode(loaded, secondErrors)) == first);
    }
}
