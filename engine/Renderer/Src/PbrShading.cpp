#include <algorithm>
#include <cmath>

#include "Hollowpoint/Renderer/PbrShading.h"
#include "Hollowpoint/Renderer/ShadingConstants.h"

namespace Hollowpoint::Renderer
{

using Math::Vec3;

namespace
{

using ShadingConstants::Epsilon;
using ShadingConstants::MinGgxAlpha;
using ShadingConstants::Pi;

// Local spellings kept so the maths below reads naturally. These are aliases of the shared constants, not copies: a
// literal here could drift from the GLSL generated from the same header, which is what makes CPU/GPU equivalence
// testable in the first place.
constexpr Float32 kPi = Pi;
constexpr Float32 kMinAlpha = MinGgxAlpha;
constexpr Float32 kEpsilon = Epsilon;

[[nodiscard]] constexpr Float32 Clamp01(Float32 value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] Vec3 Lerp(const Vec3& a, const Vec3& b, Float32 t) noexcept
{
    return a + (b - a) * t;
}

}  // namespace

Vec3 PbrDirectLightResult::Total() const noexcept
{
    return Diffuse + Specular;
}

Float32 PerceptualRoughnessToAlpha(Float32 perceptualRoughness) noexcept
{
    // Squared, not identity: authored roughness is perceptual, and GGX's response to alpha is not linear. Passing
    // roughness straight through makes mid-range materials read far too glossy (a classic "everything looks wet"
    // bug). Clamped before squaring so an out-of-range authoring value cannot produce a negative alpha.
    const Float32 clamped = Clamp01(perceptualRoughness);
    return std::max(clamped * clamped, kMinAlpha);
}

Float32 DistributionGGX(const Vec3& normal, const Vec3& viewDirection, const Vec3& halfVector, Float32 alpha) noexcept
{
    (void)viewDirection;  // Part of the BRDF signature for symmetry with the other terms; D depends only on N and H.

    const Float32 dotNormalHalf = std::max(normal.Dot(halfVector), 0.0f);
    const Float32 alphaSquared = alpha * alpha;

    // Trowbridge-Reitz: D = a^2 / (pi * ((n.h)^2 (a^2 - 1) + 1)^2).
    //
    // No divide-by-zero guard, deliberately. With cos^2 in [0,1] and alpha^2 > 0 the inner term lies in
    // [alpha^2, 1], so the denominator is bounded below by pi * alpha^4 and can never reach zero — the only way to
    // make it vanish is an alpha of zero, which PerceptualRoughnessToAlpha already prevents. An absolute-epsilon
    // guard here looked defensive and was a bug: at normal incidence with alpha = 0.01 the denominator is ~3e-8, so
    // comparing against 1e-7 returned 0 specular precisely where a smooth surface's highlight is strongest (found by
    // asserting that a smooth peak exceeds a rough one). Guards must be relative to the quantity's own range.
    const Float32 denominatorTerm = dotNormalHalf * dotNormalHalf * (alphaSquared - 1.0f) + 1.0f;
    const Float32 denominator = kPi * denominatorTerm * denominatorTerm;

    return alphaSquared / denominator;
}

Float32 GeometrySchlickGGX(Float32 dotNormalDirection, Float32 k) noexcept
{
    if (dotNormalDirection <= 0.0f)
    {
        // Back-facing to the direction being shadowed: no microfacets contribute along it.
        return 0.0f;
    }

    // Denominator is n.d(1-k) + k, a convex combination of two non-negative values with n.d > 0 guaranteed by the
    // check above — so it cannot vanish for any k >= 0 and needs no guard. (The direct-lighting k is at least 0.125;
    // callers passing arbitrary k are covered by k == 0 giving G = 1, which is the correct limit.)
    const Float32 denominator = dotNormalDirection * (1.0f - k) + k;

    return dotNormalDirection / denominator;
}

Float32 GeometrySmith(const Vec3& normal, const Vec3& viewDirection, const Vec3& lightDirection,
                      Float32 roughness) noexcept
{
    // Direct-lighting remap: k = (roughness + 1)^2 / 8. The IBL variant is roughness^2 / 2 — see the header note on
    // why they differ and why callers must not share one constant.
    const Float32 clampedRoughness = Clamp01(roughness);
    const Float32 k = (clampedRoughness + 1.0f) * (clampedRoughness + 1.0f) / 8.0f;

    const Float32 geometryView = GeometrySchlickGGX(normal.Dot(viewDirection), k);
    const Float32 geometryLight = GeometrySchlickGGX(normal.Dot(lightDirection), k);

    return geometryView * geometryLight;
}

Vec3 FresnelSchlick(Float32 cosineAngle, const Vec3& f0) noexcept
{
    // F = F0 + (1 - F0) * (1 - cos)^5. At normal incidence (cos == 1) this returns exactly F0; at grazing (cos == 0)
    // exactly 1 — both are asserted, since they pin the polynomial's endpoints independently of its shape.
    const Float32 oneMinusCosine = 1.0f - Clamp01(cosineAngle);
    const Float32 oneMinusCosineTo5th =
        oneMinusCosine * oneMinusCosine * oneMinusCosine * oneMinusCosine * oneMinusCosine;

    return f0 + (Vec3(1.0f, 1.0f, 1.0f) - f0) * oneMinusCosineTo5th;
}

Vec3 FresnelSchlickRoughness(Float32 cosineAngle, const Vec3& f0, Float32 roughness) noexcept
{
    // Roughness-aware grazing term: instead of driving F toward 1 at grazing angles, it saturates at
    // max(1 - roughness, F0). Without this, a rough surface mirrors sharply at grazing incidence — the visible "chrome
    // rim" artefact on matte materials. Per channel, because metallic tints F0 and the clamp must apply per colour.
    const Float32 clampedRoughness = Clamp01(roughness);
    const Float32 oneMinusCosine = 1.0f - Clamp01(cosineAngle);
    const Float32 oneMinusCosineTo5th =
        oneMinusCosine * oneMinusCosine * oneMinusCosine * oneMinusCosine * oneMinusCosine;

    const Float32 grazingFloor = std::max(1.0f - clampedRoughness, 0.0f);

    Vec3 result{};
    const Float32 f0Components[3] = {f0.X, f0.Y, f0.Z};
    Float32* outComponents[3] = {&result.X, &result.Y, &result.Z};

    for (int channel = 0; channel < 3; ++channel)
    {
        const Float32 grazingLimit = std::max(grazingFloor, f0Components[channel]);
        *outComponents[channel] = f0Components[channel] + (grazingLimit - f0Components[channel]) * oneMinusCosineTo5th;
    }

    return result;
}

PbrDirectLightResult EvaluateDirectLight(const PbrMaterialParameters& material, const Vec3& normalShading,
                                         const Vec3& viewDirection, const Vec3& lightDirection) noexcept
{
    PbrDirectLightResult result{};

    const Float32 dotNormalLight = normalShading.Dot(lightDirection);
    if (dotNormalLight <= 0.0f)
    {
        // Light behind the surface: no contribution, and returning early avoids a negative cosine leaking into the
        // geometric term (which would produce negative light that subtracts from the frame).
        return result;
    }

    const Vec3 halfVector = (viewDirection + lightDirection).Normalized();

    const Float32 dotNormalView = std::max(normalShading.Dot(viewDirection), kEpsilon);
    const Float32 dotViewHalf = std::max(viewDirection.Dot(halfVector), 0.0f);

    // F0 interpolates from the dielectric reflectance to the base colour: a metal's reflectance is tinted by its
    // albedo, a dielectric's is not. This is why metallic=1 moves BaseColor into the specular lobe and out of diffuse.
    const Vec3 f0 = Lerp(Vec3(material.DielectricF0, material.DielectricF0, material.DielectricF0), material.BaseColor,
                         Clamp01(material.Metallic));

    const Vec3 fresnel = FresnelSchlick(dotViewHalf, f0);
    const Float32 alpha = PerceptualRoughnessToAlpha(material.Roughness);

    const Float32 distribution = DistributionGGX(normalShading, viewDirection, halfVector, alpha);
    const Float32 geometry = GeometrySmith(normalShading, viewDirection, lightDirection, Clamp01(material.Roughness));

    // Cook-Torrance: D * G * F / (4 (n.v) (n.l)). The denominator is the Jacobian between the half-vector and surface
    // normal measures; omitting it makes specular energy scale with area instead of conserving it.
    const Float32 denominator = 4.0f * dotNormalView * dotNormalLight + kEpsilon;
    const Vec3 specular = (distribution * geometry) * fresnel / denominator;

    // Diffuse is Lambertian, weighted so the specular lobe's energy is not double-counted at grazing angles, and by
    // (1 - metallic) so a pure metal has exactly zero diffuse.
    const Float32 diffuseWeight = (1.0f - Clamp01(material.Metallic));
    const Vec3 one(1.0f, 1.0f, 1.0f);
    const Vec3 diffuseReflectance = diffuseWeight * (one - fresnel) * material.BaseColor / kPi;

    result.Diffuse = diffuseReflectance * dotNormalLight;
    result.Specular = specular * dotNormalLight;

    return result;
}

}  // namespace Hollowpoint::Renderer
