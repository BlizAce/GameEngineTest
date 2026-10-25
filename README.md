# Hollowpoint

A production-grade 3D game engine for Windows, macOS and Linux. C++20, CMake, Vulkan (via
[NVRHI](https://github.com/NVIDIA-RTX/NVRHI)) on all three platforms, GLFW windowing, an own math
library, Lua scripting, Jolt physics, miniaudio audio.

Hollowpoint **runs**: `hollowpoint-runtime` opens a window, loads a `.hpscene`, ticks Lua behaviours, renders a forward HDR
pass, tone-maps and presents. The foundation layers are complete and tested; assets (glTF/textures), physics/audio facing
code and the editor are what is still landing. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for what exists today,
module by module, and [`AGENTS.md`](AGENTS.md) for the contract anyone changing this repository works under — code style,
testing rules, layering, git workflow.

## Building

Prerequisites: CMake ≥ 3.25, Ninja, a C++20 compiler (GCC or clang), Git, and a Vulkan loader with
any Vulkan 1.3 device for the GPU test suites and to run the engine at all. Every third-party
dependency is pinned and fetched automatically — there is nothing to install by hand.

```bash
cmake --preset linux-debug        # configure (see CMakePresets.json for all presets)
cmake --build build/debug -j      # build engine, tests, and the runtime executable
ctest --test-dir build/debug      # run the whole suite
```

GPU tests skip themselves with a clear message on machines without a Vulkan device; the windowing
suite runs headless via GLFW's Null backend. The GPU suites need a **Vulkan 1.3** device — NVRHI's
backend calls `vkCmdPipelineBarrier2` and dynamic rendering unconditionally, so 1.2 is not enough
however it reports itself.

## Running the engine

```bash
./build/debug/output/hollowpoint-runtime \
    --scene assets/scenes/demo.hpscene \
    --script-dir assets/demos/scripts \
    --frames 60 --screenshot /tmp/hollowpoint.png
```

That is the whole frame loop — window, device, swapchain, frame ring, forward HDR pass, tone map,
present — with the scene's entities collected from ECS each frame and any `Script` component's Lua
behaviour ticked before transforms. `--help` lists the options; `--frames N` plus `--screenshot` is
how an unattended run produces something reviewable (`scripts/png_probe.py out.png` summarises a
capture from a terminal). Debug builds run with Vulkan validation on when the layers are installed.

The full quality gate before any commit (format check, build, tests, and an ASan/UBSan pass):

```bash
scripts/run_tests.sh --asan
```

## Layout

| Path | Contents |
|------|----------|
| `engine/` | One directory per module (`Core`, `Math`, `ECS`, `Scene`, `Graphics`, `Renderer`, `Game`, `Runtime`, …), each a static library with public headers under `Include/Hollowpoint/<Module>/` |
| `assets/` | Engine content: `.hpscene` files and the demo project's Lua scripts |
| `tests/` | doctest suites, one executable per module, registered with CTest |
| `docs/` | Architecture and per-module design notes |
| `notes/` | Development log: decisions, measured findings, bugs found by tests |
| `scripts/` | Format gate, test driver, capture-inspection helper |
| `third_party/` | Pinned dependency declarations (fetched, never vendored sources) |

## Status

Complete and tested: Core, Math, ECS, Reflection, Serialization, Scene (transforms, prefabs,
`.hpscene` I/O), Platform (windowing/input), Scripting (sandboxed Lua VM + engine API). Landing with
a working vertical slice through every one of them: Graphics (device, resources, swapchain, frame
ring) and the Renderer's forward HDR + tone-map passes, plus `Game` (scene → render-plan bridge and
the production `SceneRenderer`) and `Runtime` (application shell, frame loop, executable). The CPU
shading reference those GPU passes validate against (PBR, lighting, camera, tone mapping, IBL) is
complete and numerically tested, including GPU-vs-CPU equivalence on real draws.

Not built yet: the asset database (meshes come from built-in primitives behind a resolver seam; glTF
and texture loading land with it), physics and audio facing code, input reaching gameplay, and the
editor — all of which are consumers of what is here rather than prerequisites for it.
