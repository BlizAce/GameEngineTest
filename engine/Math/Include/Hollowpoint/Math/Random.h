#pragma once

#include <array>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Hash.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Math/MathCore.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// RandomGenerator — deterministic, serialisable pseudo-random numbers.
//
// xoshiro256++ with a SplitMix64 seeding stream. Chosen because:
//   * it is small and fully auditable (no platform RNG in the loop),
//   * it has a 2^128 period and passes BigCrush,
//   * the state is 32 bytes, so a game can save/restore RNG state in a snapshot or replay log.
//
// Never use this for security purposes — it is a fast, reproducible generator, not a CSPRNG. For
// non-reproducible seeds use RandomGenerator::MakeNonReproducibleSeed().
// ---------------------------------------------------------------------------

class HP_MATH_API RandomGenerator
{
public:
    //! Deterministic default seed; useful when a test wants the same sequence every run.
    static constexpr UInt64 kDefaultSeed = 0x9E3779B97F4A7C15ull;

    explicit RandomGenerator(UInt64 seed = kDefaultSeed);

    //! Entropy from the platform RNG. Use for a session's master seed; never inside a sim step.
    [[nodiscard]] static UInt64 MakeNonReproducibleSeed();

    //! Derives an independent, well-separated stream from (seed, streamIndex). Two streams derived
    //! with different indices must not produce correlated sequences.
    [[nodiscard]] static RandomGenerator FromStream(UInt64 seed, UInt64 streamIndex);

    void SetSeed(UInt64 seed);

    //! Advances the stream by `count` draws without producing values. Used when a caller must keep two
    //! generators in lock-step, and by tests that skip ahead; [[nodiscard]] on the draw functions makes
    //! that pattern explicit instead of silently ignoring return values.
    void Discard(UInt64 count);

    //! Raw 64-bit state advance. Returns a value in [0, 2^64).
    [[nodiscard]] UInt64 NextUInt64();

    //! Uniform integer in [minInclusive, maxInclusive]. Rejected for an inverted range.
    [[nodiscard]] Int32 NextInt(Int32 minInclusive, Int32 maxInclusive);
    [[nodiscard]] Int32 NextInt(Int32 maxExclusive)
    {
        return NextInt(0, maxExclusive - 1);
    }

    //! Uniform float in [0,1). Uses the top 24 bits so that float granularity is used exactly.
    [[nodiscard]] Float32 NextFloat01();
    [[nodiscard]] Float32 NextFloat(Float32 minInclusive, Float32 maxExclusive);
    [[nodiscard]] Float32 NextFloatRange()
    {
        return NextFloat(-1.0f, 1.0f);
    }

    //! Uniform double in [0,1) with the full 53-bit mantissa.
    [[nodiscard]] Float64 NextDouble01();

    //! Normal (Gaussian) distributed value via Box-Muller; cached second sample keeps cost at ~1 cos/sqrt/log pair per
    //! two draws.
    [[nodiscard]] Float32 NextNormal(Float32 mean = 0.0f, Float32 standardDeviation = 1.0f);

    //! Uniformly random unit direction on the sphere / in the disc / in the cube.
    [[nodiscard]] Vec3 NextUnitVector();
    [[nodiscard]] Vec3 NextPointInUnitCube();
    [[nodiscard]] Vec2 NextPointInUnitSquare();

    //! Random rotation, uniformly distributed over SO(3) (Shoemake's method — not three uniform
    //! Euler angles, which cluster rotations near the poles).
    [[nodiscard]] Quat NextQuaternion();

    //! Returns true with probability `probability` (clamped to [0,1]).
    [[nodiscard]] bool NextBool(Float32 probability = 0.5f);

    //! Picks an index in [0, count). Asserts for a non-positive count.
    [[nodiscard]] Index NextIndex(Index count);

    [[nodiscard]] std::array<UInt64, 4> GetState() const
    {
        return m_State;
    }
    void SetState(const std::array<UInt64, 4>& state);

    bool operator==(const RandomGenerator& other) const = default;

private:
    std::array<UInt64, 4> m_State{};
    bool m_HasCachedNormal = false;
    Float32 m_CachedNormal = 0.0f;
};

}  // namespace Hollowpoint::Math
