# Hollowpoint — Architecture

Status: **a working vertical slice through every layer** — the engine starts, loads a scene, ticks
scripts, renders and presents. Core, Math, Reflection, Serialization, ECS, Scene, Platform
windowing/input, Scripting, the Graphics device/resource/swapchain/frame-ring layers, the Renderer's
GPU passes, `Game` (scene → render-plan bridge + production scene renderer) and `Runtime` (application
shell, frame loop, standalone executable) are implemented and tested. What remains is breadth rather
than plumbing: assets, physics/audio facing code, input reaching gameplay, the editor. This document
describes what exists and the shape the rest of the engine is designed to fit into; it is updated
whenever a module lands.

## Design decisions worth knowing

| Decision | Choice | Why |
|----------|--------|-----|
| Language standard | C++20 | `std::format`, concepts, `<=>`, `source_location` — all used in anger |
| Graphics API | **Vulkan via NVRHI** on all platforms | one renderer code path; NVRHI gives D3D11/12 later without touching our layer |
| Shader language | GLSL by default (`HP_SHADER_PROFILE`), HLSL optional | both compile to SPIR-V offline with glslang; see §Shaders |
| Windowing | GLFW | tiny, mature, three platforms, no renderer coupling |
| Math | own library (see `docs/MODULE_MATH.md`) | header cost, deterministic conventions |
| Physics | Jolt | battle-tested, deterministic option, permissive license |
| Audio | miniaudio | single-file, no dependencies, WASAPI/CoreAudio/ALSA |
| Scripting | Lua 5.4 + sol2 | embeddable, fast to iterate, trivially sandboxable |
| Scene format | JSON text (`.hpscene`) + binary cooked assets | diff-able authoring, fast load path |
| Build | CMake ≥ 3.25 + FetchContent, all deps pinned | reproducible builds from a clean clone |
| Tests | doctest, one executable per module | isolate a failing subsystem without rebuilding the world |

## Module graph

The shape that matters is at the top: the renderer has to be reachable from a scene without either side depending on the other.
`Renderer` links neither ECS nor Graphics on purpose — that independence is what lets its CPU/GPU equivalence tests run with no
device and full leak checking — and `Scene` knows about entities but nothing about draw calls. The types legitimately needing
both (a drawable component, a per-frame render plan, the class that submits it) live in `Game`, and conversion runs one way only:
scene data in, draw submission out.

```
    Editor (planned)
          │
          ▼
      Runtime ────────────────┬────────────────┐
          │                   │                │
          ▼                   ▼                ▼
        Game              Graphics         Scripting
       │   │   │              │                │
       │   │   └────────┐     ▼                │
       │   │            │  Platform            │
       ▼   ▼            ▼                      ▼
    Renderer          Scene ◄───────────────────┘
                        │
   ECS · Serialization · Reflection · MathReflection
                        │
                      Math
                        │
                      Core      (std only)
```

Exactly what each module links, from its `CMakeLists.txt` — an edge list because a diagram can be misread and this one is short
enough to check by eye. `Core`, `Math` and `CompilerSetup` are omitted (every engine module takes them), as is the separate
`SerializationReflection` bridge target:

| Module | Also links |
|--------|-----------|
| ECS, Platform | — |
| Reflection | — |
| Serialization | — |
| MathReflection | Reflection |
| Scene | ECS, Reflection, Serialization, MathReflection |
| Graphics | Platform |
| Renderer | — |
| Game | ECS, Scene, Renderer, Graphics |
| Scripting | ECS, Scene |
| Runtime | Core, Math, ECS, Scene, Platform, Graphics, Serialization, Renderer, Game, and Scripting when built |

Two edges are worth noticing. `Game` is the only module linking both `Renderer` and `Graphics`, which is what keeps the shading
reference testable with no device while the class that submits it stays production code. And `Scripting` links `Scene` but **not**
`Game`: the `Script` component registers itself from inside Scripting, so `HP_ENABLE_SCRIPTING=OFF` still builds the whole scene →
draw path instead of failing to configure.

Planned edges: **Asset** (glTF via cgltf, texture cook via stb, a GUID/path database) under `Game`, replacing the single built-in
mesh-resolver line in the runtime; **Physics** (Jolt) and **Audio** (miniaudio), both fetched and building with no engine-facing
module yet.

## Module status

| Module | State | Tests |
|--------|-------|-------|
| **Core** | ✅ complete | `tests/Core/` — 65 cases: assert routing, GUIDs, FNV hashing, logger (threaded, file sink), allocator + backends + stats, ref counting, clocks and game-time |
| **Math** | ✅ complete | `tests/Math/` — 126 cases covering conventions (handedness, clip space, column-majority), transforms, volumes, colour transfer functions, RNG distribution |
| **ECS** | ✅ core complete | `tests/ECS/` — 118 cases: entity handles + generations, sparse sets, world lifecycle with deferred destruction, component storage/iteration, parent-child hierarchy (cycle rejection, subtree destroy/promote, 20k-deep teardown) |
| **MathReflection** | ✅ opt-in layer | descriptors for Vec2/Vec3/Vec4/Quat/Color; separate target so Math never depends on Reflection |
| **Reflection** | ✅ core complete | `tests/Reflection/` — 34 cases: type/field descriptors, scalar bridge, enum + array handling, name interning |
| **Serialization** | ✅ JSON layer complete | `tests/Serialization/` — 71 cases: document model, strict parser (grammar, UTF-8, surrogates, error positions), deterministic writer, round-trips incl. 20k random doubles |
| **Scene** | ✅ transforms complete | `tests/Scene/` — 22 cases: world-transform evaluation (parent-before-child order, inheritance through transform-less nodes, staleness, 20k-deep chain) |
| **Scene I/O** | ✅ `.hpscene` v1 complete | `tests/Scene/` — 36 cases: save/load round-trips, hierarchy + world-transform fidelity, two-phase parenting, version refusal, Guid identity/duplicates, unknown components, byte-stable re-save |
| **Prefabs** | ✅ complete | `tests/Scene/TestPrefab.cpp` — 20 cases: immutable shared templates, fresh identities per instance, parent-relative spawn transforms, multi-root prefabs, dead-parent refusal |
| **Platform** | ✅ windowing + input complete | `tests/Platform/` — 36 cases: GLFW lifecycle over the Null backend (DPI, close, clipboard, cursor modes), full input event queue with keycode/modifier mapping, Vulkan surface creation behind an opt-in GPU test |
| **Scripting** | ✅ VM + sandbox + engine API complete | `tests/Scripting/` — 58 cases: protected calls, memory budget, closed library set, opaque entity handles, hook dispatch with per-script error isolation, prefab spawn through host resolvers, local/world position split (`get_local_position` vs `get_position`, which is what makes "move relative to where I am" expressible at all), quaternion + Euler rotation setters with normalisation, `BindAndInitialize` running `on_create` for entities a scene attached the script to (and *not* re-running it on hot reload) |
| **Graphics** | ✅ device, resources, presentation, frame ring | `tests/Graphics/` — 61 cases: adapter enumeration with VRAM-filter cross-checks, Vulkan 1.3 device creation through instance API-version negotiation (a pinned 1.1 silently discarded the chained 1.2/1.3 feature structs), validation-layer detection, GLSL→SPIR-V compile (incl. error paths), HDR RGBA16F target round-trip, triangle draw asserted on readback pixels, NVRHI binding-offset contract pinned by `static_assert`, engine-level Buffer/Texture/RenderTarget/Pipeline with a triangle drawn and read back through wrappers only, texture upload round-trip incl. padded source pitches and rejection paths , front-face winding pinned counter-clockwise (back-face culling was discarding the camera-facing side until a scene→pixel draw exposed it), raw-Vulkan swapchain presentation against a mapped window (layout restored on acquire, per-image present discipline, back-buffer contents verified by readback) and the frame ring's slot rules |
| **Renderer** | ✅ CPU shading reference + GPU passes (driven by `Game::SceneRenderer`) | `tests/Renderer/` — 108 cases: PBR BRDF terms vs closed forms, light attenuation/unit conversions, camera projections and frusta, ACES+sRGB with invertibility properties, generated-GLSL-from-same-constants contract, **GPU-vs-CPU numeric equivalence** (`ShadingEquivalenceTests`, real draws compared to the CPU reference), IBL convolutions vs independent quadrature, tonemap blit pass compared texel-by-texel against `LinearToDisplay`, and **scene→pixel end-to-end** (`.hpscene` on disk → Game bridge → indexed draw with depth + culling → readback vs `ComputeForwardShadingCpu`) |
| **Game** | ✅ scene → renderer bridge + production scene renderer; asset-backed resolver pending | `tests/Game/` — 42 cases: authored→graphics cull-mode mapping (mutation-tested against a `static_cast` between the mismatched enums), render-plan collection (invisible/unassigned/missing-component/dead-entity paths, unresolvable paths reported by name), directional light direction respecting the authored field under rotation and scale, point-light positions, dense light indices, and `.hpscene` round-trips of MeshRenderer/Camera/Light incl. enum-by-name, omitted-fields-take-defaults, and factory-clear/hot-reload re-registration |
| Asset database | planned | load prefabs/materials from disk by path + Guid, hot reload; glTF import (cgltf fetched), texture cook (stb fetched) |
| Physics | planned | Jolt is fetched and builds (`HP_ENABLE_PHYSICS`); no engine-facing module yet |
| Audio | planned | miniaudio is fetched and builds (`HP_ENABLE_AUDIO`); no engine-facing module yet |
| **Runtime** | ✅ application shell + frame loop + executable | `tests/Runtime/` — 12 cases: start-up building every subsystem, draws actually submitted per frame (a non-zero frame count with zero draws is the blank-window bug and fails here), a script named in a `.hpscene` moving an entity through the real loop, screenshot → valid PNG at a plausible extent, missing-scene and stop-policy behaviour. `hollowpoint-runtime` is the standalone executable; `RuntimeApplication::RunFrame` is public so an editor hosts the same loop rather than duplicating it |
| Editor | planned | embeddable in the runtime (`RunFrame`, the world/device/renderer accessors exist for exactly this); `HP_BUILD_EDITOR` is accepted but builds nothing until the module lands |

## Core (implemented)

* `Assert.h` — `HP_ASSERT`/`HP_ASSERT_ARGUMENT`/`HP_UNREACHABLE` fire in **all** build types and route
  through a replaceable handler. Tests and the editor redirect them; shipping builds abort. Rationale:
  a corrupted scene must fail at the violation, not silently produce garbage three frames later.
* `Log.h` — level + channel filtering, multiple sinks (stdout/stderr, file), source location on Debug
  and above, forced flush on Fatal. Formatting is compile-time checked (`std::format_string`) and can
  never throw through a log call.
* `Memory.h` / `MemoryStats.h` — every allocation flows through `Allocate/Reallocate/Deallocate`, which
  can be redirected to a custom backend at startup. Statistics (live bytes, peak, per-category) are
  compiled in only when `HP_TRACK_ALLOCATIONS=ON`. The STL adapter is `Core::Allocator<T>`.
* `RefCounted.h` — intrusive refcounting (`Ref<T>`, `CreateObject<T>`), saturating 32-bit count with
  assertions on use-after-free patterns.
* `Guid.h` — RFC 4122 v4 identifiers, canonical text form, stable hash. Identity of every entity,
  component and asset in serialized data.
* `Hash.h` — FNV-1a 64 (auditable, cross-process stable) + a SplitMix64-based combiner. Changing these
  constants requires a serialized-format version bump.
* `Time.h` — monotonic vs wall clocks separated explicitly; `TimeSystem` implements scaled/paused/clamped
  frame time used by all simulation code.
* `PlatformInfo.h` — OS/arch detection, CPU count, env vars, executable and application-data paths.

## Runtime — the frame loop

`RuntimeApplication` owns window → device → swapchain → frame ring → renderer and runs one documented order per frame:

1. **Poll input**, recording any resize without acting on it (rebuilding a swapchain while an image is acquired is exactly
   the state Vulkan dislikes).
2. **Resize** if one was recorded, preferring the window's authoritative framebuffer size over the event payload, then
   rebuild the HDR target — the renderer's pipelines are keyed by attachment *format*, so resizing creates no new pipeline.
3. **Acquire nothing yet**: a window with no extent skips the frame without consuming a ring slot (skipped frames do not
   count toward `MaxFrameCount`, and have their own bound so an automated run cannot spin forever).
4. **Tick time**, then **scripts** — before transforms, so a script that moves an entity shows up this frame rather than
   next.
5. **Transform hierarchy update**, then camera selection (first active camera in deterministic slot order; aspect from the
   swapchain, not the window's content size, or HiDPI stretches the image).
6. **Collect** renderables through `Game::CollectRenderables` against a geometry resolver — today
   `BuiltInMeshes`, and the single line an asset-backed loader replaces.
7. **Acquire → render → tone map → present** via `Game::SceneRenderer::RenderFrame`, on the frame ring slot that is not
   still in flight. A failed present that reports out-of-date rebuilds rather than erroring.

Two properties worth stating because they are the reason this lives in one place: the **frame ring and the renderer must
agree on the slot count** (checked at start-up, since a mismatch is frame *N+2* overwriting data frame *N*'s draws are still
reading — flicker with nothing in a log), and **scene-file component types must be registered before any load**, because
`SceneIO` skips an unknown name with a warning naming only the entity.

Scripts reach the world through `hp`, entities by validated handle; a scene stores a behaviour's *name* (`"Script": {
"scriptName": "Spin" }`) and the host binds names to Lua tables, so loading a scene before its scripts is safe and forgetting
to bind produces one warning rather than silently dead behaviour.

## Conventions that cross module boundaries

* **Handles and identity**: nothing cross-referenced is a raw pointer or an array index; it is a `Guid`
  (persistent) or a validated handle (runtime).
* **Ownership**: documented at every raw pointer in a public header. Engine-owned objects are `Ref<T>`
  or `UniquePtr`; subsystems own their children.
* **Errors**: recoverable → return value + log; programmer error → assertion. No exceptions cross a
  module boundary (third-party exceptions are caught and converted at the boundary).
* **Threading**: only the main thread touches the ECS, the scene or the window. Renderer command
  recording and asset loading may run on workers and hand results back through the job system's
  completion queue (not yet implemented — planned with Platform).
* **Determinism**: simulation-facing code must not iterate unordered containers; physics runs with a
  fixed timestep driven by `TimeSystem`.

## Shaders

The shaders the engine ships with are **generated from C++**, not authored as `.glsl` files on disk:
`Renderer::BuildForwardVertexGlsl`, `BuildForwardFragmentGlsl`, `BuildTonemapBlitVertexGlsl` and friends emit GLSL text whose
constants come from the same headers the CPU reference reads, compiled at runtime by `Graphics::CompileGlsl` (glslang +
SPIRV-Tools). That choice is what makes the GPU-vs-CPU numeric equivalence tests meaningful rather than decorative — a shading
constant cannot be updated on one side and forgotten on the other, because there is only one side.

The cost is that shader source is invisible to an editor's syntax highlighting and to diff-based review of "what changed in the
lighting model", so two defences exist instead: `tests/Renderer` asserts properties of the generated text (the same-constants
contract), and `Game::ForwardShaderLayout.h` pins every constant-buffer struct against the generated GLSL by parsing it — because
a C++/GLSL layout mismatch delivers metallic where roughness belongs, with every pixel finite and plausible.

`.glsl` / `.hlsl` files under `assets/shaders/**`, compiled to SPIR-V at build time by a `HollowpointShaderC` tool and emitted
into the build tree (never committed), remain the plan for content-authored shaders — which arrive with the asset database that
would track them. Specialization constants are intended to carry per-material parameters so one compiled pipeline serves many
materials.

## Roadmap notes / open questions

0. **What to build next, in the order it now costs the most.** The engine runs, so every remaining item is a consumer of
   existing seams rather than a prerequisite: (a) the **asset database** — meshes come from `BuiltInMeshes` behind
   `Game::CollectInput::ResolveGeometry`, and glTF import plus texture cook replace that one line; (b) **input reaching
   gameplay** — the event queue is complete and only resize/close are consumed, so an action map or script-visible input API is
   the missing layer; (c) **physics and audio** facing code; (d) the **editor**, hosting `RunFrame`; (e) **golden-image
   regression**, which the screenshot path now makes possible — today's visual assertions are structural (non-empty, colour
   spread, texel spot-checks against the CPU transform) rather than whole-image.

1. **ECS storage layout** — sparse set vs archetype (EnTT-style). Leaning sparse set: simpler, stable
   references, and our component counts are modest; revisit when the renderer needs 100k entities.
2. **Asset cooking** — cook at import time into a content-addressed store (fast loads, incremental),
   versus cook-on-build in the editor. Not decided; both need the same GUID database first.
3. **Editor UI** — Dear ImGui + implot for profiling overlays; docking branch. The editor must be fully
   scriptable, which pushes us to define the command/console API before building widgets.
