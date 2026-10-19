#include <algorithm>
#include <cmath>

#include "Hollowpoint/Renderer/Lights.h"
#include "Hollowpoint/Renderer/ShadingConstants.h"

namespace Hollowpoint::Renderer
{

namespace
{

using ShadingConstants::MinDistanceSquared;
using ShadingConstants::Pi;

constexpr Float32 kPi = Pi;

// The inverse-square term is floored at MinDistanceSquared (see ShadingConstants.h for why: a surface point can
// coincide with a light, and infinite radiance spreads through bloom into a full-screen white flash). Aliased rather
// than redefined so the CPU reference and the GPU shader cannot disagree about where that floor is.
constexpr Float32 kMinDistanceSquared = MinDistanceSquared;

[[nodiscard]] Float32 SmoothStep01(Float32 edge0, Float32 edge1, Float32 x)
{
    if (edge1 <= edge0)
    {
        // Degenerate cone: no transition band. Treat everything past edge0 as inside rather than dividing by zero in
        // the normalisation below.
        return x >= edge1 ? 1.0f : 0.0f;
    }

    const Float32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);

    // Classic Hermite smoothstep: zero first derivative at both ends, which is what keeps the penumbra free of a
    // visible seam where it meets the fully-lit core and the unlit exterior.
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

Float32 DistanceAttenuation(Float32 distance, Float32 range) noexcept
{
    const Float32 d = std::fabs(distance);
    const Float32 clampedDistanceSquared = std::max(d * d, kMinDistanceSquared);

    if (range <= 0.0f || !std::isfinite(range))
    {
        // Unbounded: pure inverse square. Note this is the branch an unbounded light takes on purpose; a non-finite
        // range lands here too rather than poisoning the result with NaN.
        return 1.0f / clampedDistanceSquared;
    }

    const Float32 normalised = d / range;
    const Float32 normalisedTo4th = normalised * normalised * normalised * normalised;

    // Window is zero at and beyond the range, with a zero derivative there (d/dd of (1-x^4)^2 at x=1 is 0), so the
    // light's edge cannot show as a ring.
    const Float32 window = std::max(1.0f - normalisedTo4th, 0.0f);

    return window * window / clampedDistanceSquared;
}

Float32 SpotAngleAttenuation(Float32 dotViewSpotAxis, Float32 innerAngleRadians, Float32 outerAngleRadians) noexcept
{
    // Angles are full cone angles; the dot products compare against half-angle cosines.
    const Float32 safeInner = std::clamp(innerAngleRadians, 0.0f, kPi * 2.0f);
    const Float32 safeOuter = std::max(std::clamp(outerAngleRadians, 0.0f, kPi * 2.0f), safeInner);

    const Float32 innerCosine = std::cos(safeInner * 0.5f);
    const Float32 outerCosine = std::cos(safeOuter * 0.5f);

    // Cosine decreases as the angle grows, so the inside of the cone is the region with dot >= innerCosine and the
    // smoothstep runs from outerCosine (0) up to innerCosine (1).
    return SmoothStep01(outerCosine, innerCosine, dotViewSpotAxis);
}

Vec3 EvaluatePointLightRadiance(const LightComponent& light, Float32 distance) noexcept
{
    return light.Color * (light.Intensity * DistanceAttenuation(distance, light.Range));
}

Vec3 EvaluateSpotLightRadiance(const LightComponent& light, Float32 distance, Float32 dotViewSpotAxis) noexcept
{
    const Float32 angleFactor =
        SpotAngleAttenuation(dotViewSpotAxis, light.SpotInnerAngleRadians, light.SpotOuterAngleRadians);

    return EvaluatePointLightRadiance(light, distance) * angleFactor;
}

Vec3 EvaluateDirectionalLightRadiance(const LightComponent& light) noexcept
{
    // No distance term: a directional light models something at infinity (the sun), whose irradiance does not depend
    // on where the receiver is. Giving it a Range would be meaningless and is deliberately not offered.
    return light.Color * light.Intensity;
}

Float32 LuminousPowerToIntensity(Float32 luminousPowerLumens, LightComponent::LightType type,
                                 Float32 spotOuterAngleRadians) noexcept
{
    if (type != LightComponent::LightType::Spot)
    {
        // Point: radiates into the full sphere, so I = P / 4π.
        return luminousPowerLumens / (4.0f * kPi);
    }

    // Spot: concentrate the same power into the outer cone. Solid angle of a half-angle-θ cone is 2π(1 - cos θ).
    const Float32 halfAngle = std::clamp(spotOuterAngleRadians, 0.0f, kPi) * 0.5f;
    const Float32 solidAngle = 2.0f * kPi * (1.0f - std::cos(halfAngle));

    if (solidAngle <= 1e-6f)
    {
        // A zero-width cone would divide by zero and report infinite intensity. Clamping to the point-light value is
        // the least-wrong answer: it keeps the total power conserved instead of exploding.
        return luminousPowerLumens / (4.0f * kPi);
    }

    return luminousPowerLumens / solidAngle;
}

}  // namespace Hollowpoint::Renderer
