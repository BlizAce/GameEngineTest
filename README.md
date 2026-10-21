# Hollowpoint

A production-grade 3D game engine for Windows, macOS and Linux. C++20, CMake, Vulkan (via
[NVRHI](https://github.com/NVIDIA-RTX/NVRHI)) on all three platforms, GLFW windowing, glm math, Lua
scripting, Jolt physics, miniaudio audio.

Hollowpoint is under active development: the foundation layers are complete and tested, the runtime
layers (graphics resources, renderer passes, assets, editor) are landing. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for what exists today, module by module, and
[`AGENTS.md`](AGENTS.md) for the contract anyone changing this repository works under — code style,
testing rules, layering, git workflow.

## Building

Prerequisites: CMake ≥ 3.25, Ninja, a C++20 compiler (GCC or clang), Git, and a Vulkan loader with
any Vulkan 1.2 device for the GPU test suites. Every third-party dependency is pinned and fetched
automatically — there is nothing to install by hand.

```bash
cmake --preset linux-debug        # configure (see CMakePresets.json for all presets)
cmake --build build/debug -j      # build engine + tests
ctest --test-dir build/debug      # run the whole suite
```

GPU tests skip themselves with a clear message on machines without a Vulkan device; the windowing
suite runs headless via GLFW's Null backend.

The full quality gate before any commit (format check, build, tests, and an ASan/UBSan pass):

```bash
scripts/run_tests.sh --asan
```

## Layout

| Path | Contents |
|------|----------|
| `engine/` | One directory per module (`Core`, `Math`, `ECS`, `Scene`, `Graphics`, `Renderer`, …), each a static library with public headers under `Include/Hollowpoint/<Module>/` |
| `tests/` | doctest suites, one executable per module, registered with CTest |
| `docs/` | Architecture and per-module design notes |
| `notes/` | Development log: decisions, measured findings, bugs found by tests |
| `scripts/` | Format gate and test driver |
| `third_party/` | Pinned dependency declarations (fetched, never vendored sources) |

## Status

Complete and tested: Core, Math, ECS, Reflection, Serialization, Scene (transforms, prefabs,
`.hpscene` I/O), Platform (windowing/input), Scripting (sandboxed Lua VM + engine API).
In flight: Graphics resource layer and the Renderer's GPU passes — the CPU shading reference they
validate against (PBR, lighting, camera, tone mapping, IBL) is complete and numerically tested,
including GPU-vs-CPU equivalence on real draws.
