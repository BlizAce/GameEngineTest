#include <cmath>
#include <random>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Math/MathFunctions.h"
#include "Hollowpoint/Math/Random.h"

namespace Hollowpoint::Math
{

namespace
{

//! SplitMix64 — used only to expand a single seed word into the generator's full state.
[[nodiscard]] inline UInt64 SplitMix64(UInt64& value)
{
    value += 0x9e3779b97f4a7c15ull;
    UInt64 mixed = value;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebull;
    return mixed ^ (mixed >> 31);
}

[[nodiscard]] inline UInt64 RotateLeft(UInt64 value, int count)
{
    return (value << count) | (value >> (64 - count));
}

}  // namespace

RandomGenerator::RandomGenerator(UInt64 seed)
{
    SetSeed(seed);
}

UInt64 RandomGenerator::MakeNonReproducibleSeed()
{
    std::random_device randomDevice;
    UInt64 seed = 0;
    for (int index = 0; index < 4; ++index)
    {
        seed = (seed << 32) ^ static_cast<UInt64>(randomDevice());
    }
    return seed;
}

RandomGenerator RandomGenerator::FromStream(UInt64 seed, UInt64 streamIndex)
{
    // Mix the stream index into the seed with a hash finalizer before expanding, so neighbouring
    // indices land far apart in the state space instead of producing correlated sequences.
    const HashValue mixed = Core::CombineHash(seed, streamIndex + 0x517cc1b727220a95ull);
    return RandomGenerator(static_cast<UInt64>(mixed));
}

void RandomGenerator::SetSeed(UInt64 seed)
{
    UInt64 stateSeed = seed;
    for (int index = 0; index < 4; ++index)
    {
        m_State[static_cast<std::size_t>(index)] = SplitMix64(stateSeed);
    }

    // A fully zero state is a fixed point of xoshiro; the SplitMix64 expansion makes that impossible
    // unless the seed itself produced four zero words, which we guard anyway.
    if (m_State[0] == 0 && m_State[1] == 0 && m_State[2] == 0 && m_State[3] == 0)
    {
        m_State[0] = kDefaultSeed;
    }

    m_HasCachedNormal = false;
    m_CachedNormal = 0.0f;
}

void RandomGenerator::Discard(UInt64 count)
{
    for (UInt64 index = 0; index < count; ++index)
    {
        const UInt64 discarded = NextUInt64();
        HP_UNUSED(discarded);
    }
}

void RandomGenerator::SetState(const std::array<UInt64, 4>& state)
{
    m_State = state;
    m_HasCachedNormal = false;
    m_CachedNormal = 0.0f;
}

UInt64 RandomGenerator::NextUInt64()
{
    // xoshiro256++ — result from state[0] + state[3], rotated, then the usual xorshift update.
    const UInt64 result = RotateLeft(m_State[0] + m_State[3], 23) + m_State[0];

    const UInt64 temporary = m_State[1] << 17;

    m_State[2] ^= m_State[0];
    m_State[3] ^= m_State[1];
    m_State[1] ^= m_State[2];
    m_State[0] ^= m_State[3];

    m_State[2] ^= temporary;
    m_State[3] = RotateLeft(m_State[3], 45);

    return result;
}

Int32 RandomGenerator::NextInt(Int32 minInclusive, Int32 maxInclusive)
{
    HP_ASSERT_ARGUMENT(minInclusive <= maxInclusive, "NextInt requires min <= max");
    if (minInclusive == maxInclusive)
    {
        return minInclusive;
    }

    // Multiply-and-shift range reduction: take the high 32 bits of a uniform 64-bit draw, multiply by
    // the range and keep the high bits again. Unlike `value % range` this does not amplify the bias of
    // the low bits — the residual bias is bounded by range/2^32, i.e. under one part in a million for
    // any range a game uses (Lemire's exact method removes it entirely at the cost of an extra branch).
    const UInt64 range =
        static_cast<UInt64>(static_cast<UInt64>(maxInclusive) - static_cast<UInt64>(minInclusive)) + 1ull;
    const UInt64 scaled = ((NextUInt64() >> 32) * range) >> 32;
    return static_cast<Int32>(static_cast<UInt64>(minInclusive) + scaled);
}

Float32 RandomGenerator::NextFloat01()
{
    // 24 bits divided by 2^24: exactly representable, so the result is uniform over the float grid in
    // [0,1) and never rounds up to 1.0.
    return static_cast<Float32>(NextUInt64() >> 40) * (1.0f / 16777216.0f);
}

Float32 RandomGenerator::NextFloat(Float32 minInclusive, Float32 maxExclusive)
{
    HP_ASSERT_ARGUMENT(minInclusive < maxExclusive, "NextFloat requires min < max");
    return minInclusive + (maxExclusive - minInclusive) * NextFloat01();
}

Float64 RandomGenerator::NextDouble01()
{
    // 53 bits over 2^53 uses the full double mantissa without bias.
    return static_cast<Float64>(NextUInt64() >> 11) * (1.0 / 9007199254740992.0);
}

Float32 RandomGenerator::NextNormal(Float32 mean, Float32 standardDeviation)
{
    // Box-Muller with a cached partner sample: two uniforms produce two normals, so half the draws
    // cost nothing beyond a state read.
    if (m_HasCachedNormal)
    {
        m_HasCachedNormal = false;
        return mean + standardDeviation * m_CachedNormal;
    }

    // logf(0) is -inf; NextFloat01 can return 0, so nudge into the open interval.
    Float32 u1 = NextFloat01();
    if (u1 <= 1e-7f)
    {
        u1 = 1e-7f;
    }
    const Float32 u2 = NextFloat01();

    const Float32 radius = std::sqrt(-2.0f * std::log(u1));
    const Float32 angle = kTwoPi * u2;

    m_CachedNormal = radius * std::sin(angle);
    m_HasCachedNormal = true;
    return mean + standardDeviation * (radius * std::cos(angle));
}

Vec3 RandomGenerator::NextUnitVector()
{
    // Uniform point on the sphere: z uniform in [-1,1], azimuth uniform. Rejection sampling would also
    // work but this branch-free form is cheaper and never loops.
    const Float32 z = NextFloatRange();
    const Float32 radiusPerpendicular = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const Float32 azimuth = kTwoPi * NextFloat01();

    return Vec3(radiusPerpendicular * std::cos(azimuth), radiusPerpendicular * std::sin(azimuth), z);
}

Vec3 RandomGenerator::NextPointInUnitCube()
{
    return Vec3(NextFloatRange(), NextFloatRange(), NextFloatRange());
}

Vec2 RandomGenerator::NextPointInUnitSquare()
{
    return Vec2(NextFloat01(), NextFloat01());
}

Quat RandomGenerator::NextQuaternion()
{
    // Shoemake's uniform distribution on SO(3). Using three uniform Euler angles instead would cluster
    // orientations near the poles and bias any "random rotation" gameplay.
    const Float32 u1 = NextFloat01();
    const Float32 u2 = NextFloat01();
    const Float32 u3 = NextFloat01();

    const Float32 squareRootOneMinusU1 = std::sqrt(1.0f - u1);
    const Float32 squareRootU1 = std::sqrt(u1);
    const Float32 angleOne = kTwoPi * u2;
    const Float32 angleTwo = kTwoPi * u3;

    return Quat(squareRootOneMinusU1 * std::sin(angleOne), squareRootU1 * std::sin(angleTwo),
                squareRootU1 * std::cos(angleTwo), squareRootOneMinusU1 * std::cos(angleOne))
        .Normalized();
}

bool RandomGenerator::NextBool(Float32 probability)
{
    return NextFloat01() < Saturate(probability);
}

Index RandomGenerator::NextIndex(Index count)
{
    HP_ASSERT_ARGUMENT(count > 0, "NextIndex requires a positive count");
    if (count == 1)
    {
        return 0;
    }
    const UInt64 range = static_cast<UInt64>(count);
    return static_cast<Index>(((NextUInt64() >> 32) * range) >> 32);
}

}  // namespace Hollowpoint::Math
