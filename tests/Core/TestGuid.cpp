#include <unordered_set>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Guid.h"

using namespace Hollowpoint::Core;

TEST_SUITE("Core/Guid")
{
    TEST_CASE("default constructed Guid is nil")
    {
        Guid guid;
        CHECK(guid.IsNil());
        CHECK(guid == Guid{});
    }

    TEST_CASE("Generate never produces the nil Guid and always yields distinct values")
    {
        std::unordered_set<Guid> generated;
        for (int index = 0; index < 5000; ++index)
        {
            const Guid guid = Guid::Generate();
            REQUIRE_FALSE(guid.IsNil());
            generated.insert(guid);
        }
        // A collision among 5000 v4 Guids would indicate a broken generator, not bad luck.
        CHECK(generated.size() == 5000);
    }

    TEST_CASE("ToString produces the canonical 8-4-4-4-12 form")
    {
        const Guid guid(0x01234567u, 0x89abcdefu, 0xfedcba98u, 0x76543210u);
        const std::string text = guid.ToString();

        CHECK(text.size() == 36);
        CHECK(text[8] == '-');
        CHECK(text[13] == '-');
        CHECK(text[18] == '-');
        CHECK(text[23] == '-');
        CHECK(text == "01234567-89ab-cdef-fedc-ba9876543210");
    }

    TEST_CASE("Parse round-trips ToString for generated Guids")
    {
        for (int index = 0; index < 200; ++index)
        {
            const Guid guid = Guid::Generate();
            const std::optional<Guid> parsed = Guid::Parse(guid.ToString());
            REQUIRE(parsed.has_value());
            CHECK(*parsed == guid);
        }
    }

    TEST_CASE("Parse accepts upper case and missing hyphens")
    {
        constexpr std::string_view canonical = "01234567-89ab-cdef-fedc-ba9876543210";
        const Guid expected = Guid::ParseOrNil(canonical);
        REQUIRE_FALSE(expected.IsNil());

        CHECK(Guid::ParseOrNil("0123456789ABCDEF-FEDCBA9876543210") == expected);
        CHECK(Guid::ParseOrNil("01234567-89AB-CDEF-FEDC-BA9876543210") == expected);
    }

    TEST_CASE("Parse rejects malformed input")
    {
        CHECK_FALSE(Guid::Parse("").has_value());
        CHECK_FALSE(Guid::Parse("not-a-guid").has_value());
        CHECK_FALSE(Guid::Parse("01234567-89ab-cdef-fedc-ba987654321").has_value());    // too short
        CHECK_FALSE(Guid::Parse("01234567-89ab-cdef-fedc-ba9876543210f").has_value());  // too long
        CHECK_FALSE(Guid::Parse("01234567-89ab-cdef-fedc-ba987654321g").has_value());   // bad hex
        CHECK(Guid::ParseOrNil("garbage").IsNil());
    }

    TEST_CASE("ToBytes is big-endian and 16 bytes long")
    {
        const Guid guid(0x01234567u, 0x89abcdefu, 0xfedcba98u, 0x76543210u);
        const std::array<UInt8, 16> bytes = guid.ToBytes();
        CHECK(bytes[0] == 0x01);
        CHECK(bytes[1] == 0x23);
        CHECK(bytes[2] == 0x45);
        CHECK(bytes[3] == 0x67);
        CHECK(bytes[4] == 0x89);
        CHECK(bytes[7] == 0xef);
        CHECK(bytes[15] == 0x10);
    }

    TEST_CASE("version and variant bits follow RFC 4122 v4")
    {
        for (int index = 0; index < 100; ++index)
        {
            const Guid guid = Guid::Generate();
            const std::array<UInt8, 16> bytes = guid.ToBytes();
            CHECK((bytes[6] >> 4) == 4);     // version 4
            CHECK((bytes[8] >> 6) == 0b10);  // RFC 4122 variant
        }
    }

    TEST_CASE("Guid hashes are well distributed and stable")
    {
        const Guid guid(0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u);
        CHECK(GuidHash{}(guid) == GuidHash{}(guid));

        // Distinct Guids must not hash identically in bulk (a broken mixer would collide constantly).
        std::unordered_set<std::size_t> hashes;
        for (int index = 0; index < 2000; ++index)
        {
            hashes.insert(GuidHash{}(Guid::Generate()));
        }
        CHECK(hashes.size() > 1990);
    }

    TEST_CASE("ordering is a total order over the four words")
    {
        const Guid a(1, 0, 0, 0);
        const Guid b(1, 0, 0, 1);
        const Guid c(2, 0, 0, 0);

        CHECK(a < b);
        CHECK(b < c);
        CHECK(c > a);
        CHECK(a == Guid(1, 0, 0, 0));
        CHECK(a != b);
    }
}
