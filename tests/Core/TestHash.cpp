#include <doctest/doctest.h>

#include "Hollowpoint/Core/Hash.h"

using namespace Hollowpoint::Core;

TEST_SUITE("Core/Hash")
{
    TEST_CASE("HashString is FNV-1a 64 and matches known reference values")
    {
        // Reference values from the FNV-1a specification/test vectors.
        CHECK(HashString("") == FnvOffsetBasis);
        CHECK(HashString("a") == 0xaf63dc4c8601ec8cull);
        CHECK(HashString("foobar") == 0x85944171f73967e8ull);
    }

    TEST_CASE("HashBytes and HashString agree for the same bytes")
    {
        const char text[] = "mesh::vertex";
        CHECK(HashBytes(text, sizeof(text) - 1) == HashString(std::string_view(text)));
    }

    TEST_CASE("hashes distinguish nearby strings")
    {
        CHECK(HashString("diffuse") != HashString("Diffuse"));
        CHECK(HashString("a") != HashString("b"));
        CHECK(HashString("aaaaaaaaab") != HashString("aaaaaaaaac"));
    }

    TEST_CASE("case-insensitive hashing folds ASCII case only")
    {
        CHECK(HashStringCaseInsensitive("Transform") == HashStringCaseInsensitive("transform"));
        CHECK(HashStringCaseInsensitive("TRANSFORM") == HashStringCaseInsensitive("tRaNsFoRm"));
        CHECK(HashStringCaseInsensitive("Transform") != HashString("Transform"));

        // Non-ASCII bytes must pass through unchanged (no locale-dependent folding).
        CHECK(HashStringCaseInsensitive(std::string_view("\xC3\xA9", 2)) ==
              HashString(std::string_view("\xC3\xA9", 2)));
    }

    TEST_CASE("CombineHash is order sensitive and avalanche-like")
    {
        const HashValue a = HashString("entity");
        const HashValue b = HashString("transform");

        CHECK(CombineHash(a, b) != CombineHash(b, a));
        CHECK(CombineHash(a, b) == CombineHash(a, b));

        // Changing one input must change many output bits — a weak mixer would leave most bits equal.
        const HashValue first = CombineHash(a, b);
        const HashValue second = CombineHash(a, HashString("transform2"));
        int differingBits = 0;
        for (HashValue diff = first ^ second; diff != 0; diff >>= 1)
        {
            differingBits += static_cast<int>(diff & 1ull);
        }
        CHECK(differingBits > 16);
    }

    TEST_CASE("HashValueOf handles integers, enums and floats consistently")
    {
        CHECK(HashValueOf(1) == HashValueOf(1));
        CHECK(HashValueOf(1) != HashValueOf(2));
        CHECK(HashValueOf(-1) != HashValueOf(1));

        enum class ComponentType : UInt32
        {
            Transform = 7,
            Mesh = 9
        };
        CHECK(HashValueOf(ComponentType::Transform) != HashValueOf(ComponentType::Mesh));

        // Equal floating point values hash equal, including the -0.0/+0.0 special case.
        CHECK(HashValueOf(1.5f) == HashValueOf(1.5f));
        CHECK(HashValueOf(0.0) == HashValueOf(-0.0));
        CHECK(HashValueOf(1.0f) != HashValueOf(2.0f));
    }

    TEST_CASE("the string hash functor agrees with HashString")
    {
        const std::string_view text = "assets/materials/metal.json";
        Hollowpoint::Core::Hash hasher;
        CHECK(hasher(text) == static_cast<std::size_t>(HashString(text)));
    }
}
