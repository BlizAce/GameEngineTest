#include <cmath>
#include <string>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/ShadingModelGlsl.h"

namespace Hollowpoint::Renderer
{

namespace
{

//! Product of the three basis-column lengths of a 3x3 matrix.
///
/// For a pure-scale matrix this equals |determinant| exactly, which is what makes their ratio a scale-free measure of
/// how well-formed the basis is: 1 for any uniform or anisotropic scale, falling toward 0 only as axes collapse or
/// become parallel. Used instead of a bare determinant so singularity detection carries no unit assumption.
[[nodiscard]] Float32 BasisColumnLengthProduct(const Mat3& matrix)
{
    const Vec3 columnX(matrix[0][0], matrix[0][1], matrix[0][2]);
    const Vec3 columnY(matrix[1][0], matrix[1][1], matrix[1][2]);
    const Vec3 columnZ(matrix[2][0], matrix[2][1], matrix[2][2]);

    return columnX.Length() * columnY.Length() * columnZ.Length();
}

//! Below this ratio of |determinant| to the basis-column product, the basis spans a plane rather than a volume: the
///  normal transform is meaningless and its entries would be dominated by float noise. One part per million of
///  well-formedness is far tighter than any real geometry needs and far looser than genuine collapse.
constexpr Float32 kDegenerateBasisRatio = 1.0e-6f;

//! The cofactor matrix of a column-major 3x3.
/**
 * Why the cofactor rather than inverse-transpose: they differ only by a scalar,
 *
 *     (M⁻¹)ᵀ = cofactor(M) / det(M),
 *
 * and this function's result is normalised before use — so the division by det can be skipped entirely. That is not
 * a micro-optimisation, it removes two scale traps an explicit inverse cannot avoid:
 *   - Mat3::Inverse rejects |det| <= kDefaultAbsoluteEpsilon, an absolute test on a quantity that grows as scale³, so
 *     legitimate small objects (scale 0.002 → det 8e-9) assert inside core math; and
 *   - forming 1/det then multiplying loses accuracy at extreme scales (measured: a 1e6-uniform transform came back
 *     with a wrong normal through the inverse path).
 * Cofactor entries are products of two basis components — O(scale²) — which stays comfortably in float range for any
 * scale an engine sees, and the final normalisation makes the remaining magnitude irrelevant.
 *
 * Construction: with M's columns c0,c1,c2, the three cross products r0 = c1×c2, r1 = c2×c0, r2 = c0×c1 satisfy
 * R·M = det(M)·I when placed as ROWS (each r is orthogonal to the two columns it excludes and its dot with the
 * remaining one is det). Hence M⁻¹ = R/det and (M⁻¹)ᵀ = Rᵀ/det. In column-major storage, Rᵀ's column c is the vector
 * r_c — so each cross product goes into one storage column unchanged.
 *
 * Worth stating because it defeats casual verification: on any symmetric basis (identity, axis-aligned scale) the
 * transposed placement of these rows produces the same matrix, so testing with unrotated geometry cannot tell the two
 * apart. The equivalence test therefore covers rotations and shear; re-transposing this assignment was tried as a
 * mutation and fails exactly that test.
 */
[[nodiscard]] Mat3 CofactorMatrix(const Mat3& m)
{
    const Vec3 c0(m[0][0], m[0][1], m[0][2]);
    const Vec3 c1(m[1][0], m[1][1], m[1][2]);
    const Vec3 c2(m[2][0], m[2][1], m[2][2]);

    const Vec3 r0 = c1.Cross(c2);
    const Vec3 r1 = c2.Cross(c0);
    const Vec3 r2 = c0.Cross(c1);

    Mat3 cofactor;
    cofactor[0][0] = r0.X;
    cofactor[0][1] = r0.Y;
    cofactor[0][2] = r0.Z;
    cofactor[1][0] = r1.X;
    cofactor[1][1] = r1.Y;
    cofactor[1][2] = r1.Z;
    cofactor[2][0] = r2.X;
    cofactor[2][1] = r2.Y;
    cofactor[2][2] = r2.Z;

    return cofactor;
}

//! Largest absolute entry, used to bring the cofactor matrix to O(1) before applying it.
[[nodiscard]] Float32 MaxAbsEntry(const Mat3& m)
{
    Float32 largest = 0.0f;
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            largest = std::max(largest, std::abs(m[column][row]));
        }
    }
    return largest;
}

//! Whether a 3x3 basis has a usable orientation to transform normals with.
/**
 * Judged SCALE-INVARIENTLY. An absolute epsilon on the determinant is wrong by construction: uniform scale s gives
 * det = s³, so a test like |det| <= 1e-8 asserts on any object below ~0.2 units — pebbles, screws and particles in a
 * metric-scale scene — while still missing collapsed geometry that merely happens to be large. The ratio used here is
 * exactly 1 for any scale matrix whatever its magnitude and drops only as axes collapse or turn parallel: orientation
 * quality, which is what normal transforms depend on, with no unit baked in.
 *
 * Comparisons are written so NaN reports as unusable (every comparison with NaN is false) — a NaN transform is a bug
 * upstream, and callers handle that explicitly rather than propagating NaN into shading. Same idiom Lights.cpp uses
 * for non-finite ranges.
 */
[[nodiscard]] bool HasUsableOrientation(const Mat3& basis)
{
    const Float32 columnProduct = BasisColumnLengthProduct(basis);
    const Float32 degeneracyRatio = columnProduct > 0.0f ? std::abs(basis.Determinant()) / columnProduct : 0.0f;

    return degeneracyRatio > kDegenerateBasisRatio;
}

}  // namespace

Mat3 ComputeNormalMatrix(const Mat3& modelBasis)
{
    if (!HasUsableOrientation(modelBasis))
    {
        HP_ASSERT_ARGUMENT(false, "model basis has no usable orientation (zero scale axis, collapsed basis, or NaN)");

        // Identity rather than an approximation: this function returns a matrix, so there is no partial direction to
        // salvage into one. Callers holding a normal should use ComputeWorldNormal, whose fallback keeps whatever
        // direction survives the collapse; an identity normal matrix at least lights geometry as untransformed
        // instead of producing NaN that spreads through every downstream dot product.
        return Mat3::Identity();
    }

    Mat3 normalMatrix = CofactorMatrix(modelBasis);

    // Bring to O(1) before use: cofactor entries are O(scale²), and a shader that normalises the varying (all of them
    // do — `normalize(InWorldNormal)`) would underflow for a very small object whose cofactor entries reach 1e-8.
    // Dividing by the largest entry is a positive scalar, so it cannot change any direction; it makes the whole path
    // magnitude-independent, which is the point of this function.
    const Float32 scale = MaxAbsEntry(normalMatrix);
    if (scale > 0.0f)
    {
        for (int column = 0; column < 3; ++column)
        {
            for (int row = 0; row < 3; ++row)
            {
                normalMatrix[column][row] /= scale;
            }
        }
    }

    return normalMatrix;
}

Vec3 ComputeWorldNormal(const Mat4& modelMatrix, const Vec3& localGeometricNormal)
{
    // Normal matrix = inverse transpose of the upper-left 3x3. Using the model matrix directly would be correct only
    // for uniform scale — and since every transform the engine generates today *is* uniformly scaled, that shortcut
    // would sit dormant until someone stretched a box, where it reads as "lighting looks wrong on scaled objects"
    // instead of naming this line. Doing it generally costs nine multiplications.
    const Mat3 upperLeft = modelMatrix.GetUpperLeft3();

    if (!HasUsableOrientation(upperLeft))
    {
        HP_ASSERT_ARGUMENT(false, "model matrix has no usable orientation (zero scale axis, collapsed basis, or NaN)");

        // Asserted, then handled: an assertion is a report, never a guard (AGENTS.md §4). A zero or near-coplanar basis
        // means the transform deleted a dimension — usually an author zeroed a scale axis. Rather than lose the
        // orientation entirely (what ComputeNormalMatrix must do), multiply by the basis and keep whatever direction
        // survives: for a collapsed-but-real surface that is still the right normal in the axes that did not collapse.
        //
        // The explicit finiteness test is load-bearing, not defensive noise: Vec3::Normalized's comment claims "never
        // NaN", but it computes length-squared and its guard only catches underflow — a NaN input gives length² = NaN,
        // which passes the `factor == 0` check and multiplies straight through (measured). Without this test a NaN
        // transform propagates into shading and tone mapping as a black screen that names nothing; returning zero here
        // means "unlit", a defined pixel with a diagnosable cause.
        const Vec3 salvaged = (upperLeft * localGeometricNormal).Normalized();

        if (!(std::isfinite(salvaged.X) && std::isfinite(salvaged.Y) && std::isfinite(salvaged.Z)))
        {
            return Vec3::Zero();
        }
        return salvaged;
    }

    // Result is normalised: the caller asked for a normal, and ComputeNormalMatrix's magnitude correction (a positive
    // scalar) would otherwise leave a non-unit vector here.
    return (ComputeNormalMatrix(upperLeft) * localGeometricNormal).Normalized();
}

Vec3 ComputeForwardShadingCpu(const ForwardPassParams& params, const ForwardDrawItem& draw, const Vec3& worldPosition,
                              const Vec3& localGeometricNormal)
{
    // Flat-face path: derive the world normal from the model matrix, then hand off. ComputeWorldNormal already
    // normalises and ShadeWorldPointCpu normalises again — as this function did before the split, so results are
    // bit-identical to the pre-refactor code (normalising a unit vector is not exactly a no-op in float, and the
    // equivalence tests compare against values produced here).
    return ShadeWorldPointCpu(params, draw, worldPosition, ComputeWorldNormal(draw.ModelMatrix, localGeometricNormal));
}

Vec3 ShadeWorldPointCpu(const ForwardPassParams& params, const ForwardDrawItem& draw, const Vec3& worldPosition,
                        const Vec3& worldNormal)
{
    // Normalised here rather than by the caller: this is the CPU mirror of the fragment shader's
    // `normalize(InWorldNormal)`, which normalises an INTERPOLATED vector. Putting it inside means both sides
    // normalise exactly once, at the same point in the pipeline — a caller that pre-normalised and this function that
    // re-normalised would differ by float rounding from a GPU that did one of the two.
    const Vec3 normalWorld = worldNormal.Normalized();

    // Toward the eye, matching EvaluateDirectLight's documented convention (viewDirection points surface→eye).
    const Vec3 toEye = params.CameraPosition - worldPosition;
    const Float32 eyeDistance = toEye.Length();
    if (eyeDistance <= 0.0f)
    {
        // Camera exactly on the surface: no well-defined view direction, and the BRDF would divide by a zero cosine.
        // Black is the honest answer — it is visibly "nothing here", unlike garbage which reads as a shading bug.
        return Vec3(0.0f);
    }
    const Vec3 viewDirection = toEye * (1.0f / eyeDistance);

    Vec3 accumulated{};

    for (const UInt32 lightIndex : draw.LightIndices)
    {
        if (lightIndex >= params.Lights.size())
        {
            // A dangling index is a wiring bug (scene edited under the renderer). Skipping keeps release builds
            // rendering rather than reading out of bounds; the assert makes it fatal in debug where it can be fixed.
            HP_ASSERT_ARGUMENT(false, "draw references a light index outside the frame's light list");
            continue;
        }

        const ForwardLight& entry = params.Lights[lightIndex];
        if (!entry.Light.Active)
        {
            continue;
        }

        Vec3 lightDirection{};  // surface → light, the BRDF's convention.
        Vec3 radiance{};

        switch (entry.Light.Type)
        {
            case LightComponent::LightType::Directional:
                // WorldDirection points *toward* what it lights; shading needs the opposite. Normalised because the
                // component stores direction unnormalised by contract.
                lightDirection = (-entry.WorldDirection).Normalized();
                radiance = EvaluateDirectionalLightRadiance(entry.Light);
                break;

            case LightComponent::LightType::Point:
            {
                const Vec3 toLight = entry.WorldPosition - worldPosition;
                const Float32 distance = toLight.Length();
                // At the light's own position there is no direction; returning viewDirection would shade as if lit from
                // behind the camera. Zero radiance instead — DistanceAttenuation clamps d^2 from below, so the value
                // stays finite and this branch just marks "coincident, nothing sensible to compute".
                if (distance <= 0.0f)
                {
                    continue;
                }
                lightDirection = toLight * (1.0f / distance);
                radiance = EvaluatePointLightRadiance(entry.Light, distance);
                break;
            }

            case LightComponent::LightType::Spot:
            {
                const Vec3 toLight = entry.WorldPosition - worldPosition;
                const Float32 distance = toLight.Length();
                if (distance <= 0.0f)
                {
                    continue;
                }
                lightDirection = toLight * (1.0f / distance);

                // The cone test compares the spot axis against the vector from light toward the surface, which is
                // -lightDirection. Identical definition on the GPU side (see BuildForwardFragmentGlsl): both sides must
                // agree or the equivalence test measures a convention difference rather than arithmetic.
                const Float32 dotViewSpotAxis = (-lightDirection).Dot(entry.WorldDirection.Normalized());
                radiance = EvaluateSpotLightRadiance(entry.Light, distance, dotViewSpotAxis);
                break;
            }
        }

        const PbrDirectLightResult brdf =
            EvaluateDirectLight(draw.Material, normalWorld, viewDirection, lightDirection);

        // radiance × (diffuse + specular). EvaluateDirectLight applied the geometric cosine exactly once — its
        // documented contract — so this product is the whole combination. A second cosine here (the classic
        // quarter-brightness error) fails the equivalence test immediately.
        accumulated = accumulated + radiance * brdf.Total();
    }

    return accumulated;
}

std::string BuildForwardVertexGlsl()
{
    // Attribute locations match GeometryVertex's pinned layout (position 0/12/24, stride 32). These are vertex-input
    // `location`s, unrelated to descriptor bindings — only uniform/sampler declarations go through HP_BINDING_*.
    //
    // Body-only text: no #version line. The caller assembles "#version 450\n" + BuildGlslBindingPrelude() + this,
    // the order GLSL requires (#version must precede every token, and the binding macros must exist before any block
    // uses them). Same convention as BuildShadingModelGlsl — the assembly point owns ordering once.
    std::string glsl;
    glsl += "layout(location = 0) in vec3 InPosition;\n";
    glsl += "layout(location = 1) in vec3 InNormal;\n";
    glsl += "layout(location = 2) in vec2 InUv;\n\n";

    glsl += "layout(location = 0) out vec3 OutWorldPosition;\n";
    glsl += "layout(location = 1) out vec3 OutWorldNormal;\n";
    glsl += "layout(location = 2) out vec2 OutUv;\n\n";

    // Per-frame matrices (slot 0), uploaded once per frame and shared by every draw — the same sharing that makes
    // light *indices* worth their indirection.
    glsl += "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform FrameParams {\n";
    glsl += "    mat4 ViewProjection;\n";
    glsl += "    vec4 CameraPositionAndPad;\n";
    glsl += "} frameParams;\n\n";

    glsl += "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(1)) uniform DrawTransform {\n";
    glsl += "    mat4 ModelMatrix;\n";
    glsl += "    mat4 NormalMatrix;  // inverse transpose of ModelMatrix's upper-left 3x3\n";
    glsl += "} drawTransform;\n\n";

    glsl += "void main()\n{\n";
    glsl += "    vec4 worldPosition = drawTransform.ModelMatrix * vec4(InPosition, 1.0);\n";
    glsl += "    OutWorldPosition = worldPosition.xyz;\n";
    // The CPU computes the inverse transpose (ComputeWorldNormal) and uploads it: doing mat3(inverse(...)) in GLSL
    // would duplicate that logic in a second language, and duplicating logic is what this generator exists to avoid.
    glsl += "    OutWorldNormal = mat3(drawTransform.NormalMatrix) * InNormal;\n";
    glsl += "    OutUv = InUv;\n";
    glsl += "    gl_Position = frameParams.ViewProjection * worldPosition;\n";
    glsl += "}\n";

    return glsl;
}

std::string BuildForwardFragmentGlsl(UInt32 maxLightsPerDraw)
{
    // A zero-length array is invalid GLSL and every draw would render black. Asserted (a missing configuration), and
    // clamped to 1 so even a misuse emits compilable GLSL rather than a confusing compile error far from here.
    HP_ASSERT_ARGUMENT(maxLightsPerDraw > 0u, "BuildForwardFragmentGlsl requires a positive light count");
    if (maxLightsPerDraw == 0u)
    {
        maxLightsPerDraw = 1;
    }

    const std::string count = std::to_string(maxLightsPerDraw);

    // Body-only text: see BuildForwardVertexGlsl for the assembly-order contract.
    std::string glsl;

    // The shared shading model, verbatim from its generator — the whole point of generating it. No lighting formula
    // appears in this file; everything below is data plumbing around Hp* calls.
    glsl += BuildShadingModelGlsl();
    glsl += "\n";

    glsl += "layout(location = 0) in vec3 InWorldPosition;\n";
    glsl += "layout(location = 1) in vec3 InWorldNormal;\n";
    glsl += "layout(location = 2) in vec2 InUv;\n\n";

    glsl += "layout(location = 0) out vec4 OutColor;\n\n";

    glsl += "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(0)) uniform FrameParams {\n";
    glsl += "    mat4 ViewProjection;\n";
    glsl += "    vec4 CameraPositionAndPad;\n";
    glsl += "} frameParams;\n\n";

    // Light array in structure-of-arrays form: four vec4s per light, all lights of one field contiguous. AoS would
    // let a shader read one whole struct per light, but SoA means the fields this loop actually needs (position,
    // colour) land in adjacent cache lines when the loop walks many lights — and it makes the C++ upload a straight
    // memcpy per field rather than an interleaved pack. Field order is pinned by ForwardRenderTests' layout asserts.
    glsl += "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(2)) uniform LightArray {\n";
    glsl += "    vec4 PositionAndIntensity[" + count + "];  // xyz position, w intensity\n";
    glsl += "    vec4 ColorTypeAndRange[" + count + "];     // rgb colour, w = type (0 dir, 1 point, 2 spot)\n";
    glsl += "    vec4 RangeAndSpotAngles[" + count + "];   // x range, y inner cone angle (rad), z outer (rad)\n";
    glsl += "    vec4 DirectionAndPad[" + count + "];      // xyz = light direction (dir/spot), w unused\n";
    glsl += "} lightArray;\n\n";

    glsl += "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(3)) uniform DrawMaterial {\n";
    glsl += "    vec4 BaseColor;\n";
    glsl += "    vec4 MaterialParams;  // x roughness, y metallic\n";
    glsl += "} drawMaterial;\n\n";

    // Light indices as a separate block: its size depends on maxLightsPerDraw while the material block does not, and
    // splitting them keeps the material layout stable for future per-material uniforms.
    glsl += "layout(set = 0, binding = HP_BINDING_CONSTANT_BUFFER(4)) uniform DrawLightIndices {\n";
    glsl += "    ivec4 LightIndices[" + std::to_string((maxLightsPerDraw + 3) / 4) + "];\n";
    glsl += "    int LightCount;\n";
    glsl += "} drawLightIndices;\n\n";

    glsl += "void main()\n{\n";
    glsl += "    vec3 normalShading = normalize(InWorldNormal);\n";
    glsl += "    vec3 viewDirection = normalize(frameParams.CameraPositionAndPad.xyz - InWorldPosition);\n\n";

    glsl += "    float roughness = clamp(drawMaterial.MaterialParams.x, 0.0, 1.0);\n";
    glsl += "    float metallic = clamp(drawMaterial.MaterialParams.y, 0.0, 1.0);\n\n";

    glsl += "    vec3 accumulated = vec3(0.0);\n";
    glsl += "    for (int i = 0; i < drawLightIndices.LightCount; ++i) {\n";
    glsl += "        int index = drawLightIndices.LightIndices[i / 4][i % 4];\n";
    // Out-of-range indices contribute nothing rather than reading undefined array elements: the C++ side asserts its
    // indices are valid, and an explicit guard makes both sides' behaviour defined and identical.
    glsl += "        if (index < 0 || index >= " + count + ") { continue; }\n";

    glsl += "        vec3 lightColor = lightArray.ColorTypeAndRange[index].rgb;\n";
    glsl += "        float intensity = lightArray.PositionAndIntensity[index].w;\n";
    glsl += "        float lightType = lightArray.ColorTypeAndRange[index].w;\n";
    glsl += "        float range = lightArray.RangeAndSpotAngles[index].x;\n\n";

    glsl += "        vec3 lightDirection;\n";
    glsl += "        vec3 radianceScale = vec3(1.0);\n";
    glsl += "        if (lightType < 0.5) {\n";
    // Directional: direction only, no distance term at all — a light at infinity has no parallax.
    glsl += "            lightDirection = normalize(-lightArray.DirectionAndPad[index].xyz);\n";
    glsl += "        } else {\n";
    glsl += "            vec3 toLight = lightArray.PositionAndIntensity[index].xyz - InWorldPosition;\n";
    glsl += "            float distance = length(toLight);\n";
    glsl += "            if (distance <= 0.0) { continue; }\n";
    glsl += "            lightDirection = toLight / distance;\n";
    glsl += "            radianceScale = radianceScale * HpDistanceAttenuation(distance, range);\n";
    glsl += "            if (lightType > 1.5) {\n";
    // Spot cone: same definition as the CPU reference — axis against the light→surface vector (-lightDirection).
    // Angles travel as cosines because that is what the attenuation compares; acos()ing them back here would round
    // twice for no benefit, so HpSpotAngleAttenuation receives cos(θ) via the identity form below.
    glsl += "                float spotCos = dot(-lightDirection, normalize(lightArray.DirectionAndPad[index].xyz));\n";
    // Angles travel in radians so this is HpSpotAngleAttenuation's own signature — the same call the CPU makes via
    // EvaluateSpotLightRadiance. Converting to cosines here and back would round twice for no benefit.
    glsl += "                radianceScale = radianceScale * HpSpotAngleAttenuation(spotCos,\n";
    glsl += "                    lightArray.RangeAndSpotAngles[index].y, lightArray.RangeAndSpotAngles[index].z);\n";
    glsl += "            }\n";
    glsl += "        }\n\n";

    glsl += "        vec3 diffuse; vec3 specular;\n";
    glsl += "        HpShadeDirect(drawMaterial.BaseColor.rgb, roughness, metallic,\n";
    glsl += "                      normalShading, viewDirection, lightDirection, diffuse, specular);\n";
    glsl += "        accumulated += lightColor * intensity * radianceScale * (diffuse + specular);\n";
    glsl += "    }\n\n";

    // Scene-linear HDR out, no tone map: the target is RGBA16_FLOAT and values above 1 are meaningful. Tone mapping
    // happens in the blit to display (ColorPipeline), so applying it here would double-apply once that pass lands.
    glsl += "    OutColor = vec4(accumulated, 1.0);\n";
    glsl += "}\n";

    return glsl;
}

}  // namespace Hollowpoint::Renderer
