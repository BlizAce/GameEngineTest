# Hollowpoint — Math module.

Self-contained, dependency-free (on std + Core) 3D math library with documented conventions.

**Why not glm?** The brief lists glm, and it is fetched and available (`Hollowpoint::ThirdParty`). We
implemented the layer ourselves instead, for two concrete reasons:
1. **Header cost and coupling.** glm's headers are pulled into every translation unit that includes any
   engine header using a vector; keeping it out of public headers keeps compile times flat as the engine
   grows. Our types are plain `float` members with no template machinery.
2. **Semantics we own.** Column-major + Vulkan clip space (Z in [0,1]) + right-handed + our epsilon
   policy has to be exactly consistent everywhere. With glm that lives in per-file macro/incantation
   choices; here it is one implementation with tests pinning each convention.

glm stays available to other modules (the renderer may use it directly inside `.cpp` files where a
specific glm helper is a good fit); it is never required by, or exposed from, `Hollowpoint::Math`.

## Conventions (all enforced by tests in `tests/Math/`)

| Topic        | Choice                                              | Rationale                       |
|--------------|-----------------------------------------------------|---------------------------------|
| Vectors      | `Vec2`, `Vec3`, `Vec4` (float); `DVec*` doubles      | 16-byte aligned, POD            |
| Matrices     | `Mat3`, `Mat4`, **column-major**, post-multiply      | matches GLSL/HLSL and glm       |
| Handedness   | **right-handed**, +X right, +Y up, **-Z forward**    | Vulkan/glTF convention          |
| Clip space   | X/Y in [-1,1], **Z in [0,1]**                        | Vulkan depth range              |
| Angles       | radians everywhere in the engine API                 | avoids degree/radian bugs       |
| Quaternions  | `Quat(x, y, z, w)`, `w` last, right-handed           | matches glm and most DCC tools  |
| Units        | metres, seconds, kilograms                           | SI                              |

Composition order for a transform is `T * R * S` (scale first, then rotate, then translate):

```cpp
const Mat4 worldMatrix = Mat4::Translate(translation) * Mat4::FromQuaternion(rotation) *
                         Mat4::Scale(scale);
```

Column-vector convention: a point is transformed by `matrix * point`.

## Types

- `MathCore.h` — scalar aliases (`Float32`, `Angle`), constants (`kPi`, `kDefaultAbsoluteEpsilon`), `Vec2/3/4`,
  `Ivec2/3`, `Uvec2/3`, `Mat3`, `Mat4`, `Quat`, plus `Axis`/`AxisDirection` helpers.
- `MathFunctions.h` — `Lerp`, `InverseLerp`, `SmoothStep`, `Clamp`, `Saturate`, `Radians`, `Degrees`,
  `ApproximatelyEqual` (with a documented epsilon policy), `WrapAngle`, `PingPong`, `MoveTowards`.
- `Transform.h` — `Transform` (position/rotation/scale) with local<->world composition, plus
  `TRSMatrix`, `Decompose`.
- `Angles.h` — `EulerAngles` (radians, YXZ order used by the editor), conversions to/from `Quat` and
  matrices.
- `BoundingBox.h` — axis-aligned box with merge/ray/point/sphere queries.
- `Frustum.h` — six planes extracted from a view-projection matrix; point/AABB/sphere tests.
- `Color.h` — sRGB <-> linear conversions (exact spec formula), packed RGBA8, luma.
- `Random.h` — `RandomGenerator` (xoshiro256++ seeded by SplitMix64): deterministic, serialisable,
  no dependency on the platform RNG.

## Epsilon policy

Floating point comparisons use a **combination** of absolute and relative tolerance:

```
|a - b| <= max(absoluteEpsilon, relativeEpsilon * max(|a|, |b|))
```

`Math::kDefaultAbsoluteEpsilon = 1e-6f` (engine units) and `kDefaultRelativeEpsilon = 1e-5f`.
Callers comparing quantities with a known scale (pixel coordinates, audio samples) pass their own.
