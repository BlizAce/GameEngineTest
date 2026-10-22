#include <cmath>
#include <string>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Renderer/ForwardRender.h"
#include "Hollowpoint/Renderer/ShadingModelGlsl.h"

namespace Hollowpoint::Renderer
{

Vec3 ComputeWorldNormal(const Mat4& modelMatrix, const Vec3& localGeometricNormal)
{
    // Inverse transpose of the upper-left 3x3. Using the model matrix directly would be correct only for uniform
    // scale — and since every transform the engine generates today *is* uniform-scaled, that bug would sit dormant
    // until someone stretched a box, where it reads as "lighting looks wrong on scaled objects" instead of naming
    // this line. Doing it generally costs one matrix inverse.
    const Mat3 upperLeft = modelMatrix.GetUpperLeft3();

    // Mat3::Inverse asserts and returns identity for a singular matrix (documented). Asserting the determinant here
    // too makes the caller's mistake explicit: a zero-scale axis means there is no surface to light, and silently
    // shading with an identity normal would draw plausible-looking garbage.
    const Float32 determinant = upperLeft.Determinant();
    if (std::abs(determinant) <= 1.0e-8f)
    {
        // Asserted, then handled: an assertion is a report, never a guard (AGENTS.md §4). The transform has no
        // invertible orientation — usually an author zeroed a scale axis, which deletes the surface. Transforming
        // the normal by the matrix directly keeps its direction (lighting that face as if unscaled) instead of
        // Mat3::Inverse's identity fallback or a NaN, both of which read as "shading is broken" rather than naming
        // the zero scale.
        HP_ASSERT_ARGUMENT(std::abs(determinant) > 1.0e-8f,
                           "model matrix has no invertible surface orientation (zero scale axis?)");
        return (upperLeft * localGeometricNormal).Normalized();
    }

    return upperLeft.Inverse().Transposed() * localGeometricNormal;
}

Vec3 ComputeForwardShadingCpu(const ForwardPassParams& params, const ForwardDrawItem& draw, const Vec3& worldPosition,
                              const Vec3& localGeometricNormal)
{
    const Vec3 normalWorld = ComputeWorldNormal(draw.ModelMatrix, localGeometricNormal).Normalized();

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
