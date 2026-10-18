#include "TestSupport.h"

#include <cmath>
#include <set>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Math/MathFunctions.h"
#include "Hollowpoint/Math/Random.h"

using namespace Hollowpoint::Math;
using Hollowpoint::Test::ScopedAssertCapture;

namespace
{

// Chi-squared goodness-of-fit for k equally likely buckets, compared against a generous critical value.
// With 10 000 samples and 20 buckets the 1% critical value is ~43.8; we use 60 to keep the test stable
// while still catching a badly biased generator.
[[nodiscard]] int ChiSquaredUniformity(const std::vector<Index>& bucketCounts, Index bucketCount)
{
    double total = 0.0;
    for (const Index count : bucketCounts)
    {
        total += static_cast<double>(count);
    }
    const double expected = total / static_cast<double>(bucketCount);

    double chiSquared = 0.0;
    for (const Index count : bucketCounts)
    {
        const double difference = static_cast<double>(count) - expected;
        chiSquared += difference * difference / expected;
    }
    return static_cast<int>(chiSquared * 100.0);  // integer percent, comparable in CHECKs
}

}  // namespace

TEST_SUITE("Math/Random")
{
    TEST_CASE("the same seed produces the same sequence")
    {
        RandomGenerator a(12345);
        RandomGenerator b(12345);

        for (int index = 0; index < 100; ++index)
        {
            CHECK(a.NextUInt64() == b.NextUInt64());
        }
    }

    TEST_CASE("different seeds produce different sequences")
    {
        RandomGenerator a(1);
        RandomGenerator b(2);

        bool anyDifference = false;
        for (int index = 0; index < 20; ++index)
        {
            anyDifference |= (a.NextUInt64() != b.NextUInt64());
        }
        CHECK(anyDifference);
    }

    TEST_CASE("SetSeed restarts the stream")
    {
        RandomGenerator generator(999);
        const UInt64 first = generator.NextUInt64();
        generator.SetSeed(999);
        CHECK(generator.NextUInt64() == first);
    }

    TEST_CASE("state can be saved and restored for replays")
    {
        RandomGenerator generator(4242);
        generator.Discard(10);

        const std::array<UInt64, 4> savedState = generator.GetState();

        // Float draws consume exactly one state step each, so a restored stream must reproduce them
        // bit-for-bit. (NextNormal is deliberately excluded: Box-Muller caches its partner sample, so
        // two streams only stay in lock-step when the save point falls on a pair boundary — see the
        // determinism note in Random.h.)
        std::vector<Float32> forward;
        for (int index = 0; index < 20; ++index)
        {
            forward.push_back(generator.NextFloat01());
        }

        RandomGenerator restored(0);
        restored.SetState(savedState);
        for (int index = 0; index < 20; ++index)
        {
            CHECK(restored.NextFloat01() == forward[static_cast<std::size_t>(index)]);
        }
    }

    TEST_CASE("independent streams are not correlated")
    {
        // A naive implementation (seeding each stream with seed+i) produces highly correlated first
        // outputs; compare a handful of draws from two neighbouring streams.
        RandomGenerator streamA = RandomGenerator::FromStream(1000, 0);
        RandomGenerator streamB = RandomGenerator::FromStream(1000, 1);

        int matchingDraws = 0;
        for (int index = 0; index < 50; ++index)
        {
            if (streamA.NextUInt64() == streamB.NextUInt64())
            {
                ++matchingDraws;
            }
        }
        CHECK(matchingDraws == 0);
    }

    TEST_CASE("NextFloat01 stays inside [0,1) and uses the full range")
    {
        RandomGenerator generator(7);
        Float32 minimum = 2.0f;
        Float32 maximum = -1.0f;

        constexpr int SampleCount = 100000;
        for (int sample = 0; sample < SampleCount; ++sample)
        {
            const Float32 value = generator.NextFloat01();
            REQUIRE(value >= 0.0f);
            REQUIRE(value < 1.0f);
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }

        // With 100k samples the extremes should be very close to both ends.
        CHECK(minimum < 0.001f);
        CHECK(maximum > 0.999f);
    }

    TEST_CASE("NextDouble01 keeps full double precision")
    {
        RandomGenerator generator(11);
        bool sawFineGranularity = false;
        for (int sample = 0; sample < 1000; ++sample)
        {
            const Float64 value = generator.NextDouble01();
            REQUIRE(value >= 0.0);
            REQUIRE(value < 1.0);
            // A float-based implementation would produce values that round-trip exactly through float.
            if (static_cast<Float64>(static_cast<Float32>(value)) != value)
            {
                sawFineGranularity = true;
            }
        }
        CHECK(sawFineGranularity);
    }

    TEST_CASE("NextInt respects the inclusive range and covers it")
    {
        RandomGenerator generator(2024);
        std::set<Int32> seen;

        for (int sample = 0; sample < 5000; ++sample)
        {
            const Int32 value = generator.NextInt(3, 7);
            REQUIRE(value >= 3);
            REQUIRE(value <= 7);
            seen.insert(value);
        }
        CHECK(seen.size() == 5u);

        // Degenerate ranges return the single value.
        CHECK(generator.NextInt(5, 5) == 5);
        CHECK(generator.NextInt(4) >= 0);
        CHECK(generator.NextInt(4) <= 3);

        // Negative ranges work too (this is where naive unsigned arithmetic goes wrong).
        for (int sample = 0; sample < 1000; ++sample)
        {
            const Int32 value = generator.NextInt(-5, -1);
            REQUIRE(value >= -5);
            REQUIRE(value <= -1);
        }
    }

    TEST_CASE("NextIndex is uniform and rejects a zero count")
    {
        RandomGenerator generator(31337);
        constexpr Index BucketCount = 20;
        std::vector<Index> counts(BucketCount, 0);

        for (int sample = 0; sample < 20000; ++sample)
        {
            const Index index = generator.NextIndex(BucketCount);
            REQUIRE(index < BucketCount);
            ++counts[index];
        }

        // No bucket may be starved, and the distribution must pass a chi-squared check.
        for (const Index count : counts)
        {
            CHECK(count > 700);
        }
        CHECK(ChiSquaredUniformity(counts, BucketCount) < 6000);

        ScopedAssertCapture capture;
        const Index invalid = generator.NextIndex(0);
        HP_UNUSED(invalid);
        CHECK(capture.GetCaptureCount() == 1);
    }

    TEST_CASE("NextFloat range helper")
    {
        RandomGenerator generator(5);
        for (int sample = 0; sample < 1000; ++sample)
        {
            const Float32 value = generator.NextFloat(-2.0f, 3.0f);
            REQUIRE(value >= -2.0f);
            REQUIRE(value < 3.0f);
        }

        ScopedAssertCapture capture;
        const Float32 bad = generator.NextFloat(1.0f, 1.0f);
        HP_UNUSED(bad);
        CHECK(capture.GetCaptureCount() == 1);
    }

    TEST_CASE("NextNormal has the requested mean and spread")
    {
        RandomGenerator generator(6060);
        constexpr int SampleCount = 50000;

        double sum = 0.0;
        for (int sample = 0; sample < SampleCount; ++sample)
        {
            const Float32 value = generator.NextNormal(10.0f, 2.0f);
            REQUIRE(IsFinite(value));
            sum += value;
        }

        const double mean = sum / static_cast<double>(SampleCount);
        CHECK(mean == doctest::Approx(10.0).epsilon(0.02));

        // Second pass for the standard deviation (Box-Muller caches a partner sample, so re-seed to
        // keep the two passes independent).
        RandomGenerator secondGenerator(6061);
        double sumSquares = 0.0;
        for (int sample = 0; sample < SampleCount; ++sample)
        {
            const Float32 value = secondGenerator.NextNormal(0.0f, 1.0f);
            sumSquares += static_cast<double>(value) * value;
        }
        const double variance = sumSquares / static_cast<double>(SampleCount);
        CHECK(std::sqrt(variance) == doctest::Approx(1.0).epsilon(0.05));
    }

    TEST_CASE("NextUnitVector always yields unit length")
    {
        RandomGenerator generator(88);
        for (int sample = 0; sample < 2000; ++sample)
        {
            const Vec3 direction = generator.NextUnitVector();
            CHECK(direction.Length() == doctest::Approx(1.0f).epsilon(1e-4f));
        }
    }

    TEST_CASE("NextQuaternion produces unit quaternions")
    {
        RandomGenerator generator(77);
        for (int sample = 0; sample < 2000; ++sample)
        {
            const Quat rotation = generator.NextQuaternion();
            CHECK(rotation.Length() == doctest::Approx(1.0f).epsilon(1e-4f));
            CHECK(IsFinite(rotation));
        }
    }

    TEST_CASE("NextBool honours the probability")
    {
        RandomGenerator generator(3);
        CHECK(generator.NextBool(0.0f) == false);
        CHECK(generator.NextBool(1.0f) == true);

        int trueCount = 0;
        constexpr int SampleCount = 20000;
        for (int sample = 0; sample < SampleCount; ++sample)
        {
            if (generator.NextBool(0.3f))
            {
                ++trueCount;
            }
        }
        const Float32 observedProbability = static_cast<Float32>(trueCount) / static_cast<Float32>(SampleCount);
        CHECK(observedProbability == doctest::Approx(0.3f).epsilon(0.05f));
    }

    TEST_CASE("MakeNonReproducibleSeed gives different values across calls")
    {
        // Not a guarantee (a collision is possible), but four identical 64-bit values from a hardware
        // seed source would mean the entropy is broken.
        const UInt64 first = RandomGenerator::MakeNonReproducibleSeed();
        const UInt64 second = RandomGenerator::MakeNonReproducibleSeed();
        CHECK(first != second);
    }

    TEST_CASE("a zero seed still produces output (no degenerate fixed point)")
    {
        RandomGenerator generator(0);
        const UInt64 first = generator.NextUInt64();
        const UInt64 second = generator.NextUInt64();
        CHECK(first != second);
        CHECK(first != 0u);
    }
}
