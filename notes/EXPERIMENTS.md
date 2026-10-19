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

---

## Session 4 — Serialization: the engine's own JSON layer

### What was built and why it is shaped this way

`Hollowpoint::Serialization` now has a JSON document model (`JsonValue` / `JsonObject`), a strict parser, and a
deterministic writer. nlohmann/json stays in the dependency list for tooling but the engine does not link it.

* **Insertion-ordered objects** (parallel key/value arrays, not `std::map`/`unordered_map`). Scene files are committed
  and reviewed; member order that changes between runs turns every save into a whole-file diff. Replacing a member
  overwrites in place so "load → tweak one field → save" is a one-line diff.
* **Numbers keep Int64 / UInt64 / Real apart.** JSON has one number type; an engine cannot. Entity indices, GUID halves
  and 64-bit hashes must never pass through a double (precision dies above 2^53), and values in `(Int64Max, UInt64Max]`
  are stored unsigned so they round-trip exactly.
* **Errors never throw.** A malformed scene is expected input (hand-edited, truncated download, version skew). Parsing
  returns `{Success, Root, Error}` with byte offset **and** line/column plus a caret-underlined excerpt — tools want
  the offset, humans will not convert one. On failure `Root` is reset to null so a half-built tree cannot be mistaken
  for data.
* **Strict grammar.** Leading zeros, `+1`, `.5`, `1.`, `NaN`, `Infinity`, trailing commas and raw control characters in
  strings are all rejected: accepting them produces files other readers refuse, and "loads here, fails there" is a
  miserable bug to chase. Depth is bounded (`MaxWalkSteps`-style guard) because the parser recurses per open bracket —
  without a limit, a file of `[[[[…` is a remote stack overflow.
* **UTF-8 validated at the boundary** (no overlongs, no direct surrogate encoding, ≤ U+10FFFF), so every string in the
  engine can be assumed valid and text/hash code needs no defensive checks of its own.

### Experiment: write our own JSON or use nlohmann?

**Hypothesis.** A general-purpose JSON library is good enough for scene files; writing a parser is wasted effort.

**Test.** Before deciding, checked the three properties scene serialization actually depends on against what a general
library exposes, and measured `std::to_chars` as the number backend:

| Requirement | Generic library | Engine's own layer |
| --- | --- | --- |
| Shortest round-trip reals (`0.1`, not `0.10000000000000001`) | configurable, format leaks into diffs | guaranteed by `to_chars` |
| Insertion-ordered members (diffable scenes) | no (sorted or hashed) | yes, by construction |
| Error = byte offset + line/column + context excerpt | generic position only | exact, incl. CRLF handling |

`to_chars` shortest-round-trip was fuzzed over **300 000 random bit patterns: 0 failures** (and it is locale-independent,
unlike `snprintf("%g")`, which reads `1,5` as `1` in a European locale).

**Conclusion.** Build the layer. The three properties are not cosmetics — they decide whether committed scene files are
reviewable and whether an asset hash computed over serialised data is stable — and none of them are reachable through
a general library's configuration surface. Cost: ~900 lines plus tests, one day.

### Bugs found while building this module (all caught by tests, none by inspection)

1. **Every negative integer parsed as zero.** `std::from_chars` rejects a `-` when the target type is unsigned and
   leaves the output untouched; the code fed signed literals to an unsigned conversion and ignored
   `invalid_argument`. Fixed by branching on the sign established during the grammar scan and converting magnitudes
   separately, with `-2^63` handled explicitly (negating after the cast is signed overflow).
2. **The object writer emitted an extra quote per key** — `{"a"":1,…}`. The separator string re-included the closing
   quote that `WriteString` had already written. Caught by the compact round-trip test.
3. **Whole-number floats silently changed kind on save.** `3.0` wrote as `3`, so a load/save cycle turned every
   integral float into an integer: value preserved, type lost, diffs churned. Writer now appends `.0`.
4. **Recursive JSON type is not portable when members are inline.** `std::vector<JsonValue>` with an incomplete
   `JsonValue` is legal (P0448), but *using* `clear`/assignment of it while still incomplete compiles on libstdc++ and
   fails on clang/libc++ (`arithmetic on a pointer to an incomplete type`) because libc++ instantiates eagerly. Debug
   (GCC) was green for hours while the ASan (clang) configure could not build it. Rule recorded in the header: every
   member whose body touches `JsonArray` is defined out-of-line.
5. **A test asserted the wrong thing about its own fixture** (`AsString() == ""` on a value that was legitimately a
   string). Recorded because it looked like an accessor bug for a minute — check the test before blaming the module.

### Design note: no global error counters

An early draft counted "report at most 16 kind mismatches per parse" in a function-local static. That makes diagnostics
depend on process history and therefore on test order, which is how suites go flaky. Accessors are now pure (fallback,
no logging); reporting lives where paths are known — the parser, and later the reflection bridge.

### State at end of session 4

* `main` = scaffold, Core, Math, Reflection, Serialization (JSON), ECS (+ hierarchy).
* 414 doctest cases / 481 405 assertions (65 Core + 126 Math + 34 Reflection + 71 Serialization + 118 ECS),
  green in Debug and under ASan+UBSan.
* Next: reflection-driven encode/decode (engine types ↔ JSON with versioned fields), then transforms, prefabs/scenes.

---

## Session 5 — Scene: world-transform evaluation (+ ECS structure version)

### What was built and why it is shaped this way

New module `Hollowpoint::Scene` (Core + Math + ECS, no Graphics dependency — the renderer reads world matrices, never
the other way round):

* **`LocalTransform` stores the transform relative to its parent**, never in world space. It's the only representation
  that survives reparenting (world coordinates either teleport the child or force a recompute of everything below),
  and it lets an animator author "rotate the turret" without knowing where the tank is.
* **`TransformHierarchy` derives world matrices** in one pass per frame and caches them by entity slot. World values
  are *never* stored next to the local ones: two sources of truth require every write path (scripting, animation,
  physics, editor gizmo, scene load) to remember to invalidate, one of them will forget, and the symptom — object
  renders in the wrong place with correct data — reads as a renderer bug. With local-only storage, staleness is
  impossible by construction.
* **Traversal is parent-before-child and iterative.** Slot order would read a child's parent matrix before the parent
  was updated this frame: a one-frame lag that appears as jitter on nested objects and disappears when things stop
  moving — precisely the bug that survives review, so it has its own test (`evaluation order is parent-before-child
  regardless of creation or slot order`). Iterative means depth costs worklist memory, never stack.
* **Transform-less entities pass inheritance through.** A logic-only group node stores what it *inherited*, so a mesh
  under it composes against the group's parent rather than snapping to world origin — behaviour users expect and which
  then never needs explaining in docs.
* **ECS gained `GetStructureVersion()`** (bumped by create/destroy/reparent-success) and **`GetEntityAtSlot()`**.
  Derived caches compare one integer instead of every subsystem hooking three lifecycle events; nothing to forget
  because there is nothing to hook. `Unchanged` reparents deliberately do *not* bump it — they happen constantly on
  scene reload and would otherwise throw away every cache for nothing.

### Experiment: are the new tests actually load-bearing?

**Hypothesis.** A passing test suite proves the behaviours it claims are protected.

**Method.** Mutate the implementation three ways after the suite goes green and check that a test fails — and that
the *right* test fails, not some unrelated one:

| Mutation | Result | Test that caught it |
| --- | --- | --- |
| Compose in slot order instead of parent-before-child | **caught** | `evaluation order is parent-before-child regardless of creation or slot order` |
| Transform-less node stores identity instead of inheriting | **caught** | `entities without a LocalTransform pass their inherited transform to children` |
| Never record the evaluated structure version | **caught** | `stale caches are detected and answered correctly rather than wrongly` |

A fourth mutation (`IsStale` ignoring the version comparison) failed to compile because the member became unused —
which is itself a signal: the compiler flags dead state that a subtler mutation would have left in place.

**Conclusion.** All three semantic mutations were caught, each by the test named for that behaviour, so the suite is
load-bearing rather than decorative. Method worth keeping: **write the implementation, then break it on purpose and
check which test fires.** It costs minutes and directly measures what "the tests cover this" is actually worth. A test
that never fails under mutation documents nothing.

**Also worth keeping from this module:** every test compares against `ExpectedWorld()`, a deliberately naive reference
implementation that walks up the parent chain. If both the system and the test used matrix composition tricks, an
error in the trick would be invisible; comparing an O(depth) reference against the O(1)-per-entity production path is
what makes 500-entity and 20k-deep cases meaningful instead of tautological.

### State at end of session 5

* `main` = scaffold, Core, Math, Reflection, Serialization (JSON), ECS (+ hierarchy), **Scene** (transforms).
* 436 doctest cases / 502 210 assertions across 6 executables, green in Debug (GCC) and under ASan+UBSan (clang) —
  both toolchains are now part of the gate after a real portability bug passed GCC and failed clang.
* Next: prefabs + `.hpscene` load/save on top of Serialization (reflection-driven component encode/decode, entity
  references by Guid), then Platform/renderer so the transform work becomes visible.

---

## Session 6 — Serialization: reflection-driven encode/decode (the bridge)

### What was built and why it is shaped this way

`Hollowpoint::SerializationReflection` (new **separate target**): reflected object ↔ `JsonValue`. This is what makes
"declare a field once, get saving for free" true — scene files, prefabs and script-visible components all go through
one implementation, so field layout knowledge exists in exactly one place.

* **Separate CMake target on purpose.** The JSON layer must stay a Core-only leaf that anything can include; the bridge
  pulls in Reflection. Merging them would make linking JSON drag Reflection along, and the layering rule would rot
  quietly instead of failing a build.
* **Field order follows declaration order** → deterministic output, diffable scene files.
* **`DontSerialize` skipped on both paths**; `ReadOnly` saved by default (it describes the inspector, not the save
  file — dropping it would reset scripted health/ammo on every load); `Deprecated` still loaded (that flag means
  "hidden from the UI").
* **Enums stored by name.** Inserting an enumerator mid-list then silently reinterpreting saved data is the failure
  this prevents, and `"mode": "Directional"` beats `"mode": 2`. An unknown name is an error, not a fallback to zero:
  `"Direcional"` quietly becoming `Point` is a bug nobody finds. Numeric form still loads (hand-written files, old builds).
* **A field absent from JSON keeps its current value.** This is what makes adding a component field safe for every
  existing scene file — no migration, no version bump; it simply gets the same default a fresh component would have.
* **Unknown JSON members reported and skipped, not fatal** — the ordinary signature of a newer build or renamed field.
  Losing one value with a message naming the type beats refusing to load the level.
* **Diagnostics accumulate into a path-qualified list** (`BridgeComponent.nested.x: expected a number`) instead of
  throwing or stopping at the first fault: a level with twelve bad fields should produce twelve messages in one pass,
  because otherwise fixing them is a twelve-run errand.

### Bug found (the expensive kind): double-applied field offsets

Reflection's field helpers (`ReadFieldValue`, `ReadStringField`, `GetArrayElementAddress`, `ResizeArrayField`, …) all
take the **containing object** and apply `field.GetAddress(object)` internally. The first bridge version passed
already-offset field addresses into them, so the offset applied twice — not a wrong value but an out-of-bounds read,
which segfaulted on the first `std::string` field. 27 of 34 new tests failed at once, which is what made it obvious
the *convention* was wrong rather than one code path.

Fix: every internal function in the file now takes `(object, field)` and never a pre-computed field address, with the
rule written at the top of the file — "if you are tempted to hoist `GetAddress` as an optimisation, don't; either
change every call in the function or none".

**Lesson.** When an API's addressing convention is implicit, mixing it produces memory errors rather than type errors.
State the convention once at the top of every file that touches it, in terms of what breaks.

### Methodology flaw found while mutation-testing (worse than a test gap)

Ran mutations to check the new tests are load-bearing. Mutation B (write enums numerically instead of by name)
appeared **not** to be caught — the suite still reported 105/105 green. It had not been caught for a much dumber
reason: **the mutation did not compile** (`-Werror=unused-variable` after neutering the condition), so ninja left the
previous binary in place and I ran the old one.

That is a false negative about my own test suite, recorded as evidence. Fixed by gating every mutation on build
success:

```sh
cmake --build ... || { echo "BUILD FAILED (result meaningless)"; restore; exit 2; }
./Tests
```

With the gate in place, all four semantic mutations were caught by exactly the intended tests:

| Mutation | Caught by |
| --- | --- |
| `DontSerialize` ignored on write | `DontSerialize fields never reach the wire` |
| Enums written numerically | `enums are stored by name, not number` |
| Absent field resets to zero instead of keeping its value | `a field absent from JSON keeps its current value` |
| Arrays never shrink | `arrays resize down when the file is shorter` |
| Unknown members silently ignored | `an unknown JSON member is reported and skipped, not fatal` |

**Lesson.** A negative result from a test run is only meaningful if you know which binary ran. Any "prove X fails"
workflow must assert the build succeeded before trusting the run — otherwise `-Werror` (usually our friend) converts a
mutation experiment into fabricated evidence. Skill `mutation-test-before-trusting-tests` updated with the gate.

### Also found by tests, not reading

* **Encode/decode asymmetry:** encode ignored `IncludeReadOnlyFields` while decode honoured it, so a file written with
  the option off would still be loaded over a live component. Caught because both directions were tested against the
  same option rather than each in isolation.
* Two wrong expectations in my own tests (an unknown-key test asserted a pre-set value survived when the file did
  contain that field; and `index * 3 - 1` at index 4999 is 14996, not 14997). Recorded because both looked like
  implementation bugs for a minute — check the test's arithmetic before blaming the module.

### State at end of session 6

* `main` = scaffold, Core, Math, Reflection, Serialization (JSON + reflection bridge), ECS (+ hierarchy), Scene
  (transforms).
* 470 doctest cases / 507 935 assertions across 6 executables, green in Debug (GCC) and under ASan+UBSan (clang).
* Next: `.hpscene` load/save and prefabs on top of the bridge (entity identity by Guid, references, instantiation),
  then scripting so it can spawn them.

---

## Session 7 — `.hpscene` save/load, component factory, math-type descriptors

### What was built

* **`Scene::ComponentFactory`** — maps a scene-file component name to code that creates it. Loading knows only the name
  written in the file; the ECS creates components through templated `AddComponent<T>`, so something must bridge
  name → concrete type without the world knowing any component types (that open-type property is what lets plugins add
  components at all). It lives in Scene, not ECS, so the ECS stays free of reflection/serialisation.
* **`Scene::WriteScene` / `LoadScene` / `LoadSceneFromText`** — `.hpscene` format v1 with a version field, entity
  identity by Guid, hierarchy by file-local id, components keyed by reflected type name.
* **`MathReflection`** (new opt-in module) — descriptors for Vec2/Vec3/Vec4/Quat/Color.

### Design decisions and why

* **Two identity schemes.** `id` is a dense file-local index used for structure (parent links); `guid` is persistent
  identity for cross-file references. GUIDs-only would force pasting UUIDs into every parent link, making hand-editing
  a large scene miserable; indices-only would break references whenever entities are reordered between files. Both,
  with the local index confined to one document, gets readable files *and* stable identity.
* **Loading is two-phase** (create all entities → assign parents). Otherwise a child appearing before its parent in the
  file could not link, forcing authoring tools and hand-editors to guarantee topological order for no benefit.
* **Version refusal rather than best-effort.** A newer major version is refused, not partially loaded: the bridge
  tolerates unknown fields, so a v2 file would "load successfully" while quietly discarding whatever structure v2 relies
  on — data loss reported as success. Missing *fields* stay fine (they take defaults), which is forward compatibility
  where it is safe and refusal where it is not.
* **Structural errors abort; content errors degrade.** A malformed entity entry means the document cannot be trusted, so
  the world is left untouched rather than half-replaced ("this level won't load" beats "this level half loaded and
  something is missing"). An unknown component name or bad field value is a warning: those are the ordinary signature of
  a newer/older build, and blocking the level over them is worse than skipping with a message naming it.
* **`RegisterSceneTypes()` is explicit and idempotent, not a static initialiser.** This lives in a *static library*, and
  a translation unit whose symbols nobody references is simply not linked in — so its registrations would silently never
  run and scenes would save without transforms. `WriteScene`/`LoadScene` both call it; the cost is one atomic load.
* **Math type names are stable strings** (`"Math.Vec3"`), not compiler-derived names: template-type output differs
  across compilers (scene files would become compiler-specific) and a user component named `Vec3` could otherwise
  collide with an engine type in the registry.

### Bugs found (all by tests, none by reading)

1. **Every component was silently skipped on save.** ECS storage identifies types by C++ *type info* name; Reflection
   registers display names. Passing `storage.GetTypeName()` to `FindType()` looked exactly right and matched nothing —
   saves "succeeded" while writing entities with no components at all. Fixed by resolving through the registry's
   existing `FindTypeByTypeInfoName` index and keying files by the registered display name. Caught because 8 tests
   failed at once, which is what a wrong *convention* looks like (a wrong code path fails one).
2. **`ComponentFactory::Register<T>` was declared as a template but defined in the `.cpp`,** so no other translation
   unit could instantiate it (`used but never defined`). Restructured: inline template over a non-template, type-erased
   `RegisterFactory(typeName, callback, reflectionTypeName)`. The header exposes one small function and no container.
3. **`WriteScene` hard-coded `"name": "UntitledScene"`,** writing a plausible-but-false value into every file. Replaced
   with an optional `SceneName` that omits the key entirely when unset — an empty-looking name gets trusted by whatever
   reads it.

Also worth recording: **I asserted APIs that did not exist** while writing tests (`World::IsRoot`, `Registry::ForEachType`,
`Entity::IsAlive`, `RegisterTypeWithBase`) and invented a helper (`TypeNameOf`) from memory of a design I had not
actually built. Each cost one build cycle; the fix is to grep for the signature before writing call sites, which I did
consistently after that.

### Inheritance gap in the bridge (found while designing scene I/O, fixed there)

`TypeDescriptor::GetFieldCount()` reports **own** fields only — so any consumer must walk `GetBaseType()`. The bridge
did not, meaning a component with a reflected base saved *without its inherited fields*: valid-looking output, silently
missing data. Fixed with one `ForEachFieldInChain` helper (base-first ordering) plus `ChainHasField` so an inherited key
is not reported as unknown. Documented constraint: field offsets are relative to the most-derived object, correct for
single inheritance; multiple inheritance could place a base at a non-zero offset, which reflected components do not use.

Mutation-checked with the build gate in place: skipping base traversal fails 4 tests (the three inheritance cases plus
one round-trip). Also verified `IsStale`/version mutations still fail their named tests after the refactor.

### Methodology note

Two of my own tests were **weak rather than wrong** and I only noticed via mutation testing:
* "a child listed before its parent still links" asserted only root/child *counts*. A loader that resolves parents only
  when already created keeps those counts right while dropping the actual edge — so the test could pass with a broken
  hierarchy. Fixed by asserting `GetParent(child) == root` and positions, and by adding a case where the parent is a
  **later** id (the direction that genuinely requires two-phase loading). That mutation now fails 4 tests instead of 3
  unrelated ones.
* A test named "destroying a subtree keeps only the survivor" asserted promotion behaviour that `DestroyEntity` does not
  implement by default (promotion is an explicit opt-in, covered in the ECS suite). Renamed and split into two cases so
  each mode is tested as documented rather than as I assumed.

**Lesson.** A test that passes for the wrong reason is worse than no test: it retires your suspicion. Mutation testing
finds these; asserting specific relationships (not counts) prevents them.

### State at end of session 7

* `main` = scaffold, Core, Math, Reflection, Serialization (JSON + bridge), **MathReflection**, ECS (+hierarchy),
  **Scene** (transforms, `.hpscene` save/load, component factory).
* **510 doctest cases / 502 622 assertions** across 6 executables; green in Debug (GCC) and ASan+UBSan (clang).
* Next: prefabs (instantiate a saved document into an existing world — the `ClearWorldFirst=false` path is already
  there), then scripting so it can spawn them, then Platform/renderer to make transforms visible.

---

## Session 8 — Prefabs (instantiation), plus two methodology lessons worth keeping

### What was built

`Scene::Prefab` + `InstantiatePrefab`. A prefab is deliberately **just a saved scene document plus a name**, not a
parallel data structure: everything that makes scenes work (component factory, reflection bridge, hierarchy resolution,
unknown-component tolerance) applies to prefabs for free and there is one loader to debug rather than two. The
alternative — a dedicated prefab format — reliably drifts from the scene format within months, and the drift surfaces
as "prefabs support X but levels don't".

Decisions:
* **Immutable template, stored by value.** Prefabs are shared assets instantiated many times; if an instance could edit
  its template, the tenth spawn would differ from the first — a bug that appears only after minutes of runtime and
  never in a smoke test. There is no non-const accessor to the document.
* **Fresh identities by default.** Spawning one asset twice must not produce two entities claiming one Guid, since
  every Guid-based reference would then resolve ambiguously. Keeping stored identities is opt-in (deterministic replay,
  golden files).
* **Instantiating never clears the world**, whatever the caller's defaults — it appends. `ClearWorldFirst = false` is
  forced internally rather than trusted from outside.
* **Attach after loading, not by patching the document**, so one shared document can be instantiated under a hundred
  parents.

### The transform bug I would have shipped

The first version composed the spawn offset against the parent's *local* transform **and** attached the instance to
that parent — double-counting it. Symptom: spawned objects drift progressively further away the deeper they are nested,
which reads as a bug in the transform system rather than in spawning. Correct behaviour needs no composition at all:
attaching already gives `world = parentWorld * local`, so setting `local = offset` yields
`parentWorld * offset * authored`.

Second half of the same function: compose **outside** the authored value
(`authored.Concatenated(offset)`) rather than replacing it. Replacing flattens a prefab whose root is raised by half its
height, so every instance lands half-buried; composing the other way applies the author's own transform to the spawn
position, so moving a rotated instance moves it twice.

Both are pinned by tests that assert resulting *world positions*, not stored locals.

### Lesson 1 — a test that passes for the wrong reason (found again, worse)

`MUT-P5`: hard-code `AssignFreshIdentities = false` (i.e. ignore the option and reuse the asset's guids). **The suite
passed.** My "instantiating twice creates independent entities with distinct identities" test looks like it covers this
and cannot: with the mutation, the *first* instance keeps the document's guids and the loader's duplicate-guid guard
rescues the second instance — so all four entities still end up unique. The assertion was satisfied by a different
mechanism than the one under test.

Fix: assert **directly against the authored guids** — no instantiated entity may hold them, and the asset's identities
must remain unclaimed in the world. Mutation now fails exactly that test.

**Rule.** When a safety net (dedup guard, clamp, fallback) sits near the behaviour under test, an assertion about the
*symptom* can be satisfied by the net. Assert the mechanism itself: name the specific values that must not appear,
rather than counting how many things are unique.

### Lesson 2 — "100% tests passed" from a failed build (this one produced false evidence)

The ASan gate printed `100% tests passed out of 6` while the **build had failed** (`-Wmismatched-tags`:
`class SceneLoadReport;` forward-declared where it is defined as a `struct` — caught by clang, invisible to GCC). Ninja
left the previous binaries, ctest happily ran them, and I nearly recorded "ASan green" for work that had not been built.

This is the same failure mode as the mutation-build trap from session 6, but in the *main* gate rather than an
experiment, which makes it more dangerous: nobody suspects the routine command.

Fixes adopted:
1. Never write `cmake --build ... | grep`. A pipeline returns the **last** command's status, so a failed build reports
   success to anything checking `$?`. Run the build bare (or `set -o pipefail`) and check its exit code explicitly.
2. Verify the artefact is fresh (`ls -l` timestamp / `ninja: no work to do`) before trusting a test run, and confirm
   the new test count appears in the sanitised binary — 78 Scene cases there proves it was built from current sources.

### Other findings

* **Use-after-move caught by an existing test.** Recording load results did `report.CreatedEntities = std::move(created)`
  and then indexed `created` to collect roots. Reading a moved-from vector is legal and silently empty, so every prefab
  instance would have looked root-less; the SceneIO suite failed immediately. Roots are now collected before the move,
  with a comment saying the order is load-bearing.
* **Applying a spawn offset to an entity with no LocalTransform needs the component created.** Such an entity inherits
  its parent exactly, so writing nowhere would silently leave instances stacked at the parent's origin. Covered by its
  own test (`MUT-P7`).
* Spawning into a **dead handle** is refused rather than allowed: otherwise the instance attaches to whatever now
  occupies that recycled slot — an object appearing welded to unrelated geometry.

### Mutation results (all build-gated)

| Mutation | Result | Caught by |
| --- | --- | --- |
| Replace authored transform instead of composing | caught (3) | `the authored root offset is preserved rather than replaced` +2 |
| Double-count the parent transform | caught | `the spawn offset is relative to the parent, not doubled by it` |
| Never attach roots to the requested parent | caught (3) | `roots attach to the requested parent` +2 |
| Allow spawning into a dead parent | caught | `instantiating into a dead parent is refused` |
| Ignore `AssignFreshIdentities` (reuse asset guids) | **initially missed**, then caught after rewriting the test | `instances do not reuse the asset's own identities` |
| Instantiating clears the world | caught (6) | `instantiating never clears the world` +5 |
| Drop the offset when the entity has no transform | caught | `a spawn offset on a prefab without a root LocalTransform still applies` |

### State at end of session 8

* **530 doctest cases / 502 826 assertions** across 6 executables; Debug (GCC) and ASan+UBSan (clang) both built clean
  from scratch and green (verified fresh, per Lesson 2).
* Scene module now: LocalTransform, TransformHierarchy, `.hpscene` v1 I/O, ComponentFactory, Prefab + instantiation.
* Next: scripting (spawn prefabs, create/destroy entities, per-component updates), then Platform/renderer to make the
  transform work visible, then the comprehensive test scene that ties it together.

---

## Session 9 — Scripting: Lua VM, sandbox, engine API, dispatch (+ four real bugs)

### Goal and hypothesis

Expose gameplay scripting on top of the ECS/prefab work: a Lua VM that can create/destroy entities, spawn prefabs,
read/write transforms, and run per-frame hooks. Hypothesis from the earlier de-risking session — *entities as opaque
integer handles plus plain functions (no C++ usertypes)* — would let scripts mutate the world without any lifetime
coupling to Lua's garbage collector. That hypothesis held, but only after fixing how the API was registered: the first
implementation still leaked engine object lifetimes into the collector, in a way no single-VM test could see.

### Design decisions

| Decision | Reason |
|---|---|
| Entities cross the boundary as `int64` handles (slot+generation), never userdata/pointers | A destroyed entity cannot be used: a stale handle decodes to "dead" instead of addressing whatever recycled the slot. No pointer means nothing for Lua's GC to keep alive or free. |
| Handle encoding is `pack(slot, gen) + 1` | Reserves 0 for "no entity" without aliasing. The obvious alternative — encode normally and remap the single colliding value — maps entity (0,0) onto entity (1,0), so a script's reference to the first entity silently addresses the second. |
| Libraries opened individually (base/coroutine/table/math/string/utf8); `io`, `os`, `load`, `dofile`, `require`, `package`, `debug` absent | Fail-closed: adding a library upstream cannot silently grant file/process/native-loader access. Scripts are content. |
| No engine state captured in sol2 closures; raw `lua_CFunction` + light-userdata upvalue | See Bug 1. sol2 stores capturing functors as userdata with `__gc`. |
| Custom allocator enforces a byte budget | A runaway script becomes a Lua error naming itself, not an OOM kill blamed on the engine. |
| Hook dispatch snapshots entities, then revalidates `IsAlive` per entity | Scripts destroy and spawn entities during update (split-on-death); iterating live storage would skip or revisit freed slots. A spawner must not tick what it spawned in the same frame. |
| Unbound script names reported **once** | 200 entities using one renamed script produced 400 warnings/second otherwise; and there is deliberately no "no scripts bound → early-out" fast path, because that silences the exact failure worth reporting. |

### Bug 1 — engine objects destroyed by Lua's GC (order-dependent crash)

Symptom: `SIGSEGV` in `_Sp_counted_base::_M_release` with `this = 0x3f8000003f800000` (two `1.0f`s — matrix data
overwriting a freed control block), during `lua_close` → `callallpendingfinalizers`. Only reproducible under *some*
doctest orderings; ASan did not report it (different allocation layout).

Root cause: `api.set_function("create_entity", [state]{...})` — sol2 stores the capturing functor as Lua **userdata with
a `__gc` finalizer**. The captured `shared_ptr<ApiState>` (and transitively the `World` pointer) was therefore owned by
the collector: freed whenever GC liked, including after the host had destroyed the world.

Fix: register raw `lua_CFunction`s whose context is a **light-userdata upvalue** (no finalizer at all; unreachable from
script code because `debug` is never opened), and have `ScriptVM` own that context through an explicit deleter, running
it deterministically *before* `lua_close`. Move operations were written out rather than defaulted so the moved-from VM
cannot double-destroy it.

Lesson: *"no C++ usertypes" is not enough — any captured state in a bound closure is userdata state.* Tests that each
create one VM pass; the bug needs many VMs in one process, which is now an explicit test case.

### Bug 2 — allocator accounting was wrong, silently disabling the memory cap

Two compounding errors against Lua's real contract (`block == NULL` ⇒ `oldSize` is a **type tag**, `LUA_TTHREAD`, not a
size; `luaM_realloc_` returns NULL on failure *without* updating `GCdebt`):

1. Subtracting `oldSize` on fresh allocations under-counted every VM.
2. A "defensive" `min(oldSize, LiveBytes)` clamp swallowed the resulting mismatch — driving `LiveBytes` to **0**, which
   means the cap stopped applying entirely, with no error anywhere. Measured: 16+ clamp events per run, usage reported
   as 0 while the VM was alive.

Fix: compute `released = block ? oldSize : 0` once and use it for both the cap test and the accounting, so they cannot
disagree. Rejecting *grows* is correct (not leaky) precisely because Lua leaves the old block counted on failure — and
grows must be capped, since `t[#t+1] = ...` in a loop **is** the runaway pattern.

Measured after the fix: our counter equals Lua's own `LUA_GCCOUNT`/`LUA_GCCOUNTB` byte count exactly at every stage;
zero drift across 12 VM create/destroy cycles; cap fires and usage stays under the limit (67 108 606 ≤ 67 108 864).

Lesson: *a guard that clamps instead of trusting its contract fails invisibly.* And `collectgarbage("count")` returns
float kilobytes — use the C API counters for an equality assertion (the float version failed spuriously under ASan).

### Bug 3 — printf format specifiers into an fmt-style logger

Every `HP_LOG_*("...", "%.*s", n, s)` in the new module compiled and logged the literal text `%.*s`, discarding all
arguments (`%` is not special to `std::format`). Caught only because one test was strengthened from a count assertion to
`AnyMessageContains(...)`; count-only assertions elsewhere would have passed against garbage. Fixed across the module and
noted in AGENTS.md with the rule that log assertions assert content.

### Bug 4 — handle-encoding aliasing (caught by tests, not review)

The original `EncodeEntity` remapped its one colliding encoding to `1`, which is exactly entity `(slot 1, gen 0)`'s
handle. A test asserting "no two distinct entities share a handle" catches it; the exhaustive slot×generation round-trip
test would not have.

### Methodology notes (process failures, worth keeping)

* **`assert`-driven Python patch scripts silently skipped edits and once mangled a function** (a grow branch lost its
  `realloc` and inherited a `malloc`, producing a crash that looked like an engine bug). Several "fixes" appeared not to
  work because the file was never actually changed, or a stale binary was run. From now on: after any scripted edit,
  `grep` for the intended text before building, and never trust a build/test result whose edit was not verified present.
* **`git checkout` cannot restore untracked files.** A mutation left in place after a failed restore; mutations must be
  reverted by re-writing from an explicit `/tmp` copy and confirmed with `grep -c MUTANT`.
* Mutation results are only meaningful when the build succeeded (build-gated), and M3 (`released = 0`) was rejected by
  `-Werror` rather than a test — that is *not* evidence of coverage.

### Mutation testing (build-gated, each reverted and re-verified)

| Mutation | Result |
|---|---|
| `oldSize` treated as size when `block == nullptr` (the original bug) | caught — 4 assertions incl. the exact-count invariant |
| Memory cap disabled entirely | caught — runaway + post-churn cap tests |
| Sandbox removed (`luaL_openlibs`, `load`/`require` restored) | caught — forbidden-globals test, 5 hits |
| Hook calls unprotected (`lua_call` instead of `lua_pcall`) | caught — process aborts, suite fails |

### State at end of session 9

* **581 doctest cases / 503 444 assertions** across 7 executables; Debug (GCC) and ASan+UBSan (clang, built from
  scratch after `rm -rf build/asan`, leak detection on) both green; clang-format clean. Scripting: 49 cases covering handle encoding, sandbox policy, error containment, memory budget and
  accounting exactness, engine API semantics (including recycled-slot safety), and dispatch (mid-frame destroy, spawn
  during update, hot rebinding, lifecycle ordering, subtree destruction semantics).
* Scripting module: `ScriptVM`, `EntityHandle`, `EngineAPI` (create/destroy/is_alive/parent/child/sibling/set_parent/
  get_position/set_position/spawn_prefab[_at]/time/log_*), `ScriptSystem` (bind + on_update/on_create/on_destroy).
* Next: drive scripts from a frame loop with a real `.hpscene` + prefab asset set, then Platform/renderer so any of this
  is visible, then the comprehensive test scene.

---

## Session 10 — M10 Platform: window + input over GLFW

### Goal and hypothesis

Add the first module that touches the outside world: window creation, DPI-aware sizing, an input event
queue, clipboard, and a Vulkan surface handle. Hypothesis worth testing explicitly: **windowing can be
unit-tested on this machine at all**, since it is an ostree (Bazzite) system with a real Wayland session but
no X11/Wayland *development* headers. If not, the fallback would be headless-only smoke tests, which would
leave input handling unverified for the rest of the project.

### Environment probes (each before writing code that depended on it)

| Probe | Result | Consequence |
|---|---|---|
| Vulkan devices present | RTX 3080 (api 1.4) + llvmpipe, `vkCreateInstance` OK | renderer work is viable |
| GLFW availability | only Linuxbrew (`glfw3Config.cmake`, 3.5.1); no system dev package | must resolve via CMake config, not `find_library` |
| Window creation on Wayland session | works; a 64×64 content window reports fb 92×92 → **fractional scaling 1.4375** | DPI design (content vs framebuffer) is load-bearing, not theoretical |
| Null backend (`glfwInitHint(GLFW_PLATFORM, NULL)`) | creates windows, no presentation | headless tests possible |
| `glfw3native.h` | **needs `<wayland-client.h>` / `<X11/Xlib.h>`, absent here** | native-handle support must be compile-gated, not mandatory |
| GLFW headers | need BOTH `GLFW_INCLUDE_NONE` (no `/usr/include/GL/gl.h`) and `GLFW_INCLUDE_VULKAN` (declares `glfwCreateWindowSurface`) | Platform depends on `Vulkan::Headers` |
| Ad-hoc probe vs CMake build | hand-written g++ line linked `/usr/lib64/libglfw.so.3` (**3.4**); CMake target gets brew **3.5.1** via RPATH | probes must be built through a real CMake target or they measure the wrong library |

The last row caused a wrong conclusion that cost time: an ad-hoc probe reported "Null backend → 0 Vulkan
instance extensions". The real (CMake-built, 3.5.1) behaviour is `VK_KHR_surface` + `VK_EXT_headless_surface`.
A test written against the probe would have asserted the opposite of reality.

### Bugs found by my own tests (each a real defect, not a test artefact)

1. **`glfwGetPlatform()` is a scalar enum, not a bitmask.** Values are consecutive from `GLFW_ANY_PLATFORM`
   (0x60001 Win32 … 0x60005 Null). My initial `QueryBackend()` used `platform & GLFW_PLATFORM_WAYLAND`, which
   also matches Win32 and Cocoa — so a Windows build would have been reported as running Wayland, and the
   headless fallback decision would be made on a wrong value. My first probe *appeared* to confirm the bitmask
   reading because it ran on the Null backend where only one bit matched by luck. Fixed with `switch`/equality.
2. **Nested `PlatformScope` silently ignored an explicitly requested backend** — joining whatever the process
   already had would let code that checked "is Wayland active?" believe whatever the first scope chose. Now a
   mismatched explicit request makes *that scope* unusable (`IsInitialized()` false, `GetBackend()` None) while
   leaving the shared initialisation intact.
3. **Refcount imbalance:** the refused (non-usable) scope still decremented in its destructor, driving the count
   to zero while GLFW was still initialised — every later window creation was then rejected as "windowing not
   initialized" (17 tests failed at once). Increments and decrements must pair exactly.
4. **`glfw` is an IMPORTED target created in `third_party/`**, so it is invisible to sibling dirs; linking it from
   `engine/Platform` produced "header not found". Fixed by wrapping in the `hp_glfw` INTERFACE library (same pattern
   as `hp_sol2`). Imported targets do not propagate across directory scope; normal targets do.
5. **Test helper with unreachable logic:** `if (desc.Title.empty()) desc.Title = "…"` could never fire because
   `WindowDesc::Title` defaults to a non-empty string, so the title test silently checked the wrong value. Caught
   as a genuine failure — helpers should not contain hidden defaulting.

### LeakSanitizer: isolating rather than suppressing

Any test that queries GLFW's Vulkan instance extensions makes GLFW initialise the Vulkan loader → vendor driver →
session **D-Bus** registration, which retains ~2 146 bytes for the process lifetime (stack entirely inside
`libdbus-1`/`libvulkan`/the NVIDIA module). LSan reports per *process*, so one such call would force suppressions
onto every windowing test and blunt engine leak checking for all of them.

Resolution: `PlatformVulkanTests` is a **separate executable** holding only the Vulkan-touching assertions, with a
narrow suppression file attached to it via `LEAK_SUPPRESSIONS` (a new opt-in option on `hp_add_module_test`). The
main `PlatformTests` runs fully unsuppressed.

Bisecting this was non-obvious: forcing the Null backend did *not* avoid it, individual tests leaked nothing in
isolation, and doctest's `--first=N` does not limit to N cases (so an early bisect read as "test 1 leaks"). The
actual trigger was found by running every test case individually.

**Suppression verified load-bearing in both directions:**
* without `LSAN_OPTIONS`, `PlatformVulkanTests` reports the 2 146-byte third-party leak;
* with suppressions active, a deliberately leaked `Detail::WindowImpl` is still reported and fails the suite
  (mutation reverted, both presets re-verified green).

So the suppression removes noise without creating a blind spot.

### Design decisions worth keeping

* **Input is an ordered event queue**, not polled state: recordable/replayable, and two presses in one frame are
  not collapsed (the classic "inputs skip when the framerate drops").
* **Engine-owned `KeyCode`** with contiguous letter/digit/function ranges, so bindings never depend on a windowing
  library's numeric values and range iteration needs no lookup table. Keypad digits are deliberately distinct from
  top-row digits.
* **Content vs framebuffer coordinates** everywhere, with DPI scale exposed; this machine's 1.4375 scaling makes
  any equality assumption between the two wrong.
* **Events queued, drained into a caller-supplied vector** — an idle frame allocates nothing. Queue is a `deque`
  because the overflow policy drops from the front (a `vector::erase(begin())` would be O(n) per event).
* **Queue cap (16 384) with chunked drop and one report per episode**: an app that stops pumping input must not
  OOM because a key was held down.
* **`InjectEvent`** is a product feature, not a test backdoor: the editor is meant to be agent-driven, so sending
  synthetic input is required — it also makes the queue testable with no display at all. It goes through the same
  queue and overflow policy as real events.
* **Tests never map a window** (forced `Visible=false`, Null backend): a suite must not flash windows across the
  user's screen or steal focus mid-keystroke. Nothing under test needs presentation.
* **Native handles are compile-gated** (`HP_PLATFORM_HAS_NATIVE_HEADERS`): requiring X11/Wayland headers would make
  the whole engine unbuildable on this OS to support one accessor.

### Deliberately not asserted (would encode one compositor's behaviour)

* That a window reads back as iconified — KDE does not iconify a never-mapped window, others do. What *is*
  guaranteed and checked: restoring cannot leave a window reported iconified.
* `IsVisible()` after `SetVisible(true)` — that would map the window onto the user's display.

### Mutation tests

| Mutation | Result |
|---|---|
| Leak a `Detail::WindowImpl` in `Window::Create` (suppressions active) | caught — LSan names `WindowImpl::WindowImpl()` and the suite fails |

### State at end of session 10

* **617 doctest cases / 503 665 assertions across 9 executables** — measured by summing each binary's doctest
  summary rather than estimated, since an earlier section of these notes carried a hand-tallied figure that turned
  out to be off. Debug (GCC) and ASan+UBSan (clang) both green, clang-format clean. Platform contributes 34 cases /
  214 assertions, plus 2 Vulkan-surface cases in the separate target.
* New module `engine/Platform` (`PlatformScope`, `Window`, `InputEvent`) wired behind `HP_ENABLE_VULKAN`.
* Next: M11 Graphics — NVRHI device, offscreen render target, SPIR-V pipeline compile through glslang, and pixel
  readback so "the triangle renders" is verified by pixels rather than by hope. Surface creation gets exercised
  there with a real `VkInstance` (driver state being inherent to what is tested).
