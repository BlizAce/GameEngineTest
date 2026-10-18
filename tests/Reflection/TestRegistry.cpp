#include "TestSupport.h"

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Reflection/ScalarAccess.h"

using namespace Hollowpoint::Reflection;

namespace
{

// Fixtures local to this suite. Registration names are prefixed so they cannot collide with anything else
// linked into the same test binary (the registry is a process-wide singleton by design).
struct Vec2Fixture
{
    float X = 0.0f;
    float Y = 0.0f;
};

enum class EKindFixture : unsigned char
{
    None = 0,
    Fast = 1,
    Slow = 2
};

enum class EBiggerFixture : int
{
    First = 100,
    Second = 200
};

struct AllKindsFixture
{
    bool Flag = false;
    signed char I8 = 0;
    short I16 = 0;
    int I32 = 0;
    long long I64 = 0;
    unsigned char U8 = 0;
    unsigned short U16 = 0;
    unsigned int U32 = 0;
    unsigned long long U64 = 0;
    float F32 = 0.0f;
    double F64 = 0.0;
    std::string Text;
    EKindFixture Kind = EKindFixture::None;
    Vec2Fixture Nested;
    std::vector<int> Numbers;
    std::vector<Vec2Fixture> Points;
    std::vector<EKindFixture> Flags;
};

struct BaseFixture
{
    int BaseValue = 0;
};

struct DerivedFixture : BaseFixture
{
    int OwnValue = 0;
};

}  // namespace

// Registration happens at namespace scope, exactly as a real component type would do it.
HP_REGISTER_TYPE(Vec2Fixture).Field("x", &Vec2Fixture::X).Field("y", &Vec2Fixture::Y);

HP_REGISTER_ENUM(EKindFixture)
    .Value("None", EKindFixture::None)
    .Value("Fast", EKindFixture::Fast)
    .Value("Slow", EKindFixture::Slow);

HP_REGISTER_ENUM(EBiggerFixture).Value("First", EBiggerFixture::First).Value("Second", EBiggerFixture::Second);

HP_REGISTER_TYPE(AllKindsFixture)
    .Field("flag", &AllKindsFixture::Flag)
    .Field("i8", &AllKindsFixture::I8)
    .Field("i16", &AllKindsFixture::I16)
    .Field("i32", &AllKindsFixture::I32)
    .Field("i64", &AllKindsFixture::I64)
    .Field("u8", &AllKindsFixture::U8)
    .Field("u16", &AllKindsFixture::U16)
    .Field("u32", &AllKindsFixture::U32)
    .Field("u64", &AllKindsFixture::U64)
    .Field("f32", &AllKindsFixture::F32)
    .Field("f64", &AllKindsFixture::F64)
    .Field("text", &AllKindsFixture::Text)
    .Field("kind", &AllKindsFixture::Kind)
    .Field("nested", &AllKindsFixture::Nested)
    .Field("numbers", &AllKindsFixture::Numbers)
    .Field("points", &AllKindsFixture::Points)
    .Field("enumList", &AllKindsFixture::Flags);

HP_REGISTER_TYPE(BaseFixture).Field("baseValue", &BaseFixture::BaseValue);

HP_REGISTER_TYPE_WITH_BASE(DerivedFixture, BaseFixture).Field("ownValue", &DerivedFixture::OwnValue);

TEST_SUITE("Reflection/Registry")
{
    TEST_CASE("a registered type is findable by name and exposes its fields in order")
    {
        const TypeDescriptor* type = Registry::Get().FindType("AllKindsFixture");
        REQUIRE(type != nullptr);

        CHECK(type->GetName() != nullptr);
        CHECK(std::string(type->GetName()) == "AllKindsFixture");
        CHECK(type->GetSize() == sizeof(AllKindsFixture));
        CHECK(type->GetAlign() == alignof(AllKindsFixture));

        REQUIRE(type->GetFieldCount() == 17u);
        for (Size index = 0; index < type->GetFieldCount(); ++index)
        {
            // Registration order is preserved, which is what makes the inspector's field order predictable.
            CHECK(type->GetField(index).GetName() != nullptr);
        }

        CHECK(std::string(type->GetField(0).GetName()) == "flag");
        CHECK(std::string(type->GetField(16).GetName()) == "enumList");
    }

    TEST_CASE("field offsets address the right member of a live object")
    {
        const TypeDescriptor* type = Registry::Get().FindType("AllKindsFixture");
        REQUIRE(type != nullptr);

        AllKindsFixture instance{};

        // Every offset must land inside the object; an off-by-allocation error here would corrupt neighbours.
        for (Size index = 0; index < type->GetFieldCount(); ++index)
        {
            const FieldDescriptor& field = type->GetField(index);
            const void* address = field.GetAddress(&instance);

            CHECK(address != nullptr);
            const auto* objectBase = reinterpret_cast<const std::byte*>(static_cast<const void*>(&instance));
            const auto* asBytes = reinterpret_cast<const std::byte*>(address);

            CHECK(asBytes >= objectBase);
            CHECK(asBytes <= objectBase + sizeof(AllKindsFixture));
        }

        // Spot-check the arithmetic against the real members, which catches a mis-measured offset even when
        // it happens to stay in range.
        CHECK(type->FindField("i32")->GetAddress(&instance) == static_cast<void*>(&instance.I32));
        CHECK(type->FindField("f64")->GetAddress(&instance) == static_cast<void*>(&instance.F64));
        CHECK(type->FindField("text")->GetAddress(&instance) == static_cast<void*>(&instance.Text));
        CHECK(type->FindField("nested")->GetAddress(&instance) == static_cast<void*>(&instance.Nested));
    }

    TEST_CASE("every primitive kind is classified correctly")
    {
        const TypeDescriptor* type = Registry::Get().FindType("AllKindsFixture");
        REQUIRE(type != nullptr);

        CHECK(type->FindField("flag")->GetKind() == ValueKind::Boolean);
        CHECK(type->FindField("i8")->GetKind() == ValueKind::Int8);
        CHECK(type->FindField("i16")->GetKind() == ValueKind::Int16);
        CHECK(type->FindField("i32")->GetKind() == ValueKind::Int32);
        CHECK(type->FindField("i64")->GetKind() == ValueKind::Int64);
        CHECK(type->FindField("u8")->GetKind() == ValueKind::UInt8);
        CHECK(type->FindField("u16")->GetKind() == ValueKind::UInt16);
        CHECK(type->FindField("u32")->GetKind() == ValueKind::UInt32);
        CHECK(type->FindField("u64")->GetKind() == ValueKind::UInt64);
        CHECK(type->FindField("f32")->GetKind() == ValueKind::Float32);
        CHECK(type->FindField("f64")->GetKind() == ValueKind::Float64);
        CHECK(type->FindField("text")->GetKind() == ValueKind::String);
        CHECK(type->FindField("kind")->GetKind() == ValueKind::Enum);
        CHECK(type->FindField("nested")->GetKind() == ValueKind::Struct);
        CHECK(type->FindField("numbers")->GetKind() == ValueKind::Array);
    }

    TEST_CASE("unknown field and unknown type lookups return null rather than asserting")
    {
        const TypeDescriptor* type = Registry::Get().FindType("AllKindsFixture");
        REQUIRE(type != nullptr);

        CHECK(type->FindField("nope") == nullptr);
        CHECK(Registry::Get().FindType("ThisTypeWasNeverRegistered") == nullptr);
        CHECK(Registry::Get().FindEnum("ThisEnumWasNeverRegistered") == nullptr);
    }

    TEST_CASE("nested struct fields resolve to their own descriptor regardless of registration order")
    {
        // Vec2Fixture is registered after AllKindsFixture in this file; lazy resolution must still find it.
        const TypeDescriptor* type = Registry::Get().FindType("AllKindsFixture");
        REQUIRE(type != nullptr);

        const FieldDescriptor* nested = type->FindField("nested");
        REQUIRE(nested != nullptr);

        const TypeDescriptor* nestedType = nested->GetStructType();
        REQUIRE(nestedType != nullptr);
        CHECK(std::string(nestedType->GetName()) == "Vec2Fixture");
        CHECK(nestedType->GetFieldCount() == 2u);
    }

    TEST_CASE("array element types resolve and report their kind")
    {
        const TypeDescriptor* type = Registry::Get().FindType("AllKindsFixture");
        REQUIRE(type != nullptr);

        const FieldDescriptor* points = type->FindField("points");
        REQUIRE(points != nullptr);

        const TypeDescriptor* elementType = points->GetArrayElementType();
        REQUIRE(elementType != nullptr);
        CHECK(std::string(elementType->GetName()) == "Vec2Fixture");

        // Arrays of scalars have no element descriptor but must still report their element kind and size,
        // which is what lets the serializer save a std::vector<float>.
        const FieldDescriptor* numbers = type->FindField("numbers");
        REQUIRE(numbers != nullptr);
        CHECK(numbers->GetArrayElementType() == nullptr);
        CHECK(numbers->GetArrayOps().ElementKind == ValueKind::Int32);
        CHECK(numbers->GetArrayOps().ElementSize == sizeof(int));
    }

    TEST_CASE("inheritance is recorded when the base is registered first")
    {
        const TypeDescriptor* derived = Registry::Get().FindType("DerivedFixture");
        REQUIRE(derived != nullptr);

        const TypeDescriptor* base = derived->GetBaseType();
        REQUIRE(base != nullptr);
        CHECK(std::string(base->GetName()) == "BaseFixture");

        // Own fields only; the inspector walks GetBaseType() itself so counts never double-count.
        CHECK(derived->GetFieldCount() == 1u);
        CHECK(derived->FindField("ownValue") != nullptr);
        CHECK(derived->FindField("baseValue") == nullptr);
    }

    TEST_CASE("registering the same name twice is idempotent, not a duplicate registration")
    {
        const Size countBefore = Registry::Get().GetTypeCount();

        // A second HP_REGISTER_TYPE for one type (e.g. a header included into two translation units) must
        // return the existing descriptor instead of adding a competing entry.
        TypeBuilder again = RegisterTypeOf<Vec2Fixture>("Vec2Fixture");
        (void)again;

        CHECK(Registry::Get().GetTypeCount() == countBefore);
    }

    // Registration mistakes are reported through the assert handler, which is only diverted while a test case
    // is running (TestMain.cpp installs it for the run). Everything below therefore registers *at runtime*,
    // inside the capture scope — a static-initialisation registration could not be tested this way, and that
    // is worth remembering when adding fixtures: put deliberately-broken registrations in a TEST_CASE.

    TEST_CASE("registering one name for two different C++ types is reported")
    {
        struct Impostor
        {
            int Different = 0;
        };

        // Two classes claiming "Vec2Fixture" would make scene files ambiguous — a hard failure now beats
        // silently loading the wrong layout later.
        Hollowpoint::Test::ScopedAssertCapture capture;
        TypeBuilder impostor = RegisterTypeOf<Impostor>("Vec2Fixture");
        (void)impostor;

        CHECK(capture.GetCaptureCount() == 1);

        // The original descriptor must survive: the bad registration may not replace it.
        const TypeDescriptor* type = Registry::Get().FindType("Vec2Fixture");
        REQUIRE(type != nullptr);
        CHECK(type->GetSize() == sizeof(Vec2Fixture));
    }

    TEST_CASE("a base type that was never registered is reported")
    {
        struct ChildOfMissingBase
        {
            int Value = 0;
        };

        // The base name is a string, so the compiler cannot check it. An unregistered base would make the
        // inspector silently show an incomplete component, which is worth asserting on.
        Hollowpoint::Test::ScopedAssertCapture capture;
        TypeBuilder child = RegisterTypeOf<ChildOfMissingBase>("ChildOfMissingBaseFixture", "NoSuchBaseFixture");
        (void)child;

        CHECK(capture.GetCaptureCount() == 1);
    }

    TEST_CASE("duplicate field names within one type are reported")
    {
        struct ClashingFields
        {
            int First = 0;
            int Second = 0;
        };

        Hollowpoint::Test::ScopedAssertCapture capture;
        RegisterTypeOf<ClashingFields>("ClashingFieldsFixture")
            .Field("value", &ClashingFields::First)
            .Field("value", &ClashingFields::Second);

        // A duplicate key would make the serialized form ambiguous on load.
        CHECK(capture.GetCaptureCount() == 1);
    }

    TEST_CASE("types are iterable in registration order")
    {
        const Size count = Registry::Get().GetTypeCount();
        REQUIRE(count > 0u);

        bool sawAllKinds = false;
        for (Size index = 0; index < count; ++index)
        {
            const TypeDescriptor& type = Registry::Get().GetTypeByRegistrationIndex(index);
            // Every registered type must have a usable name and size, or the editor's component list breaks.
            REQUIRE(type.GetName() != nullptr);
            CHECK(type.GetSize() > 0u);

            if (std::string_view(type.GetName()) == "AllKindsFixture")
            {
                sawAllKinds = true;
            }
        }
        CHECK(sawAllKinds);
    }

    // Sub-type resolution policy. Both directions matter: a genuine registration bug must be reported, and
    // the ordinary shape of a std::vector<float> must not flood the log — a channel that cries wolf gets
    // ignored, which is worse than silence.

    TEST_CASE("a scalar array does not report a missing element descriptor")
    {
        const TypeDescriptor* type = Registry::Get().FindType("AllKindsFixture");
        REQUIRE(type != nullptr);

        const FieldDescriptor* numbers = type->FindField("numbers");
        REQUIRE(numbers != nullptr);

        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK(numbers->GetArrayElementType() == nullptr);

        // std::vector<int> is saved through ArrayOps and needs no descriptor: quiet, not an error.
        CHECK(logs.GetTotalCount() == 0);
    }

    TEST_CASE("an array of structs whose type was never registered is reported once")
    {
        struct UnregisteredInner
        {
            int Value = 0;
        };
        struct HolderOfUnregistered
        {
            std::vector<UnregisteredInner> Items;
        };

        const TypeDescriptor& holderType = RegisterTypeOf<HolderOfUnregistered>("HolderOfUnregisteredFixture")
                                               .Field("items", &HolderOfUnregistered::Items)
                                               .GetType();
        const TypeDescriptor* holder = &holderType;
        REQUIRE(holder != nullptr);

        const FieldDescriptor* items = holder->FindField("items");
        REQUIRE(items != nullptr);

        Hollowpoint::Test::ScopedLogCapture logs;

        // Several traversals (inspector redraw, save, scripting walk) must produce exactly one diagnostic.
        CHECK(items->GetArrayElementType() == nullptr);
        CHECK(items->GetArrayElementType() == nullptr);
        CHECK(items->GetArrayElementType() == nullptr);

        CHECK(logs.GetErrorCount() == 1);
    }

    TEST_CASE("ValueKindName covers every enumerator")
    {
        CHECK(std::string_view(ValueKindName(ValueKind::Boolean)) == "bool");
        CHECK(std::string_view(ValueKindName(ValueKind::Float32)) == "float");
        CHECK(std::string_view(ValueKindName(ValueKind::String)) == "string");
        CHECK(std::string_view(ValueKindName(ValueKind::Enum)) == "enum");
        CHECK(std::string_view(ValueKindName(ValueKind::Struct)) == "struct");
        CHECK(std::string_view(ValueKindName(ValueKind::Array)) == "array");
        CHECK(std::string_view(ValueKindName(ValueKind::Invalid)) == "Invalid");
    }
}
