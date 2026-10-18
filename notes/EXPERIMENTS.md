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

---

## Session 2 — pushing the foundation, and the Reflection module

### Remote

The repository is pushed to **`git@github.com:BlizAce/GameEngineTest.git`** (the user supplied this remote
after `Hollowpoint` turned out not to exist and no API token was available to create it). `origin/main` tracks
local `main`. Workflow correction agreed mid-session: **commit per unit as work proceeds**, not in one blob at
the end — the Reflection module sat uncommitted across a long stretch, which is exactly what reviewable history
is supposed to prevent.

### Why Reflection exists before ECS

The scene format and the editor inspector both need to read/write members of arbitrary component types. The
alternative (a hand-written JSON writer plus ImGui widget per component) duplicates layout knowledge in three
places and drifts. Registering a type once gives saving, loading, inspector and scripting bindings one source
of truth, so this module is a prerequisite for ECS rather than an accessory to it.

### Design decisions worth keeping

* **`TypeTraits<T>` → `ValueKind`, with Struct as the fallback.** Anything not specialised is a nested struct
  resolved by name, so adding a math or component type needs no change to reflection.
* **Lazy sub-type resolution.** A field's struct/array-element descriptor is looked up by name on first use, so
  translation-unit registration order cannot matter. Verified by registering `Vec2Fixture` *after* the type that
  contains it.
* **Member offsets measured, not cast.** The standard `offsetof` takes a member *name*, not a pointer-to-member
  value, so it is unusable from a template. An object is constructed in aligned storage and the address
  difference taken — well defined, unlike the null-base `reinterpret_cast` trick, and it costs one construction
  per field at startup. Requiring default-constructibility is free: the ECS and serializer need it anyway.
* **Type-erased `ArrayOps`, captured from the concrete member type.** An earlier draft read `begin`/`end` through
  a hand-made three-pointer struct — that compiles and then breaks on another standard library. Function pointers
  also let `std::vector<float>` work, which a descriptor-only design cannot express (a scalar has no
  `TypeDescriptor`).
* **One shared conversion policy** (`Detail/ScalarConvert.h`) for field writes *and* array-element writes. If the
  two diverged, editing a value in the inspector and loading it from disk would store different bytes — invisible
  until someone noticed an edit changing a value they did not touch.
* **Writes clamp, never wrap.** 300 into an `Int8` stores 127, not 44. A wrapped field is a silent simulation
  drift; a saturated one is visibly wrong in the inspector.
* **NaN survives into float fields, and is zeroed for integers.** An earlier version rewrote NaN to zero
  everywhere, destroying the only evidence of an upstream bug. Integers cannot represent it, so they degrade to
  zero *and log*, which distinguishes "stored zero" from "had to discard a NaN".
* **Leaked registry singleton.** Descriptors are referenced by raw pointer from scenes and UI, and the editor may
  still draw an inspector during static destruction. A few dozen descriptors; no dangling-pointer class at exit.

### Bugs found while building this module

1. **`WriteScalar` silently refused bool fields** — `Boolean` is not a numeric kind, so every guard fell through
   to `return false`. An inspector checkbox would have been dead on arrival. Caught by the first round-trip test.
2. **A false-alarm error for every plain scalar array.** `GetArrayElementType()` reported "element type was never
   registered" for `std::vector<int>`, which is legal and common (UVs, indices, sample data). Now null is returned
   silently unless the elements are *supposed* to be reflected structs.
3. **Diagnostics on a redraw path.** Resolution failures fired per call; with an inspector open that buries the
   log. Now reported at most once per field (a warning nobody reads trains people to ignore the channel).
4. **Redundant `ElementsAreStructs` flag** duplicating `ElementKind == Struct` — removed before it could drift.

### Test-support addition

`ScopedLogCapture` (tests/Support) attaches an `ILogSink` for a scope. Two subtleties handled once rather than per
test: the dispatcher drops messages above its *global* maximum before formatting, so the ceiling must be raised
too (refcounted, because captures nest and can span threads); and sinks run on arbitrary threads, so storage is
mutex-guarded.

### State at end of session 2

* `main` = scaffold, Core, Math, docs, test-log-capture, **Reflection**.
* 225 doctest cases (65 Core + 126 Math + 34 Reflection), green in Debug and under ASan+UBSan.
* `scripts/run_tests.sh` (format → configure → build → ctest) is the commit gate; it caught its own formatting
  regressions twice, which is the intended behaviour.
* Next: ECS on top of Reflection, then Serialization (JSON scene format), then Platform.

---

## Session 3 — ECS: handles, world lifecycle, components, hierarchy

### What was built and why it is shaped this way

`Hollowpoint::ECS` now has entity handles + sparse sets, world lifecycle with deferred destruction, component
storage/registry/iteration, and a first-class parent/child hierarchy.

* **32-bit slot + 32-bit generation handle** (`Entity`), not a 64-bit id. A handle must be checked against the
  world on every use regardless of id width, so the extra bits buy nothing except cache pressure; the generation
  is what actually prevents use-after-destroy through a stale handle.
* **Destruction is deferred** (`DestroyEntity` marks dead + queues; `FlushPendingDestructions` reclaims). Freeing
  immediately would invalidate pointers handed to the caller's own iteration, and every engine convention (Unity
  `Destroy`, Unreal `Kill`) defers for exactly that reason. Liveness is re-checked *per entity* inside `ForEach`,
  so an entity destroyed earlier in the same pass is skipped rather than visited with components not yet freed.
* **Hierarchy is world state, not a component.** Destroying an entity has to resolve its children; if the tree lived
  in components, the world would need to know those types exist (breaking the open-type design) or leave orphaned
  child lists whenever a module forgot a cleanup hook. It stores topology only — no transforms — so ECS still
  depends on nothing but Core; the transform system will subscribe to reparent events later.
* **Intrusive doubly-linked sibling lists** rather than per-parent vectors: no allocation per parent, O(1) insert and
  remove, and children are appended never reordered, so iteration order is stable across runs — which is what keeps
  saved scene files diffable in git.

### Experiment: should tree depth be capped?

**Hypothesis.** A hard `MaxDepth = 256` on the hierarchy is worthwhile protection, because recursive consumers
(world-transform composition, recursive teardown, scene save) would overflow the stack on a pathological chain built
by a scripted bug.

**Experiment.** Wrote the cap into `SetParent`, then wrote the obvious stress test: build a 20 000-level chain and
destroy it.

**Result.** The test failed at level ~256 with `ReparentResult = 4` (`DepthLimitExceeded`) — the *setup* was refused,
not the teardown. Investigating showed the cap protected nothing that the implementation needed:

| Concern | With recursive walks | With the iterative walks actually shipped |
| --- | --- | --- |
| Stack overflow on deep trees | Real risk | Impossible — destroy uses an explicit worklist |
| Cost of `SetParent` | O(depth) because of the cap's own depth walk | O(1) |
| Valid content (long attachment/bone chains) | Rejected at 256 | Accepted; bounded only by entity count |

**Conclusion.** The cap was removed, not raised. Raising it would have kept a limit that is arbitrary in both
directions: too small for real content, and unnecessary because the walks it guarded are iterative. `MaxDepth` became
`MaxWalkSteps = 1<<20`, used *only* as corruption detection — if a parent chase exceeds that, some other bug created a
cycle and hanging is worse than reporting. The regression test was inverted into "deep chains are accepted" (5 000
levels) plus the 20 000-level destroy test, so the decision is now enforced by CI rather than remembered in prose.

**Lesson.** When a limit exists to protect against a failure mode, verify the failure mode survives without it. A
guard inherited from "engines have depth limits" costs valid behaviour and buys nothing once the walks are iterative.

### Bugs found while building this module

1. **`ForEach` doc/implementation mismatch.** Docs promised "every live entity"; the body only tested component
   presence, so an entity destroyed earlier in the same pass could still be visited with components not yet freed.
   Both `ForEach` overloads now re-check liveness per entity.
2. **Nested-type ordering in `Hierarchy.h`.** A member function's return type is *not* a complete-class context, so
   `const Node*` cannot appear before `struct Node` is declared. Fixed by declaring the node struct above the
   accessors.
3. **`HP_UNUSED(x)` expands through `sizeof`, which does not evaluate its operand.** Wrapping a call in it silently
   skips the call. All existing uses audited and safe, but it must never be used to "execute" a side effect — noted
   in the style guide.
4. **Test expectation arithmetic, not engine logic.** Entities were created with even-valued health, so the expected
   post-increment total was `2*i+1`, not `i+1`. Worth recording because the failure looked like an ECS bug for a
   minute: check the test's own arithmetic before blaming the system under test.

### State at end of session 3

* `main` = scaffold, Core, Math, Reflection, **ECS** (handles, world, components, hierarchy).
* 343 doctest cases / 420 895 assertions (65 Core + 126 Math + 34 Reflection + 118 ECS), green in
  Debug and under ASan+UBSan.
* Next: serialization (JSON scene format) + prefabs, then transforms, then physics and scripting on top.
