#pragma once

#include <limits>
#include <string>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Game/RenderPrimitives.h"
#include "Hollowpoint/Math/MathCore.h"

// Forward declarations at Hollowpoint scope, before Game opens. Declaring them *inside* namespace Hollowpoint::Game
// would introduce Hollowpoint::Game::Graphics and shadow the real one, breaking every `Graphics::` lookup below with an
// error that names neither the mistake nor the fix. A reference parameter needs a name rather than a header (the
// convention Buffer.h establishes in this layer), so consumers that only build plans never pay for GraphicsDevice.h or
// Texture.h.
namespace Hollowpoint::Graphics
{
class GraphicsDevice;
class RenderTarget;
class Texture;
}  // namespace Hollowpoint::Graphics

// ---------------------------------------------------------------------------
// The production GPU scene renderer: a RenderPlan in, pixels out.
//
// Why this lives here and not in Renderer. Renderer links neither Graphics nor ECS on purpose — that is what lets its
// CPU/GPU shading equivalence tests run with no device and full leak checking (see engine/Renderer/CMakeLists.txt).
// Game is the module already sitting above both, holding the scene → renderer bridge; consuming a plan with an actual
// RHI belongs beside producing one.
//
// What existed before: every line of GPU submission here lived in tests/Renderer/TestSceneToPixel.cpp. That file proved
// the engine's shaders, uniform layouts and upload offsets reproduce the CPU reference's pixels — and then threw the
// code away. Shipping an engine whose only renderer is a test helper is how "works in the test, black in the app" bugs
// happen: the application ends up re-deriving buffer sizes, binding orders and clear sequences that were proven once.
// This class is that code turned into an API with the contract the test asserted — so its tests now exercise production
// code, not a copy of it.
//
// Command recording goes through NVRHI directly (RhiAccess.h in the .cpp), which is Graphics' documented escape hatch:
// wrapping ~50 record calls would add indirection without adding safety (see Buffer.h). What this class owns instead is
// everything expensive that must not repeat per frame — compiled pipelines, binding layouts, constant buffers, GPU
// geometry copies — because those are exactly what a naive implementation puts on the hot path.
//
// Frame slots: every draw call takes a slot index into per-slot constant-buffer state (SceneRendererDesc::FrameSlots).
// That is what makes Graphics::FrameRing mean something. The ring bounds how many frames may be in flight; its purpose
// is preventing frame N+2 from overwriting data frame N's draws are still reading — a hazard with no error, no report
// on a host without validation layers, and no symptom but flicker. A ring cannot protect buffers every slot shares, so
// each slot owns its own per-frame state and callers pass the slot they acquired. Slot 0 everywhere is correct for a
// caller that waits between frames, which is why it is the default.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Game
{

using Math::Float32;
using Math::Vec4;

class SceneRendererImpl;

//! Creation parameters.
struct SceneRendererDesc
{
    std::string Name = "scene-renderer";

    //! Light-array bound baked into the generated fragment shader and its constant buffers. A draw referencing a light
    //! index at or beyond this is rejected (logged) rather than reading undefined array elements — the shader guards
    //! `index < count` too, so without an explicit check the failure would be silent: an object simply missing a lamp.
    Core::UInt32 MaxLightsPerDraw = 8;

    //! How many frames' worth of per-frame constant buffers to keep, indexed by the slot argument of every call.
    //!
    //! This is the number that decides whether frame N+2 can overwrite data the GPU is still reading for frame N. A
    //! renderer
    //  whose per-frame buffers are shared between slots makes a Graphics::FrameRing decorative: the ring bounds how
    //  many frames may be in flight while every slot writes the same bytes, which is precisely the hazard it was
    //  written to prevent (session 14 recorded that "the renderer can no longer overwrite in-flight buffers" was not
    //  yet true — this field is what makes it true). Each slot therefore owns its own frame matrices, light array,
    //  exposure and per-draw buffer pool.
    //!
    //! Default 1 is correct — and cheapest — for a caller that idles between frames (a test, an asset tool, a
    //! screenshot
    //  pass). An application driving a FrameRing sets this to the ring's slot count and passes its current slot.
    Core::UInt32 FrameSlots = 1;
};

//! Renders RenderPlans with the engine's generated forward and tonemap shaders.
/**
 * Thread model: single-threaded, called from whichever thread records the frame. The caches below are unsynchronised,
 * matching NVRHI's per-command-list state tracking.
 *
 * Lifetime: owns only what it created (pipelines, buffers, binding sets). Targets and plans are borrowed per call. The
 * GraphicsDevice must outlive this object.
 */
class HP_GAME_API SceneRenderer : public Core::RefCounted
{
public:
    //! Compiles the engine's shaders and builds pipeline layouts. Returns nullptr on failure with the reason logged;
    //! expected failures are shader-compilation errors (a generator regression, or a driver rejecting the produced
    //! SPIR-V) and a device that cannot create pipelines.
    [[nodiscard]] static Core::Ref<SceneRenderer> Create(Graphics::GraphicsDevice& device,
                                                         const SceneRendererDesc& desc = {});

    ~SceneRenderer() override;

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // --- geometry cache ------------------------------------------------------

    //! True when this renderer already holds a GPU copy of `geometry`. The cache is keyed by address, so an asset layer
    //! must call ReleaseGeometry when it frees or reloads mesh data — see the contract in the .cpp.
    [[nodiscard]] bool HasGeometry(const Renderer::GeometryData& geometry) const noexcept;

    //! Drops the cached GPU copy for `geometry` so the next draw uploads again. Call from an asset layer's unload or
    //! reload path; cheap and safe when nothing was cached. Does not disturb draws in flight — NVRHI defers the actual
    //! free until the submissions referencing those buffers have finished.
    void ReleaseGeometry(const Renderer::GeometryData& geometry) noexcept;

    //! Resident geometry uploads, for tests and a memory HUD.
    [[nodiscard]] Core::UInt32 GetCachedGeometryCount() const noexcept;

    // --- rendering -----------------------------------------------------------

    //! Draws the plan into `target`'s colour attachment (and depth, when it has one).
    /**
     * Sequence per call: write frame matrices and lights, write per-draw transforms/materials, clear colour to
     //  `backgroundColour` and depth to 1.0, then one indexed draw per mesh with that mesh's authored cull mode.
     Everything
     //  lands in ONE command-list submission, so a frame ring's completion query covers the whole pass — splitting a
     frame
     //  across submissions would let it report "finished" while half the frame was still queued.
     *
     * The clear belongs to this function rather than the caller: a render target's memory is undefined at creation, and
     //  relying on previous contents produces pixels whose value depends on allocation history (this project hit
     exactly that
     //  with LessOrEqual against garbage depth — notes/EXPERIMENTS.md session 14). `backgroundColour` defaults to NaN
     so a
     //  pixel never written reads back as NaN and fails loudly; pass a real colour for frames meant to be looked at.
     *
     * A target with no depth attachment renders with depth testing off, because the pipeline must match the target's
     formats
     //  (asking for depth on a target without one would create no pipeline). That is a legitimate mode — flat scenes,
     debug
     //  overlays — but overlapping geometry then resolves by rasterisation order, so it logs once.
     *
     * `slot` selects this frame's constant-buffer state and must be < GetFrameSlotCount(). Out of range asserts and
     refuses:
     //  guessing a clamped slot would turn a caller error into exactly the silent cross-frame corruption slots exist to
     stop.
     *
     * Returns false when nothing was submitted (shader/pipeline/buffer failure, each already logged with its cause);
     true once
     //  the command list has executed. A plan whose meshes all failed to resolve is NOT an error: zero draws are
     submitted, the
     //  clear still lands, and the frame is valid.
     */
    bool RenderForward(const RenderPlan& plan, const Graphics::RenderTarget& target, Core::UInt32 slot = 0,
                       const Vec4& backgroundColour = Vec4(std::numeric_limits<float>::quiet_NaN(),
                                                           std::numeric_limits<float>::quiet_NaN(),
                                                           std::numeric_limits<float>::quiet_NaN(), 1.0f));

    //! Tone-maps `source` (scene-linear HDR) into `destination` with the engine's one display transform.
    /**
     * `source` must have been created with IsShaderResource (it is sampled). The destination may be any target, which
     is what
     //  lets one call present an 8-bit back buffer or write an RGBA32 test target compared texel-by-texel against
     //  ColorPipeline::LinearToDisplay. No depth is used: the blit covers the viewport with a synthesised triangle, so
     it works
     //  on targets without a depth attachment (a swapchain's, after the forward pass). Returns false without submitting
     when
     //  compilation or pipeline creation fails.
     */
    bool BlitTonemap(const Graphics::Texture& source, const Graphics::RenderTarget& destination, Float32 exposure,
                     Core::UInt32 slot = 0);

    //! Forward-passes `plan` into `hdrTarget`, then tone-maps that into `displayTarget`: one frame of the engine's
    //! pipeline,
    //  so the ordering and exposure handoff exist once rather than in every application. Two submissions — a frame ring
    //  must therefore be ended after this returns, since its query covers everything queued at EndFrame time.
    bool RenderFrame(const RenderPlan& plan, const Graphics::RenderTarget& hdrTarget,
                     const Graphics::RenderTarget& displayTarget, Float32 exposure, Core::UInt32 slot = 0,
                     const Vec4& backgroundColour = Vec4(0.0f, 0.0f, 0.0f, 1.0f));

    // --- diagnostics ---------------------------------------------------------

    //! Indexed draws submitted since creation. A cheap liveness signal: a frame that renders nothing and a frame that
    //! draws nothing look identical on screen; this is the difference in a log line.
    [[nodiscard]] Core::UInt64 GetDrawCount() const noexcept;

    //! Submissions made through RenderForward / BlitTonemap since creation.
    [[nodiscard]] Core::UInt64 GetSubmissionCount() const noexcept;

    //! The light-array bound this renderer was created with.
    [[nodiscard]] Core::UInt32 GetMaxLightsPerDraw() const noexcept;

    //! Per-slot state count, i.e. one past the largest valid `slot` argument. A frame loop checks this against its
    //! Graphics::FrameRing's slot count when both are created — a mismatch means slots alias each other silently.
    [[nodiscard]] Core::UInt32 GetFrameSlotCount() const noexcept;

    // --- cache diagnostics ---------------------------------------------------
    //
    // Exposed because both caches are keyed by derived state (formats, geometry address) rather than by the obvious
    // argument, and "the cache is working" is otherwise indistinguishable from "the cache grew unboundedly and every
    // lookup missed". A resize test needs to assert the pipeline count stays flat across ten resizes; without a counter
    // the only evidence would be memory use several frames later.

    //! Forward pipelines created so far, one per (colour format, depth presence, cull mode) combination seen.
    [[nodiscard]] Core::UInt32 GetForwardVariantCount() const noexcept;

    //! Tonemap pipelines created so far, one per (colour format, depth presence) pair.
    [[nodiscard]] Core::UInt32 GetTonemapVariantCount() const noexcept;

    //! Per-draw constant-buffer entries resident in frame slot `slot`, or 0 when the slot does not exist.
    /**
     * The reason this is per-slot rather than a total: "each slot owns its own buffers" is the claim that makes a frame
     * ring meaningful, and it needs an observable consequence. Growing slot 0's pool and observing slot 1's stay empty
     * is that observation; a single aggregate count could not distinguish separate pools from one shared one, which is
     * exactly the distinction at issue.
     */
    [[nodiscard]] Core::UInt32 GetDrawPoolSize(Core::UInt32 slot) const noexcept;

private:
    SceneRenderer();

    // Nested pimpl, matching GraphicsDevice/Swapchain/FrameRing: NVRHI and Vulkan types stay in the .cpp so including
    // this header costs nothing (AGENTS.md §3).
    SceneRendererImpl* m_Impl = nullptr;  //!< Owned. Null only if construction failed.
};

}  // namespace Hollowpoint::Game
