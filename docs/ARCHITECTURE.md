# Hollowpoint — Architecture

Status: **foundation complete; runtime layers landing** (Core, Math, Reflection, Serialization, ECS,
Scene, Platform windowing/input, Scripting and the Graphics device layer are implemented and tested;
the Graphics resource layer and the Renderer's GPU passes are in flight). This document describes
what exists and the shape the rest of the engine is designed to fit into; it is updated whenever a
module lands.

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

```
                 ┌──────────┐     ┌─────────┐
   Editor ──────►│ Runtime  │────►│Renderer │
   (ImGui)       └────┬─────┘     └────┬────┘
                      │                │
              ┌───────▼────────┐  ┌────▼─────┐   ┌──────────┐
              │   Platform     │◄─┤Graphics  │◄──│  Asset   │
              │ (GLFW, files)  │  │ (NVRHI)  │   │(glTF,img)│
              └───────┬────────┘  └──────────┘   └────┬─────┘
        ┌─────────────┼──────────────┐                │
   ┌────▼────┐  ┌─────▼────┐  ┌──────▼───┐    ┌───────▼─────┐
   │Physics  │  │  Audio   │  │Scripting │    │Serialization│
   │ (Jolt)  │  │(miniaudio)│ │(Lua/sol2)│    │  (+Reflection)│
   └────┬────┘  └─────┬────┘  └──────┬───┘    └───────┬─────┘
        └─────────────┴──────┬───────┴────────────────┘
                        ┌────▼────┐
                        │   ECS   │◄─── Scene (transforms, prefabs)
                        └────┬────┘
                        ┌────▼────┐
                        │  Math   │
                        └────┬────┘
                        ┌────▼────┐
                        │  Core   │  (std only)
                        └─────────┘
```

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
| **Scripting** | ✅ VM + sandbox + engine API complete | `tests/Scripting/` — 51 cases: protected calls, memory budget, closed library set, opaque entity handles, hook dispatch with per-script error isolation, prefab spawn through host resolvers |
| **Graphics** | 🟡 device + resource layers landing | `tests/Graphics/` — 38 cases: adapter enumeration with VRAM-filter cross-checks, Vulkan 1.2 device creation, validation-layer detection, GLSL→SPIR-V compile (incl. error paths), HDR RGBA16F target round-trip, triangle draw asserted on readback pixels, NVRHI binding-offset contract pinned by `static_assert`, engine-level Buffer/Texture/RenderTarget/Pipeline with a triangle drawn and read back through wrappers only, texture upload round-trip incl. padded source pitches and rejection paths |
| **Renderer** | 🟡 CPU shading reference complete; GPU passes landing | `tests/Renderer/` — 101 cases: PBR BRDF terms vs closed forms, light attenuation/unit conversions, camera projections and frusta, ACES+sRGB with invertibility properties, generated-GLSL-from-same-constants contract, **GPU-vs-CPU numeric equivalence** (`ShadingEquivalenceTests`, real draws compared to the CPU reference), IBL convolutions vs independent quadrature, tonemap blit pass compared texel-by-texel against `LinearToDisplay` |
| Asset database | planned | load prefabs/materials from disk by path + Guid, hot reload; glTF import (cgltf fetched), texture cook (stb fetched) |
| Physics | planned | Jolt is fetched and builds (`HP_ENABLE_PHYSICS`); no engine-facing module yet |
| Audio | planned | miniaudio is fetched and builds (`HP_ENABLE_AUDIO`); no engine-facing module yet |
| Editor / Runtime | planned | editor embeddable in the runtime; both scriptable. `HP_BUILD_EDITOR`/`HP_BUILD_RUNTIME` are accepted but currently build nothing — targets land with the modules themselves |

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

`assets/shaders/**.glsl` (or `.hlsl` when `HP_SHADER_PROFILE=HLSL`) are compiled to SPIR-V at build time
by the `HollowpointShaderC` tool (glslang + SPIRV-Tools optimizer) and emitted into the build tree.
Generated SPIR-V is never committed. Specialization constants carry per-material parameters so one
compiled pipeline can serve many materials.

## Roadmap notes / open questions

1. **ECS storage layout** — sparse set vs archetype (EnTT-style). Leaning sparse set: simpler, stable
   references, and our component counts are modest; revisit when the renderer needs 100k entities.
2. **Asset cooking** — cook at import time into a content-addressed store (fast loads, incremental),
   versus cook-on-build in the editor. Not decided; both need the same GUID database first.
3. **Editor UI** — Dear ImGui + implot for profiling overlays; docking branch. The editor must be fully
   scriptable, which pushes us to define the command/console API before building widgets.
