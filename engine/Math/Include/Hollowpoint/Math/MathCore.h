#pragma once

#include <cmath>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Math
{

// The engine's fixed-width integer aliases live in Core; re-exporting them here lets math code write
// `UInt32` without a namespace qualifier on every line (and keeps the public headers free of macros).
using Core::HashValue;
using Core::Index;
using Core::Int16;
using Core::Int32;
using Core::Int64;
using Core::Int8;
using Core::Size;
using Core::UInt16;
using Core::UInt32;
using Core::UInt64;
using Core::UInt8;

// ---------------------------------------------------------------------------
// Scalar types and constants.
//
// The engine computes in float (Float32) almost everywhere; double is reserved for time and for
// world-space positions in large open worlds (see DVec3). Angles are always radians.
// ---------------------------------------------------------------------------

using Float32 = float;
using Float64 = double;
using Angle = Float32;  //!< Radians. Never store degrees in engine types.

inline constexpr Float32 kPi = 3.14159265358979323846f;
inline constexpr Float32 kTwoPi = 6.28318530717958647692f;
inline constexpr Float32 kHalfPi = 1.57079632679489661923f;
inline constexpr Float32 kEulerGamma = 0.5772156649015329f;
inline constexpr Float32 kFloatEpsilon = std::numeric_limits<Float32>::epsilon();

// Default tolerances for ApproximatelyEqual — see docs/MODULE_MATH.md for the policy.
inline constexpr Float32 kDefaultAbsoluteEpsilon = 1e-6f;
inline constexpr Float32 kDefaultRelativeEpsilon = 1e-5f;

// ---------------------------------------------------------------------------
// Vector types.
//
// These are thin wrappers over glm so that (a) public engine headers do not expose glm and (b) we can
// attach the constructors and helpers gameplay code actually needs. They remain trivially copyable
// and layout-compatible with the corresponding glm types, which are 16-byte aligned for Vec3/Vec4.
// ---------------------------------------------------------------------------

class Vec2;
class Vec3;
class Vec4;
class Quat;

class HP_MATH_API alignas(8) Vec2
{
public:
    Float32 X = 0.0f;
    Float32 Y = 0.0f;

    Vec2() = default;
    constexpr Vec2(Float32 x, Float32 y) : X(x), Y(y) {}
    explicit constexpr Vec2(Float32 scalar) : X(scalar), Y(scalar) {}

    static constexpr Vec2 Zero()
    {
        return Vec2(0.0f, 0.0f);
    }
    static constexpr Vec2 One()
    {
        return Vec2(1.0f, 1.0f);
    }
    static constexpr Vec2 UnitX()
    {
        return Vec2(1.0f, 0.0f);
    }
    static constexpr Vec2 UnitY()
    {
        return Vec2(0.0f, 1.0f);
    }

    // Component access by index (0 = X). Kept as an explicit switch so the class stays standard C++
    // (no anonymous structs or type punning) while remaining layout-compatible with glm::vec2.
    [[nodiscard]] Float32& operator[](std::size_t index)
    {
        return index == 0 ? X : Y;
    }
    [[nodiscard]] const Float32& operator[](std::size_t index) const
    {
        return index == 0 ? X : Y;
    }

    Vec2 operator+(Vec2 other) const
    {
        return Vec2(X + other.X, Y + other.Y);
    }
    Vec2 operator-(Vec2 other) const
    {
        return Vec2(X - other.X, Y - other.Y);
    }
    Vec2 operator*(Float32 scalar) const
    {
        return Vec2(X * scalar, Y * scalar);
    }
    Vec2 operator*(Vec2 other) const
    {
        return Vec2(X * other.X, Y * other.Y);
    }
    Vec2 operator/(Float32 scalar) const
    {
        return Vec2(X / scalar, Y / scalar);
    }
    Vec2 operator-() const
    {
        return Vec2(-X, -Y);
    }

    Vec2& operator+=(Vec2 other)
    {
        X += other.X;
        Y += other.Y;
        return *this;
    }
    Vec2& operator-=(Vec2 other)
    {
        X -= other.X;
        Y -= other.Y;
        return *this;
    }
    Vec2& operator*=(Float32 scalar)
    {
        X *= scalar;
        Y *= scalar;
        return *this;
    }
    Vec2& operator/=(Float32 scalar)
    {
        X /= scalar;
        Y /= scalar;
        return *this;
    }

    bool operator==(const Vec2& other) const = default;
    bool operator!=(const Vec2& other) const = default;

    [[nodiscard]] Float32 Length() const
    {
        return std::sqrt(X * X + Y * Y);
    }
    [[nodiscard]] Float32 LengthSquared() const
    {
        return X * X + Y * Y;
    }
    [[nodiscard]] Vec2 Normalized() const;
    [[nodiscard]] Float32 Dot(Vec2 other) const
    {
        return X * other.X + Y * other.Y;
    }
    [[nodiscard]] Float32 Distance(Vec2 other) const
    {
        return (*this - other).Length();
    }
};

inline Vec2 operator*(Float32 scalar, Vec2 vector)
{
    return vector * scalar;
}

class HP_MATH_API alignas(16) Vec3
{
public:
    Float32 X = 0.0f;
    Float32 Y = 0.0f;
    Float32 Z = 0.0f;

    Vec3() = default;
    constexpr Vec3(Float32 x, Float32 y, Float32 z) : X(x), Y(y), Z(z) {}
    explicit constexpr Vec3(Float32 scalar) : X(scalar), Y(scalar), Z(scalar) {}
    Vec3(Vec2 xy, Float32 z) : X(xy.X), Y(xy.Y), Z(z) {}

    static constexpr Vec3 Zero()
    {
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    static constexpr Vec3 One()
    {
        return Vec3(1.0f, 1.0f, 1.0f);
    }
    static constexpr Vec3 UnitX()
    {
        return Vec3(1.0f, 0.0f, 0.0f);
    }
    static constexpr Vec3 UnitY()
    {
        return Vec3(0.0f, 1.0f, 0.0f);
    }
    static constexpr Vec3 UnitZ()
    {
        return Vec3(0.0f, 0.0f, 1.0f);
    }
    static constexpr Vec3 Up()
    {
        return Vec3(0.0f, 1.0f, 0.0f);
    }
    static constexpr Vec3 Right()
    {
        return Vec3(1.0f, 0.0f, 0.0f);
    }
    //! Camera/world forward direction: right-handed coordinate system looks down -Z.
    static constexpr Vec3 Forward()
    {
        return Vec3(0.0f, 0.0f, -1.0f);
    }

    [[nodiscard]] Float32& operator[](std::size_t index);
    [[nodiscard]] const Float32& operator[](std::size_t index) const;

    Vec3 operator+(Vec3 other) const
    {
        return Vec3(X + other.X, Y + other.Y, Z + other.Z);
    }
    Vec3 operator-(Vec3 other) const
    {
        return Vec3(X - other.X, Y - other.Y, Z - other.Z);
    }
    Vec3 operator*(Float32 scalar) const
    {
        return Vec3(X * scalar, Y * scalar, Z * scalar);
    }
    Vec3 operator*(Vec3 other) const
    {
        return Vec3(X * other.X, Y * other.Y, Z * other.Z);
    }
    Vec3 operator/(Float32 scalar) const
    {
        return Vec3(X / scalar, Y / scalar, Z / scalar);
    }
    Vec3 operator-() const
    {
        return Vec3(-X, -Y, -Z);
    }

    Vec3& operator+=(Vec3 other)
    {
        X += other.X;
        Y += other.Y;
        Z += other.Z;
        return *this;
    }
    Vec3& operator-=(Vec3 other)
    {
        X -= other.X;
        Y -= other.Y;
        Z -= other.Z;
        return *this;
    }
    Vec3& operator*=(Float32 scalar)
    {
        X *= scalar;
        Y *= scalar;
        Z *= scalar;
        return *this;
    }
    Vec3& operator/=(Float32 scalar)
    {
        X /= scalar;
        Y /= scalar;
        Z /= scalar;
        return *this;
    }

    bool operator==(const Vec3& other) const = default;
    bool operator!=(const Vec3& other) const = default;

    [[nodiscard]] Float32 Length() const
    {
        return std::sqrt(X * X + Y * Y + Z * Z);
    }
    [[nodiscard]] Float32 LengthSquared() const
    {
        return X * X + Y * Y + Z * Z;
    }

    //! Returns a unit-length vector, or Zero() when the length is below the epsilon (never NaN).
    [[nodiscard]] Vec3 Normalized(Float32 epsilon = kDefaultAbsoluteEpsilon) const;

    [[nodiscard]] Float32 Dot(Vec3 other) const
    {
        return X * other.X + Y * other.Y + Z * other.Z;
    }
    [[nodiscard]] Vec3 Cross(Vec3 other) const
    {
        return Vec3(Y * other.Z - Z * other.Y, Z * other.X - X * other.Z, X * other.Y - Y * other.X);
    }
    [[nodiscard]] Float32 Distance(Vec3 other) const
    {
        return (*this - other).Length();
    }

    [[nodiscard]] Vec2 XY() const
    {
        return Vec2(X, Y);
    }
    [[nodiscard]] Vec2 XZ() const
    {
        return Vec2(X, Z);
    }
    [[nodiscard]] Vec2 YZ() const
    {
        return Vec2(Y, Z);
    }

    [[nodiscard]] Vec3 Min(Vec3 other) const
    {
        return Vec3(X < other.X ? X : other.X, Y < other.Y ? Y : other.Y, Z < other.Z ? Z : other.Z);
    }
    [[nodiscard]] Vec3 Max(Vec3 other) const
    {
        return Vec3(X > other.X ? X : other.X, Y > other.Y ? Y : other.Y, Z > other.Z ? Z : other.Z);
    }
    [[nodiscard]] Vec3 Abs() const
    {
        return Vec3(std::fabs(X), std::fabs(Y), std::fabs(Z));
    }
};

inline Vec3 operator*(Float32 scalar, Vec3 vector)
{
    return vector * scalar;
}

class HP_MATH_API alignas(16) Vec4
{
public:
    Float32 X = 0.0f;
    Float32 Y = 0.0f;
    Float32 Z = 0.0f;
    Float32 W = 0.0f;

    Vec4() = default;
    constexpr Vec4(Float32 x, Float32 y, Float32 z, Float32 w) : X(x), Y(y), Z(z), W(w) {}
    explicit constexpr Vec4(Float32 scalar) : X(scalar), Y(scalar), Z(scalar), W(scalar) {}
    Vec4(Vec2 xy, Float32 z, Float32 w) : X(xy.X), Y(xy.Y), Z(z), W(w) {}
    Vec4(Vec3 xyz, Float32 w) : X(xyz.X), Y(xyz.Y), Z(xyz.Z), W(w) {}

    static constexpr Vec4 Zero()
    {
        return Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    static constexpr Vec4 One()
    {
        return Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    [[nodiscard]] Float32& operator[](std::size_t index);
    [[nodiscard]] const Float32& operator[](std::size_t index) const;

    Vec4 operator+(Vec4 other) const
    {
        return Vec4(X + other.X, Y + other.Y, Z + other.Z, W + other.W);
    }
    Vec4 operator-(Vec4 other) const
    {
        return Vec4(X - other.X, Y - other.Y, Z - other.Z, W - other.W);
    }
    Vec4 operator*(Float32 scalar) const
    {
        return Vec4(X * scalar, Y * scalar, Z * scalar, W * scalar);
    }
    Vec4 operator*(Vec4 other) const
    {
        return Vec4(X * other.X, Y * other.Y, Z * other.Z, W * other.W);
    }
    Vec4 operator-() const
    {
        return Vec4(-X, -Y, -Z, -W);
    }

    bool operator==(const Vec4& other) const = default;
    bool operator!=(const Vec4& other) const = default;

    [[nodiscard]] Float32 Length() const
    {
        return std::sqrt(X * X + Y * Y + Z * Z + W * W);
    }
    [[nodiscard]] Float32 LengthSquared() const
    {
        return X * X + Y * Y + Z * Z + W * W;
    }
    [[nodiscard]] Vec4 Normalized(Float32 epsilon = kDefaultAbsoluteEpsilon) const;
    [[nodiscard]] Float32 Dot(Vec4 other) const
    {
        return X * other.X + Y * other.Y + Z * other.Z + W * other.W;
    }

    [[nodiscard]] Vec3 XYZ() const
    {
        return Vec3(X, Y, Z);
    }
};

inline Vec4 operator*(Float32 scalar, Vec4 vector)
{
    return vector * scalar;
}

// Integer and unsigned vectors (no alignment requirement; used for counts, formats, indices).
struct HP_MATH_API IVec2
{
    Int32 X = 0, Y = 0;
    constexpr IVec2() = default;
    constexpr IVec2(Int32 x, Int32 y) : X(x), Y(y) {}
    bool operator==(const IVec2&) const = default;
};

struct HP_MATH_API IVec3
{
    Int32 X = 0, Y = 0, Z = 0;
    constexpr IVec3() = default;
    constexpr IVec3(Int32 x, Int32 y, Int32 z) : X(x), Y(y), Z(z) {}
    bool operator==(const IVec3&) const = default;
};

struct HP_MATH_API UVec2
{
    UInt32 X = 0, Y = 0;
    constexpr UVec2() = default;
    constexpr UVec2(UInt32 x, UInt32 y) : X(x), Y(y) {}
    bool operator==(const UVec2&) const = default;
};

struct HP_MATH_API UVec3
{
    UInt32 X = 0, Y = 0, Z = 0;
    constexpr UVec3() = default;
    constexpr UVec3(UInt32 x, UInt32 y, UInt32 z) : X(x), Y(y), Z(z) {}
    bool operator==(const UVec3&) const = default;
};

// ---------------------------------------------------------------------------
// Mat3 — 3x3 matrix (normals, 2D transforms, inertia tensors).
// ---------------------------------------------------------------------------

class HP_MATH_API alignas(16) Mat3
{
public:
    // Column-major storage: Columns[column][row]. Layout matches glm::mat3 and GLSL `mat3`.
    Float32 Columns[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    Mat3() = default;
    explicit Mat3(Float32 diagonal);
    Mat3(Float32 c0r0, Float32 c0r1, Float32 c0r2, Float32 c1r0, Float32 c1r1, Float32 c1r2, Float32 c2r0, Float32 c2r1,
         Float32 c2r2);

    static Mat3 Identity();
    static Mat3 FromRotation(Float32 angleRadians);  //!< Rotation about +Z (2D rotation).
    static Mat3 FromScaling(Vec2 scale);
    static Mat3 FromBasisVectors(Vec3 xAxis, Vec3 yAxis, Vec3 zAxis);

    [[nodiscard]] Float32* operator[](std::size_t column)
    {
        return Columns[column];
    }
    [[nodiscard]] const Float32* operator[](std::size_t column) const
    {
        return Columns[column];
    }

    Mat3 operator*(const Mat3& other) const;
    Vec3 operator*(Vec3 vector) const;
    bool operator==(const Mat3&) const = default;

    [[nodiscard]] Mat3 Transposed() const;
    [[nodiscard]] Float32 Determinant() const;
    [[nodiscard]] Mat3 Inverse() const;          //!< Identity (and asserted) for a singular matrix.
    [[nodiscard]] Mat3 AbsDeterminant1() const;  //!< Sign-corrected scaling used to renormalise normals.
};

// ---------------------------------------------------------------------------
// Mat4 — 4x4 transform/projection matrix. Column-major, post-multiplied, column vectors:
//   transformed = M * v      and      worldFromLocal = parentWorld * localTransform
// ---------------------------------------------------------------------------

class HP_MATH_API alignas(16) Mat4
{
public:
    Float32 Columns[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    Mat4() = default;
    explicit Mat4(Float32 diagonal);
    explicit Mat4(const Mat3& upperLeft);

    static Mat4 Identity();

    // Primitive transforms --------------------------------------------------
    static Mat4 Translate(Vec3 translation);
    static Mat4 Scale(Vec3 scale);
    static Mat4 FromQuaternion(Quat rotation);
    static Mat4 Rotate(Vec3 axis, Float32 angleRadians);   //!< Right-handed rotation about `axis`.
    static Mat4 LookAtLH(Vec3 eye, Vec3 target, Vec3 up);  //!< Unused by the renderer; kept for tools.
    static Mat4 LookAtRH(Vec3 eye, Vec3 target, Vec3 up);

    // Projection ------------------------------------------------------------
    //! Right-handed view matrix (camera looks down -Z). This is what the renderer uses.
    static Mat4 PerspectiveRH(Float32 verticalFieldOfViewRadians, Float32 aspectRatio, Float32 nearPlane,
                              Float32 farPlane);
    //! Vulkan clip space: X/Y in [-1,1], Z in [0,1]. Right-handed.
    static Mat4 OrthographicRH(Float32 width, Float32 height, Float32 nearPlane, Float32 farPlane);

    [[nodiscard]] Float32* operator[](std::size_t column)
    {
        return Columns[column];
    }
    [[nodiscard]] const Float32* operator[](std::size_t column) const
    {
        return Columns[column];
    }

    Mat4 operator*(const Mat4& other) const;
    Vec4 operator*(Vec4 vector) const;
    Vec3 TransformPoint(Vec3 point) const;  //!< Applies perspective division (assumes w != 0).
    Vec3 TransformDirection(Vec3 direction) const
    {
        return TransformVector(direction);
    }
    Vec3 TransformVector(Vec3 vector) const;  //!< Rotation/scale only, ignores translation.
    bool operator==(const Mat4&) const = default;

    [[nodiscard]] Mat4 Transposed() const;
    [[nodiscard]] Float32 Determinant() const;
    [[nodiscard]] Mat4 Inverse() const;  //!< Identity (and asserted) for a singular matrix.

    [[nodiscard]] Vec3 GetTranslation() const;
    [[nodiscard]] Mat3 GetUpperLeft3() const;
    [[nodiscard]] Mat3 GetRotationMatrix() const;  //!< Rotation with scale removed.
    //! Lengths of the three basis columns, with a negative sign reported on X when the matrix mirrors
    //! (negative determinant). Two mirror axes give a positive determinant and read as all-positive:
    //! that ambiguity is inherent to extracting a signed scale from a single matrix, so use
    //! Transform::Decompose when the exact original signs must survive.
    [[nodiscard]] Vec3 GetScale() const;

    //! Row extraction, used by the frustum-plane derivation.
    [[nodiscard]] Vec4 GetRow(std::size_t rowIndex) const;
};

// ---------------------------------------------------------------------------
// Quat — unit quaternion for rotations. Components are stored (x, y, z, w) with w last to match glm,
// glTF and the DCC tools our artists use. Multiplication order: q = qParent * qChild applies qChild
// first.
// ---------------------------------------------------------------------------

class HP_MATH_API alignas(16) Quat
{
public:
    Float32 X = 0.0f, Y = 0.0f, Z = 0.0f, W = 1.0f;

    constexpr Quat() = default;
    constexpr Quat(Float32 x, Float32 y, Float32 z, Float32 w) : X(x), Y(y), Z(z), W(w) {}

    static constexpr Quat Identity()
    {
        return Quat(0.0f, 0.0f, 0.0f, 1.0f);
    }

    //! Right-handed rotation of `angleRadians` about a (not necessarily normalised) axis.
    static Quat FromAxisAngle(Vec3 axis, Float32 angleRadians);
    static Quat LookRotation(Vec3 forward, Vec3 up = Vec3::Up());
    //! Shortest-arc rotation from one unit direction to another.
    static Quat FromTo(Vec3 from, Vec3 to);

    [[nodiscard]] Float32 Length() const
    {
        return std::sqrt(X * X + Y * Y + Z * Z + W * W);
    }
    [[nodiscard]] Float32 LengthSquared() const
    {
        return X * X + Y * Y + Z * Z + W * W;
    }
    [[nodiscard]] Quat Normalized() const;  //!< Identity for a near-zero quaternion.
    [[nodiscard]] Quat Conjugated() const
    {
        return Quat(-X, -Y, -Z, W);
    }
    [[nodiscard]] Quat Inverse() const;  //!< Conjugate divided by squared length.

    Quat operator*(const Quat& other) const;
    Vec3 operator*(Vec3 vector) const;  //!< Rotates `vector`.
    bool operator==(const Quat&) const = default;

    //! Named alias for the rotation operator, used where `q * v` would read ambiguously.
    [[nodiscard]] Vec3 MapVector(Vec3 vector) const
    {
        return (*this) * vector;
    }

    [[nodiscard]] Float32 Dot(const Quat& other) const
    {
        return X * other.X + Y * other.Y + Z * other.Z + W * other.W;
    }

    //! Spherical linear interpolation taking the short path (never rotates the long way round).
    [[nodiscard]] static Quat Slerp(Quat from, Quat to, Float32 t);
    [[nodiscard]] static Quat NLerp(Quat from, Quat to, Float32 t);  //!< Cheap, good enough per frame.

    //! Extracts a right-handed rotation about the given world axis, in [-pi, pi].
    [[nodiscard]] Float32 GetAngle() const;
    [[nodiscard]] Vec3 GetAxis() const;
};

// ---------------------------------------------------------------------------
// Basis — the six axis-aligned directions, mostly for gizmos and culling code.
// ---------------------------------------------------------------------------

enum class Axis : UInt8
{
    X = 0,
    Y = 1,
    Z = 2
};

enum class AxisDirection : UInt8
{
    NegativeX = 0,
    PositiveX,
    NegativeY,
    PositiveY,
    NegativeZ,
    PositiveZ
};

[[nodiscard]] HP_MATH_API Vec3 ToVector(AxisDirection direction);
[[nodiscard]] HP_MATH_API Axis GetAxis(AxisDirection direction);
[[nodiscard]] HP_MATH_API bool IsPositive(AxisDirection direction);

}  // namespace Hollowpoint::Math
