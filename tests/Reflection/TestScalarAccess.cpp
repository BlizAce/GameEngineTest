#include "TestSupport.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Reflection/ScalarAccess.h"

using namespace Hollowpoint::Reflection;

namespace
{

struct ScalarFixture
{
    bool Flag = false;
    signed char I8 = 0;
    int I32 = 0;
    long long I64 = 0;
    unsigned char U8 = 0;
    unsigned int U32 = 0;
    float F32 = 0.0f;
    double F64 = 0.0;
    std::string Text;
};

enum class EChannel : unsigned char
{
    Red = 1,
    Green = 2,
    Blue = 3
};

// A signed underlying type on purpose: reading an enum through the wrong width or signedness is exactly the
// kind of bug that only shows up on one platform.
enum class ESignedChannel : int
{
    Minus = -5,
    Zero = 0,
    Plus = 5
};

struct EnumFixture
{
    EChannel Channel = EChannel::Red;
    ESignedChannel Signed = ESignedChannel::Zero;
};

struct ArrayFixture
{
    std::vector<int> Numbers;
    std::vector<float> Reals;
    std::vector<EChannel> Channels;
    std::string NotAnArray;
};

}  // namespace

HP_REGISTER_TYPE(ScalarFixture)
    .Field("flag", &ScalarFixture::Flag)
    .Field("i8", &ScalarFixture::I8)
    .Field("i32", &ScalarFixture::I32)
    .Field("i64", &ScalarFixture::I64)
    .Field("u8", &ScalarFixture::U8)
    .Field("u32", &ScalarFixture::U32)
    .Field("f32", &ScalarFixture::F32)
    .Field("f64", &ScalarFixture::F64)
    .Field("text", &ScalarFixture::Text);

HP_REGISTER_ENUM(EChannel).Value("Red", EChannel::Red).Value("Green", EChannel::Green).Value("Blue", EChannel::Blue);

HP_REGISTER_ENUM(ESignedChannel)
    .Value("Minus", ESignedChannel::Minus)
    .Value("Zero", ESignedChannel::Zero)
    .Value("Plus", ESignedChannel::Plus);

HP_REGISTER_TYPE(EnumFixture).Field("channel", &EnumFixture::Channel).Field("signed", &EnumFixture::Signed);

HP_REGISTER_TYPE(ArrayFixture)
    .Field("numbers", &ArrayFixture::Numbers)
    .Field("reals", &ArrayFixture::Reals)
    .Field("channels", &ArrayFixture::Channels)
    .Field("notAnArray", &ArrayFixture::NotAnArray);

TEST_SUITE("Reflection/ScalarAccess")
{
    TEST_CASE("scalars round-trip exactly through their own kind")
    {
        ScalarFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        CHECK(WriteFieldValue(&instance, *type->FindField("flag"), ScalarValue::FromBool(true)));
        CHECK(instance.Flag == true);

        CHECK(WriteFieldValue(&instance, *type->FindField("i8"), ScalarValue::FromSigned(ValueKind::Int8, -100)));
        CHECK(instance.I8 == -100);

        CHECK(WriteFieldValue(&instance, *type->FindField("i32"), ScalarValue::FromSigned(ValueKind::Int32, -123456)));
        CHECK(instance.I32 == -123456);

        // int64 must keep all 64 bits — a silent narrowing to int here would be invisible in a 32-bit test.
        const long long bigValue = 8000000000000LL;
        CHECK(WriteFieldValue(&instance, *type->FindField("i64"), ScalarValue::FromSigned(ValueKind::Int64, bigValue)));
        CHECK(instance.I64 == bigValue);

        CHECK(WriteFieldValue(&instance, *type->FindField("u8"), ScalarValue::FromUnsigned(ValueKind::UInt8, 250)));
        CHECK(instance.U8 == 250);

        CHECK(WriteFieldValue(&instance, *type->FindField("f32"), ScalarValue::FromReal(ValueKind::Float32, 1.5)));
        CHECK(instance.F32 == doctest::Approx(1.5f));

        ScalarValue read{};
        REQUIRE(ReadFieldValue(&instance, *type->FindField("i64"), read));
        CHECK(read.Kind == ValueKind::Int64);
        CHECK(read.AsInt == bigValue);
    }

    TEST_CASE("reads preserve the declared kind rather than collapsing to one number type")
    {
        ScalarFixture instance{};
        instance.U8 = 200;
        instance.I8 = -3;

        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        ScalarValue read{};
        REQUIRE(ReadFieldValue(&instance, *type->FindField("u8"), read));
        CHECK(read.Kind == ValueKind::UInt8);
        CHECK(read.AsUnsigned == 200u);

        REQUIRE(ReadFieldValue(&instance, *type->FindField("i8"), read));
        CHECK(read.Kind == ValueKind::Int8);
        // A signed read must not be reinterpreted as unsigned: -3 stays -3.
        CHECK(read.AsInt == -3);
    }

    TEST_CASE("narrowing writes clamp instead of wrapping")
    {
        ScalarFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        // 300 into an int8: wrapping would give 44, which is the bug this guards.
        CHECK(WriteFieldValue(&instance, *type->FindField("i8"), ScalarValue::FromSigned(ValueKind::Int32, 300)));
        CHECK(instance.I8 == 127);

        CHECK(WriteFieldValue(&instance, *type->FindField("i8"), ScalarValue::FromSigned(ValueKind::Int32, -300)));
        CHECK(instance.I8 == -128);

        // A negative value into an unsigned field saturates to zero instead of becoming 253.
        CHECK(WriteFieldValue(&instance, *type->FindField("u8"), ScalarValue::FromSigned(ValueKind::Int32, -5)));
        CHECK(instance.U8 == 0);

        CHECK(WriteFieldValue(&instance, *type->FindField("u8"), ScalarValue::FromUnsigned(ValueKind::UInt32, 5000)));
        CHECK(instance.U8 == 255);

        // A double into an int truncates toward zero after clamping.
        CHECK(WriteFieldValue(&instance, *type->FindField("i32"), ScalarValue::FromReal(ValueKind::Float64, 2.9)));
        CHECK(instance.I32 == 2);

        CHECK(WriteFieldValue(&instance, *type->FindField("i32"), ScalarValue::FromReal(ValueKind::Float64, -2.9)));
        CHECK(instance.I32 == -2);
    }

    TEST_CASE("NaN never reaches a reflected field")
    {
        ScalarFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        const double nan = std::numeric_limits<double>::quiet_NaN();

        // A NaN index or count would poison every downstream computation, so it degrades to zero.
        CHECK(WriteFieldValue(&instance, *type->FindField("i32"), ScalarValue::FromReal(ValueKind::Float64, nan)));
        CHECK(instance.I32 == 0);

        CHECK(WriteFieldValue(&instance, *type->FindField("u8"), ScalarValue::FromReal(ValueKind::Float64, nan)));
        CHECK(instance.U8 == 0);

        CHECK(WriteFieldValue(&instance, *type->FindField("f32"), ScalarValue::FromReal(ValueKind::Float64, nan)));
        CHECK(std::isnan(instance.F32));  // A float field may legitimately hold NaN; only integers are guarded.
    }

    TEST_CASE("huge values saturate instead of producing garbage")
    {
        ScalarFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        // A double beyond int64 range: the cast is undefined behaviour if unclamped.
        CHECK(WriteFieldValue(&instance, *type->FindField("i64"), ScalarValue::FromReal(ValueKind::Float64, 1.0e30)));
        CHECK(instance.I64 == std::numeric_limits<long long>::max());

        // Beyond float range saturates to the largest finite float rather than infinity, keeping downstream
        // rendering and gameplay arithmetic finite.
        CHECK(WriteFieldValue(&instance, *type->FindField("f32"), ScalarValue::FromReal(ValueKind::Float64, 1.0e300)));
        CHECK(instance.F32 == std::numeric_limits<float>::max());
    }

    TEST_CASE("bool fields always hold a canonical 0 or 1")
    {
        ScalarFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        CHECK(WriteFieldValue(&instance, *type->FindField("flag"), ScalarValue::FromReal(ValueKind::Float64, 3.5)));
        CHECK(instance.Flag == true);

        CHECK(WriteFieldValue(&instance, *type->FindField("flag"), ScalarValue::FromSigned(ValueKind::Int32, 0)));
        CHECK(instance.Flag == false);

        // Reading back must give a strict bool, since the engine branches on it.
        ScalarValue read{};
        REQUIRE(ReadFieldValue(&instance, *type->FindField("flag"), read));
        CHECK(read.Kind == ValueKind::Boolean);
        CHECK(read.AsBool == false);
    }

    TEST_CASE("string fields round-trip and reject scalar access")
    {
        ScalarFixture instance;
        instance.Text = "player_spawn";

        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        std::string_view text;
        REQUIRE(ReadStringField(&instance, *type->FindField("text"), text));
        CHECK(text == "player_spawn");

        CHECK(WriteStringField(&instance, *type->FindField("text"), "checkpoint_b"));
        CHECK(instance.Text == "checkpoint_b");

        // A string is not a scalar: reading it as one must fail rather than return the first bytes of the
        // std::string's internal buffer.
        ScalarValue read{};
        CHECK_FALSE(ReadFieldValue(&instance, *type->FindField("text"), read));
    }

    TEST_CASE("scalar access on a mismatched field reports and fails")
    {
        ScalarFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("ScalarFixture");
        REQUIRE(type != nullptr);

        std::string_view text;
        // Asking for a string from an int field is a caller bug: it must be reported, not coerced.
        Hollowpoint::Test::ScopedLogCapture logs;
        CHECK_FALSE(ReadStringField(&instance, *type->FindField("i32"), text));
        CHECK(logs.GetErrorCount() == 1);
    }

    TEST_CASE("enums read as int64 whatever their underlying type")
    {
        EnumFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("EnumFixture");
        REQUIRE(type != nullptr);

        instance.Channel = EChannel::Blue;
        ScalarValue read{};
        REQUIRE(ReadFieldValue(&instance, *type->FindField("channel"), read));
        CHECK(read.Kind == ValueKind::Enum);
        CHECK(read.AsInt == 3);

        // The signed underlying type must not be reinterpreted as unsigned.
        instance.Signed = ESignedChannel::Minus;
        REQUIRE(ReadFieldValue(&instance, *type->FindField("signed"), read));
        CHECK(read.AsInt == -5);
    }

    TEST_CASE("enum writes clamp to the underlying range and never trap")
    {
        EnumFixture instance{};
        const TypeDescriptor* type = Registry::Get().FindType("EnumFixture");
        REQUIRE(type != nullptr);

        // EChannel's underlying type is unsigned char, so a negative write must not wrap to 251.
        CHECK(WriteFieldValue(&instance, *type->FindField("channel"), ScalarValue::FromSigned(ValueKind::Int32, -1)));
        CHECK(instance.Channel == static_cast<EChannel>(0));

        CHECK(WriteFieldValue(&instance, *type->FindField("channel"), ScalarValue::FromSigned(ValueKind::Int32, 999)));
        CHECK(static_cast<int>(instance.Channel) == 255);

        // A value outside the declared enumerators is still representable (the scene format may legitimately
        // contain one from a newer build), so no error is raised — only clamping happens.
        CHECK(WriteFieldValue(&instance, *type->FindField("channel"), ScalarValue::FromSigned(ValueKind::Int32, 42)));
        CHECK(static_cast<int>(instance.Channel) == 42);
    }

    TEST_CASE("enum descriptors map names to values in both directions")
    {
        const EnumDescriptor* descriptor = Registry::Get().FindEnum("EChannel");
        REQUIRE(descriptor != nullptr);

        CHECK(descriptor->GetEntryCount() == 3u);
        CHECK(std::string(descriptor->GetEntry(0).Name) == "Red");
        CHECK(descriptor->GetEntry(0).Value == 1);

        Int64 value = 0;
        CHECK(descriptor->FindValueByName("Green", value));
        CHECK(value == 2);

        CHECK_FALSE(descriptor->FindValueByName("Magenta", value));

        const char* name = descriptor->FindNameByValue(3);
        REQUIRE(name != nullptr);
        CHECK(std::string(name) == "Blue");

        // A value from a newer build has no name; callers fall back to the number instead of asserting.
        CHECK(descriptor->FindNameByValue(77) == nullptr);
    }

    TEST_CASE("a field finds its enum descriptor")
    {
        const TypeDescriptor* type = Registry::Get().FindType("EnumFixture");
        REQUIRE(type != nullptr);

        const FieldDescriptor* field = type->FindField("channel");
        REQUIRE(field != nullptr);

        const EnumDescriptor* descriptor = field->GetEnum();
        REQUIRE(descriptor != nullptr);
        CHECK(std::string(descriptor->GetName()) == "EChannel");

        // A non-enum field must not resolve an enum.
        const TypeDescriptor* scalarType = Registry::Get().FindType("ScalarFixture");
        REQUIRE(scalarType != nullptr);
        CHECK(scalarType->FindField("i32")->GetEnum() == nullptr);
    }
}

TEST_SUITE("Reflection/ArrayAccess")
{
    TEST_CASE("array count follows the live vector")
    {
        ArrayFixture instance;
        const TypeDescriptor* type = Registry::Get().FindType("ArrayFixture");
        REQUIRE(type != nullptr);

        const FieldDescriptor* numbers = type->FindField("numbers");
        REQUIRE(numbers != nullptr);

        CHECK(GetArrayCount(&instance, *numbers) == 0u);

        instance.Numbers = {10, 20, 30};
        CHECK(GetArrayCount(&instance, *numbers) == 3u);

        // A non-array field reports zero rather than reading unrelated memory.
        CHECK(GetArrayCount(&instance, *type->FindField("notAnArray")) == 0u);
    }

    TEST_CASE("scalar array elements read and write by index")
    {
        ArrayFixture instance;
        instance.Numbers = {10, 20, 30};
        instance.Reals = {1.5f, 2.5f};

        const TypeDescriptor* type = Registry::Get().FindType("ArrayFixture");
        REQUIRE(type != nullptr);

        ScalarValue read{};
        REQUIRE(ReadArrayElementScalar(&instance, *type->FindField("numbers"), 1, read));
        CHECK(read.Kind == ValueKind::Int32);
        CHECK(read.AsInt == 20);

        REQUIRE(ReadArrayElementScalar(&instance, *type->FindField("reals"), 1, read));
        CHECK(read.Kind == ValueKind::Float32);
        CHECK(read.AsReal == doctest::Approx(2.5));

        CHECK(WriteArrayElementScalar(&instance, *type->FindField("numbers"), 0,
                                      ScalarValue::FromSigned(ValueKind::Int32, -7)));
        CHECK(instance.Numbers[0] == -7);

        // Out-of-range indexes fail instead of reading past the buffer.
        CHECK_FALSE(ReadArrayElementScalar(&instance, *type->FindField("numbers"), 99, read));
        CHECK_FALSE(WriteArrayElementScalar(&instance, *type->FindField("numbers"), 99,
                                            ScalarValue::FromSigned(ValueKind::Int32, 0)));
    }

    TEST_CASE("array element writes clamp exactly like field writes")
    {
        // This is the property that makes the shared conversion rules worth factoring out: an element written
        // through the array path must store the same bytes as the same value written to a plain field.
        ArrayFixture instance;
        instance.Numbers = {0};

        const TypeDescriptor* type = Registry::Get().FindType("ArrayFixture");
        REQUIRE(type != nullptr);

        CHECK(WriteArrayElementScalar(&instance, *type->FindField("numbers"), 0,
                                      ScalarValue::FromReal(ValueKind::Float64, -2.7)));
        CHECK(instance.Numbers[0] == -2);
    }

    TEST_CASE("enum arrays read their elements as enum scalars")
    {
        ArrayFixture instance;
        instance.Channels = {EChannel::Red, EChannel::Blue};

        const TypeDescriptor* type = Registry::Get().FindType("ArrayFixture");
        REQUIRE(type != nullptr);

        ScalarValue read{};
        REQUIRE(ReadArrayElementScalar(&instance, *type->FindField("channels"), 1, read));
        CHECK(read.Kind == ValueKind::Enum);
        CHECK(read.AsInt == 3);

        CHECK(WriteArrayElementScalar(&instance, *type->FindField("channels"), 0,
                                      ScalarValue::FromSigned(ValueKind::Enum, 2)));
        CHECK(instance.Channels[0] == EChannel::Green);
    }

    TEST_CASE("resize grows with default-constructed elements and shrinks")
    {
        ArrayFixture instance;
        const TypeDescriptor* type = Registry::Get().FindType("ArrayFixture");
        REQUIRE(type != nullptr);

        void* first = ResizeArrayField(&instance, *type->FindField("numbers"), 4);
        REQUIRE(first != nullptr);
        CHECK(instance.Numbers.size() == 4u);
        // std::vector value-initialises on resize, so a fresh scene array element starts at zero.
        CHECK(instance.Numbers[0] == 0);
        CHECK(static_cast<int*>(first) == instance.Numbers.data());

        CHECK(ResizeArrayField(&instance, *type->FindField("numbers"), 2) != nullptr);
        CHECK(instance.Numbers.size() == 2u);

        // A non-array field cannot be resized.
        CHECK(ResizeArrayField(&instance, *type->FindField("notAnArray"), 3) == nullptr);
    }

    TEST_CASE("push_back returns the address of the new element even after reallocation")
    {
        ArrayFixture instance;
        const TypeDescriptor* type = Registry::Get().FindType("ArrayFixture");
        REQUIRE(type != nullptr);

        const FieldDescriptor* numbers = type->FindField("numbers");
        REQUIRE(numbers != nullptr);

        // Fill past the initial capacity so the buffer definitely moves during at least one push.
        for (int iteration = 0; iteration < 64; ++iteration)
        {
            void* address = PushBackArrayField(&instance, *numbers);
            REQUIRE(address != nullptr);
            *static_cast<int*>(address) = iteration;
        }

        REQUIRE(instance.Numbers.size() == 64u);
        for (int iteration = 0; iteration < 64; ++iteration)
        {
            // If push_back had returned data() instead of the new element, every slot would hold 63.
            CHECK(instance.Numbers[static_cast<std::size_t>(iteration)] == iteration);
        }
    }

    TEST_CASE("element addresses are contiguous with the element stride")
    {
        ArrayFixture instance;
        instance.Numbers = {1, 2, 3};

        const TypeDescriptor* type = Registry::Get().FindType("ArrayFixture");
        REQUIRE(type != nullptr);

        const void* element0 = GetArrayElementAddress(&instance, *type->FindField("numbers"), 0);
        const void* element2 = GetArrayElementAddress(&instance, *type->FindField("numbers"), 2);

        REQUIRE(element0 != nullptr);
        REQUIRE(element2 != nullptr);
        CHECK(static_cast<const std::byte*>(element2) - static_cast<const std::byte*>(element0) ==
              2 * static_cast<std::ptrdiff_t>(sizeof(int)));
        CHECK(element2 == static_cast<const void*>(&instance.Numbers[2]));

        CHECK(GetArrayElementAddress(&instance, *type->FindField("numbers"), 3) == nullptr);
    }
}
