# Hollowpoint — Agent & Developer Guide

Hollowpoint is a production-grade 3D game engine for Windows, macOS and Linux. This file is the
contract for anyone (human or AI agent) changing this repository. Read it before writing code.

---

## 1. Ground rules

1. **Production quality or nothing.** No TODO-driven architecture, no placeholder implementations that
   silently do nothing, no "good enough for now". If a feature cannot be done properly in the current
   step, leave a documented gap in `notes/` and keep the public API honest (return errors, don't fake
   success).
2. **Everything ships with tests.** A new class or function is not finished until a test exercises it.
   Bug fixes come with a regression test that fails without the fix.
3. **Never look outside this repository** for reference implementations or copy code from other
   projects on the machine. Third-party sources under `third_party/` (fetched, pinned) are the only
   external code.
4. **Ask only when blocked.** Prefer a small, reversible decision documented in `notes/` over a
   question. Ask when a choice is expensive to undo (file formats, public API, serialized data).
5. **The working tree must always build and pass tests.** Never commit with a red test suite, and never
   disable or delete a test to get green — fix the cause or document why the test was wrong.

---

## 2. Code style (Hazel/Cherno conventions)

Mandatory. When in doubt, read neighbouring code — the codebase is the primary reference.

| Element               | Convention                          | Example                                    |
|-----------------------|-------------------------------------|--------------------------------------------|
| Namespaces            | `PascalCase`, one per module        | `Hollowpoint::Renderer`                     |
| Classes / structs     | `PascalCase`                        | `class MeshRenderer`                        |
| Functions / methods   | `PascalCase`                        | `void SubmitDrawCalls()`                    |
| Enum types            | `PascalCase` (`enum class`)         | `enum class ShaderStage : UInt32`           |
| Enum values           | `PascalCase`                        | `ShaderStage::Vertex`                       |
| Local variables       | `camelCase`                         | `float deltaTimeSeconds`                    |
| Function parameters   | `camelCase`                         | `void Resize(int width, int height)`        |
| Private members       | `m_` + `PascalCase`                 | `int m_FrameCount = 0;`                     |
| Statics (file/class)  | `s_` + `PascalCase`                 | `static Logger s_Logger;`                   |
| Constants (`constexpr`) | `kCamelCase`                      | `constexpr float kPi = 3.14159265f;`        |
| Macros                | `HP_` prefix, `SCREAMING_SNAKE`     | `HP_ASSERT`, `HP_LOG_INFO`                  |
| Files / directories   | `PascalCase.h/.cpp`, one class each  | `MeshRenderer.h`, `MeshRenderer.cpp`        |
| Booleans              | `Is/Has/Can/Should` prefix          | `bool IsVisible() const;`                   |

Additional rules:

- **4-space indentation, Allman braces** (opening brace on its own line). Never tabs. 120 column limit.
- **Member initialisation**: every member gets a default initialiser in its declaration. Prefer
  `m_Value{}` / `= {}` over uninitialised members.
- **`#pragma once`** in all headers. Include order (enforced by `clang-format`, do not hand-order):
  the header a `.cpp` implements when it matches the file name, then standard library, then
  third-party, then engine headers — alphabetised within each group, one blank line between groups.
- **Full namespace qualification is forbidden in headers**; use `namespace Hollowpoint::X { ... }`.
  Never `using namespace` at file scope (test files may use it inside functions).
- **Doc comments** (Doxygen): `//!<` only as a **trailing** comment on the line it documents; use
  `//!` on its own line **above** a declaration (or `/** */`). Every public class and non-trivial
  public function gets one, explaining *why*, contracts, ownership and units — never restating the
  signature.
- **Ownership and lifetime are documented** at every raw pointer in a public API (`// not owned`,
  `// caller keeps alive`). Engine-owned objects are `Ref<T>` (intrusive refcount) or `UniquePtr`.
- **Errors**: recoverable problems return `Result<T>` / `bool` + log; programmer errors use
  `HP_ASSERT`/`HP_ASSERT_ARGUMENT`. Never throw across a module boundary; third-party exceptions are
  caught at the boundary and converted.
- **No old-style casts** (`(int)x`) — use `static_cast`/`reinterpret_cast`; `-Wold-style-cast` is on.
- **Warnings are errors** for engine targets (`hp_set_engine_target`). Third-party code is exempt; do
  not add warnings to it, and do not weaken the engine warning list to accommodate new code.
- **Formatting**: `clang-format` with the repository `.clang-format`. Run `scripts/format.py --check`
  before committing (it exits non-zero on violations).

---

## 3. Architecture

Layered; each layer depends only on layers below it. Breaking this order breaks every build and is the
single most common mistake — check the dependency graph in `docs/ARCHITECTURE.md`.

```
Editor ─┐
        ├─► Runtime ─► Renderer ─► Graphics ─► Platform ─► ECS ─► Serialization ─► Math ─► Core
Apps  ──┘                                                              ▲
                          Physics ─────────────────────────────────────┤
                          Audio   ─────────────────────────────────────┤
                          Scripting ───────────────────────────────────┘
```

Module → CMake target → directory:

| Module         | Target                    | Directory             | Depends on            |
|----------------|---------------------------|-----------------------|-----------------------|
| Core           | `Hollowpoint::Core`       | `engine/Core`         | std only              |
| Math           | `Hollowpoint::Math`       | `engine/Math`         | Core (glm)            |
| ECS            | `Hollowpoint::ECS`        | `engine/ECS`          | Core, Math            |
| Reflection     | `Hollowpoint::Reflection` | `engine/Reflection`   | Core, Math            |
| Serialization  | `Hollowpoint::Serialization` | `engine/Serialization` | Core (JSON layer); +Math, +Reflection for the value bridge |

> The JSON layer (`Hollowpoint::Serialization`: `JsonValue`, parser, writer) depends on Core alone and must
> stay that way — it is a leaf every module may include. The reflection-driven encode/decode bridge is a
> **separate target** (`Hollowpoint::SerializationReflection`) because it needs Reflection; merging them would
> make linking JSON drag Reflection along, and the layering rule would then rot quietly instead of failing a
> build.
| Scene          | `Hollowpoint::Scene`      | `engine/Scene`        | Core, Math, ECS, Reflection, Serialization(+Reflection), MathReflection |
| MathReflection | `Hollowpoint::MathReflection` | `engine/MathReflection` | Core, Math, Reflection |
| Asset          | `Hollowpoint::Asset`      | `engine/Asset`        | Core, Math, Serialization |
| Platform       | `Hollowpoint::Platform`   | `engine/Platform`     | Core, Math (GLFW)     |
| Graphics       | `Hollowpoint::Graphics`   | `engine/Graphics`     | Platform (NVRHI)      |
| Renderer       | `Hollowpoint::Renderer`   | `engine/Renderer`     | Graphics, Asset       |
| Physics        | `Hollowpoint::Physics`    | `engine/Physics`      | Core, Math, ECS (Jolt)|
| Audio          | `Hollowpoint::Audio`      | `engine/Audio`        | Core, Math (miniaudio)|
| Scripting      | `Hollowpoint::Scripting`  | `engine/Scripting`    | Core, Math, ECS, Reflection (Lua) |

Rules that keep this graph honest:

- **Core never depends on anything but the standard library.** No GLFW, no NVRHI, no Lua.
- Lower layers must not know about higher ones. If a subsystem needs to react to something above it,
  use an event/callback registered by the higher layer — never an include.
- Public headers live in `<Module>/Include/Hollowpoint/<Module>/*.h`; implementation-only headers stay
  in `<Module>/Src/` and are never installed/exported.
- Every module links `Hollowpoint::CompilerSetup` (features + feature macros) and, if it needs
  third-party code, `Hollowpoint::ThirdParty`.

---

## 4. Engine conventions worth knowing

- **Identity**: entities, components and assets are addressed by `Core::Guid`, never by pointer or
  array index, in anything serialized or cross-thread.
- **Logging**: `HP_LOG_INFO("CHANNEL", "text {}", value)`. Channels are short upper-case tags
  (`CORE`, `ECS`, `RENDER`, `PHYSICS`, `AUDIO`, `SCRIPT`, `ASSET`, `PLATFORM`). Never `printf`/
  `std::cout` in engine code.
- **Assertions**: `HP_ASSERT(cond)` for invariants, `HP_ASSERT_ARGUMENT(cond, "msg")` for public API
  preconditions, `HP_VERIFY` when the expression has side effects. Assertions fire in **all** build
  configurations (a corrupted scene must not silently produce garbage); they can be redirected via
  `Core::SetAssertHandler`, which is what tests and the editor do.
- **An assertion is a report, never a guard.** Because a handler may return "continue", code after
  `HP_ASSERT` runs in the editor and in tests. So an assertion must *always* be followed by a safe
  action (clamp, early return, fallback value) before any out-of-bounds or null dereference it was
  warning about. ASan caught exactly this in `Mat4::GetRow`; do not let it happen again.
- **Time**: game logic uses `TimeSystem::GetDeltaSeconds()` (scaled, clamped). Wall-clock reads are
  for logs/timestamps only. Never `sleep` in a frame-loop path.
- **Math**: use `Hollowpoint::Math` types (`Vec3`, `Mat4`, `Quat`, `Color`). Column-major matrices,
  right-handed, +Y up, metres, radians. glm may be used inside modules; do not leak glm types through
  public headers of layers above Graphics.
- **Handles over raw pointers**: GPU resources are `Ref<Graphics::...>`; ECS access goes through
  `Entity`/`ComponentHandle`. A handle is 8 bytes and validated (`IsValid()`).
- **Scripts** are gameplay code; engine code never hard-codes game behaviour.
- **Determinism**: physics and simulation must be reproducible from a fixed seed (see Jolt
  `CROSS_PLATFORM_DETERMINISTIC`); do not iterate over unordered containers when producing
  simulation output or serialized data.

---

## 5. Testing

- Framework: [doctest](https://github.com/doctest/doctest). One executable per module
  (`tests/<Module>/Test<Feature>.cpp` → `<Module>Tests`), registered with CTest.
- Every test needs a `TEST_CASE` name that states the behaviour ("hitches are clamped to the maximum
  delta"), grouped in a `TEST_SUITE("Module/Area")`.
- Prefer deterministic, hermetic tests: no wall-clock sleeps (inject time), no filesystem outside
  `std::filesystem::temp_directory_path()`, no network, no GPU requirement unless the test is labelled
  `[gpu]` and skipped when no device exists.
- Engine assertions inside a test are **failures** unless the test expects them — wrap those calls in
  `Hollowpoint::Test::ScopedAssertCapture`.
- GPU rendering tests render to an offscreen target and assert on readback pixels (see
  `tests/Graphics/TestDevice.cpp`); visual regressions additionally write a `.cmp.png` reference.

Commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug      # configure (dev preset)
cmake --build build -j                                      # build everything
ctest --test-dir build --output-on-failure                  # run all tests
ctest --test-dir build -R CoreTests --output-on-failure     # one module
scripts/run_tests.sh                                        # full gate: format + build + tests + sanitizers
```

---

## 6. Build system

- CMake ≥ 3.25, Ninja recommended. Presets in `CMakePresets.json`: `linux-debug`, `linux-release`,
  `windows-debug`, `macos-debug`.
- All third-party dependencies are pinned to exact tags/commits in `third_party/CMakeLists.txt` and
  fetched with `FetchContent`. **Pinning is a rule, not a suggestion** — floating refs make builds
  irreproducible. Bump one dependency at a time and record why.
- Shared dependency cache: `-DHP_DEPS_ROOT=$HOME/.cache/hollowpoint-deps` (what the presets use).
- Engine options: `HP_BUILD_TESTS`, `HP_BUILD_EDITOR`, `HP_BUILD_RUNTIME`, `HP_ENABLE_VULKAN`,
  `HP_ENABLE_PHYSICS`, `HP_ENABLE_AUDIO`, `HP_ENABLE_SCRIPTING`, `HP_WARNINGS_AS_ERRORS`,
  `HP_TRACK_ALLOCATIONS`, `HP_ENABLE_TRACE_LOGS`, `HP_SHADER_PROFILE` (`GLSL`|`HLSL`).
- Shader sources live in `assets/shaders/**.glsl` / `**.hlsl` and are compiled to SPIR-V at build time
  by the `HollowpointShaderC` tool; generated files land in the build tree and are never committed.

---

## 7. Git workflow

- `main` is always green and releasable. Work on feature branches (`feature/gltf-import`,
  `fix/shadow-bias`) or, for agent sessions, commit small coherent units directly with honest messages.
- Commit message format: `module: what changed and why` in the subject (≤ 72 chars), body explaining
  the *why*, wrapped at 100 columns. Examples:
  - `renderer: clamp shadow bias to avoid acne at grazing angles`
  - `ecs: fix dangling component pointer when destroying a parent entity`
- Before every commit: `scripts/run_tests.sh` green, `scripts/format.py --check` clean, and the diff
  reviewed for style compliance (see §8).
- Never commit build outputs, IDE files, or generated shaders. Never force-push or rewrite history on
  `main`; never touch credentials or `.git/config` of remotes.

## 8. Code review checklist (run this before every commit)

1. Does it build with warnings-as-errors on Debug **and** Release?
2. Are all new behaviours covered by tests, and did the tests actually run?
3. Public API documented? Ownership and lifetime stated for every raw pointer?
4. Style: naming, member prefixes, brace style, no old-style casts, no `using namespace` at file scope?
5. Layering respected (no upward includes, Core stays dependency-free)?
6. No hidden costs in hot paths (allocations per frame, unordered-map lookups in the render loop)?
7. Errors handled: file not found, GPU device lost, script runtime error, malformed asset — logged,
   not swallowed, and not asserted if they are plausibly user data?
8. Platform-specific code confined to `engine/Platform` (or a `Win32/`, `Linux/`, `Mac/` subfolder)?

## 9. Agent operating notes

- Work autonomously; keep `notes/EXPERIMENTS.md` and `docs/ARCHITECTURE.md` updated as decisions are
  made — transcripts die, files don't.
- Long builds: run in the background and poll; never let a timeout kill a build or a test suite.
- GPU tests must degrade gracefully on machines without a Vulkan device: skip with a clear message, do
  not fail. Detect once, cache the result.
- The editor is designed to be driven by agents: every editor action has a scripting/console command
  equivalent (see `docs/EDITOR_SCRIPTING.md`). Prefer the scripted path when automating scene work.
