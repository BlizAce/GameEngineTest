#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Math/MathCore.h"

// ---------------------------------------------------------------------------
// The single source of truth for every constant the shading model uses.
//
// Why these are not left inside their respective .cpp files: the engine validates its GPU shaders by comparing
// rendered pixels against the CPU reference (PbrShading, Lights, ColorPipeline). That comparison is only meaningful if
// both sides compute the *same function*. If a constant lives in a C++ file and a matching literal is typed into a GLSL
// string, they can drift — and the symptom is not an error but a small, plausible-looking difference that shows up as
// "the image looks slightly off" or fails an image test for reasons nobody can explain.
//
// So: constants are declared once here, and ShadingModelGlsl.h generates GLSL text with these values substituted (see
// BuildGgxBrdfGlsl). A shader generated this way cannot disagree with the C++ implementation about a number, only about
// arithmetic — which is exactly the class of difference the equivalence tests are designed to detect.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Renderer
{

using Math::Float32;

//! Mathematical and numerical constants shared by the CPU shading reference and the generated GLSL.
namespace ShadingConstants
{

//! Ratio of a circle's circumference to its diameter.
constexpr Float32 Pi = 3.14159265358979323846f;

//! Lower bound on GGX alpha (see PbrShading.cpp for why an exact zero is unusable).
constexpr Float32 MinGgxAlpha = 1e-3f;

//! Generic denominator guard. Only used where a divide-by-zero is genuinely reachable; where the maths bounds the
//! denominator away from zero, no guard is applied (see the notes in PbrShading.cpp — an absolute epsilon there was a
//! real bug, not a safety measure).
constexpr Float32 Epsilon = 1e-7f;

//! Lower bound on squared distance in point/spot attenuation: lights are treated as small emissive spheres (~1 cm)
//! rather than true points, which keeps a surface point coinciding with a light finite.
constexpr Float32 MinDistanceSquared = 1e-4f;

//! Reciprocal of 4π — the solid angle of a full sphere, used converting luminous power to intensity.
constexpr Float32 InvFourPi = 1.0f / (4.0f * Pi);

}  // namespace ShadingConstants

//! Coefficients of Narkowicz's rational approximation of the ACES output transform:
//! `f(x) = x(Ax + C) / (x(Bx + D) + E)` — note the pairing (A,C) in the numerator and (B,D,E) in the denominator, which
//! is easy to transpose by hand. Named individually so both the C++ implementation and the generated GLSL draw from one
//! definition.
struct AceFilmicCoefficients
{
    Float32 NumeratorLinear = 2.51f;      //!< A
    Float32 NumeratorConstant = 0.03f;    //!< C
    Float32 DenominatorLinear = 2.43f;    //!< B
    Float32 DenominatorMiddle = 0.59f;    //!< D
    Float32 DenominatorConstant = 0.14f;  //!< E

    //! Input at which the curve reaches 1.0 and beyond which output is clamped. Derived, not authored: it is where the
    //! numerator and denominator polynomials cross. Clamping there is what keeps absurd HDR values (an unnormalised
    //! HDRI, a bug upstream) from overflowing float32 inside the rational form and producing NaN — see
    //! ColorPipeline.cpp.
    Float32 SaturationPoint = 8.0f;

    //! Depth of the toe's linear region: as x -> 0 the curve is (C/E)x. Exposed because it is the number people reason
    //! about when judging how much a tone map darkens shadows.
    [[nodiscard]] constexpr Float32 ToeSlope() const noexcept
    {
        return NumeratorConstant / DenominatorConstant;
    }
};

//! The engine's ACES filmic parameters.
constexpr AceFilmicCoefficients AceFilmic{};

}  // namespace Hollowpoint::Renderer
