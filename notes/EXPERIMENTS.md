# Hollowpoint — development log

Reverse-chronological lab notebook. Each entry: what was tried, what the result was, why, and what it
implies. Code comments explain the final state; this file explains how we got there and what failed.

---

## 2026-10-19 — Foundation: build system, Core, Math

### Toolchain findings (this machine: Bazzite 44, GCC 16.2.1, RTX 3080)

| Item | Finding | Consequence |
|------|---------|-------------|
| cmake / ninja / clang-format | absent from the system, present in Homebrew | build instructions use `PATH=/home/linuxbrew/.linuxbrew/bin` |
| Vulkan headers | not installed system-wide | fetched by CMake (`Vulkan-Headers` pinned to NVRHI's expected tag) |
| GLFW runtime vs dev | `libglfw.so.3` exists, no X11/Wayland/GL **dev** headers, no pkg-config database | building GLFW from source fails; use an installed glfw3 CMake package → added `HP_USE_SYSTEM_GLFW` + `HP_Glfw_DIR` |
| NVRHI (Vulkan-only) | clones, configures and builds clean; its memory-queries test runs on the RTX 3080 | graphics abstraction is viable; no need to write a Vulkan device layer |
| glslang + SPIRV-Tools | build from source in ~35 s at -O2 with 24 jobs | offline shader compilation is cheap enough for every build |
| Jolt v5.6.0 | builds in ~19 s as a subproject (`SOURCE_SUBDIR Build`) | physics dependency settled |
| Lua | upstream repo has **no CMakeLists** (makefile only); sol2's CMake insists on finding a *system* Lua | compile Lua sources into our own static lib; expose sol2 as an interface target |
| System GCC sanitizers | `-fsanitize=address` fails to link: `cannot find /usr/lib64/libasan.so.8.0.0` | `linux-asan` preset pins clang (Homebrew LLVM has the runtimes) |

**Verified ASan actually works here** before trusting it as a gate: heap-buffer-overflow, global-buffer-
overflow and heap-use-after-free probes were all caught with exit code 1. Note that `-O1` on GCC 16
miscompiled glslang's parser during an early probe, so optimization levels are left at CMake defaults
rather than being tuned per-config.

### Design decisions and the reasoning

* **Assertions fire in all build configurations.** Deliberate: a corrupted scene or bad asset must fail
  at the violation rather than silently producing garbage later, which is what makes field bugs
  reproducible. Combined with a replaceable handler so tests/editor can continue.
* **An assertion is a report, not a guard.** Because a handler may return "continue", code after an
  assertion still runs in debug/editor builds. ASan caught exactly this: `Mat4::GetRow` asserted
  `rowIndex < 4` and then indexed with it — a stack-buffer-overflow in the test suite. Every such check
  now clamps/returns before dereferencing. Rule recorded in AGENTS.md §4.
* **Own math library instead of exposing glm.** Two concrete reasons: glm's headers would be pulled into
  every translation unit that touches a vector, and our conventions (column-major + Vulkan clip space +
  right-handed + a specific epsilon policy) become one tested implementation rather than per-file macro
  choices. glm stays available internally to other modules.
* **Vec2 is alignas(8), Vec3/Vec4 alignas(16).** Vec2 must map onto GPU `float2` attributes and shader
  `vec2`; padding it to 16 would break vertex layouts and waste memory in position arrays. This was
  originally `alignas(16)` on all three — caught by a size test, not by a crash.
* **Colour uses the exact IEC 61966-2-1 curve, not pow(2.2).** At sRGB 0.05 the spec's linear segment
  gives 0.00394 vs 0.00137 for the power approximation — nearly 3× brighter, visible as crushed shadows.
  Pinned by a test with both numbers in the comment.
* **FNV-1a 64 for hashes.** Deterministic across processes/platforms (hashes go into saved scenes) and
  auditable; changing a constant requires a serialized-format version bump. Documented in Hash.h.
* **Guid stored as four big-endian-normalised words** so hashing/comparison are padding-independent and
  the binary scene format is portable.
* **Handler slot uses a mutex, not `std::atomic<16-byte struct>`.** Lock-free 16-byte atomics need
  libatomic on Clang, which this toolchain lacks — a "links only on my machine" trap for zero measurable
  benefit on a path that is fatal by default.

### Bugs found by tests during development (all real)

1. `Mat4::GetRow` out-of-bounds read after asserting (ASan/UBSan) → clamp + rule in AGENTS.md.
2. `Quat::LookRotation` built its basis with two sign errors that cancelled for `forward == -Z`, so the
   common case looked correct while other directions produced a **reflection** matrix (determinant −1).
   Now derived once from `backward = -forward` with a determinant assertion.
3. `Transform::MapPoint` applied rotation before scale — shears positions under non-uniform scale, and
   broke parent/child composition against `parent.ToMatrix() * child.ToMatrix()`.
4. `Frustum::TestSphere` only classified a straddling sphere when its centre was already outside the
   plane, so "centre inside, radius crosses the far plane" wrongly reported Inside.
5. Warning flags `-Walloc-zero`, `-Wduplicated-branches`, `-Wduplicated-cond` are GCC-only; applying them
   unconditionally broke every Clang build with `-Werror,-Wunknown-warning-option`. Now gated on
   `CMAKE_CXX_COMPILER_ID`, which also protects the macOS (AppleClang) target.

### Test expectations that were wrong (engine was right)

Recording these because "the test failed, so I changed the test" needs justification each time:

* `PingPong(2.75)` → 0.75, not 0.25 (verified against the reference triangle-wave definition).
* Colour round-trip: `FromSRGB` takes **sRGB** inputs, so re-encoding must return the original bytes
  (64/128/191), not the encoding of a linear colour (137/188/225). Both directions now have separate
  tests, and the byte comparison allows ±1 LSB because one value sits exactly on the rounding boundary.
* `Radians(30 + kTwoPi)` is not "390 degrees" — the test added radians to degrees.
* A matrix-association test multiplied a `Vec3` by a `Mat4` and then called a method that does not exist;
  rewritten as `(A*B)*v == A*(B*v)`.
* Transpose expectations used the wrong (row, column) mental model of `m[col][row]`.

### Commit hygiene lesson

The first three commits were not independently buildable: the scaffold commit referenced `engine/Core`
before it existed, and `tests/CMakeLists.txt` listed the Math suite before the Math module landed. Since
nothing was pushed, history was rewritten so each commit configures, builds and passes its tests on its
own (verified by checking out each commit into a scratch worktree). Rule: when adding a module, the same
commit must add its `add_subdirectory` line and its test registration.

Also self-inflicted and cleaned up: a verification loop ran `cp -r .git <sha>` which left stray `.git`
copies in the working tree — never copy a repository directory into itself.

### State at end of session

* 3 commits on `main`: build scaffold, Core, Math.
* 191 doctest cases (65 Core + 126 Math), all passing in Debug and under ASan+UBSan; `scripts/run_tests.sh`
  (format → configure → build → test) is green.
* Full third-party set configures with everything enabled: NVRHI, glslang/SPIRV-Tools, Jolt, miniaudio,
  Lua+sol2 all compile in-tree.
* Not yet implemented: ECS, Reflection/Serialization, Platform, Graphics, Asset, Renderer, Physics/Audio/
  Scripting wrappers, editor, runtime.
