#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"  // API macro + the UInt8/UInt32/UInt64 aliases used below

namespace Hollowpoint::Graphics
{

class GraphicsDevice;

// ---------------------------------------------------------------------------
// The frame ring: how many frames may be in flight at once, and which one the CPU may write to now.
//
// Why it exists. A renderer writes per-frame data (constant buffers, instance tables) while the GPU is still
// executing earlier frames. Vulkan's model permits exactly that — up to `maxFramesInFlight` frames ahead — but only
// if each frame uses resources the in-flight older frames do not. Without a slot discipline, frame N+3 overwrites the
// constant buffer frame N is still reading: no error, no validation report on this machine (no validation layer is
// installed), just a flickering or stale frame that appears and disappears. This class is the single place that
// decides whether writing now is safe, so callers cannot get it wrong by accident.
//
// How completion is observed. NVRHI exposes event queries — createEventQuery / setEventQuery / pollEventQuery /
// waitEventQuery / resetEventQuery — and states plainly that it inserts no synchronisation of its own, so an engine
// that never arms one simply never learns when a frame finished. This class owns exactly one query per slot: armed by
// EndFrame against the graphics queue, then polled or waited on before BeginFrame hands that slot out again. The
// semantics those calls actually have (an event query records the queue's submission id at call time; an un-armed one
// reads as never-complete) are documented where they are used in FrameRing.cpp, because two of them are surprising and
// both were found by reading NVRHI's implementation rather than its docs.
//
// Deliberate limits. This tracks *frame slots*, not individual resources: it answers "may I begin another frame" and
// "has this slot's GPU work finished", and nothing about which buffer is bound where. Per-resource hazard tracking is
// a different, much larger problem (Vulkan's own answer is pipeline barriers, which recording code already owns).
//
// Status: not yet wired into anything. There is no application frame loop in the engine — no executable target exists —
// so this class currently has tests and no production caller. That is deliberate (the ring is a Graphics-layer
// primitive and wanted its own slice), but it means "the renderer can no longer overwrite in-flight buffers" is not yet
// true: it becomes true when the loop that owns BeginFrame/EndFrame is written, and until then nothing prevents a
// caller from ignoring the ring entirely.
// ---------------------------------------------------------------------------

//! Outcome of BeginFrame. The two refusals mean different things to the caller and must not be conflated:
//! Full means every slot is held by a frame that has not called EndFrame — the caller's own accounting is behind, and
//! no amount of waiting will help. Waiting means a slot is free on the CPU but its previous frame is still executing
//! on the GPU (only reported when the caller declined to block), so skipping this frame or waiting are both valid.
enum class FrameQueueStatus : UInt8
{
    Began,    //!< A slot was acquired; it is safe to write this frame's per-frame data.
    Waiting,  //!< The next slot's previous frame is still on the GPU. Skip this frame or call again with waiting on.
    Full,     //!< Every slot is acquired and not yet ended: more frames begun than the ring can hold.
};

//! One step of the frame loop. Not thread-safe for BeginFrame/EndFrame from multiple threads: a frame ring models
//! one submission stream, matching Vulkan's queue semantics.
class HP_GRAPHICS_API FrameRing : public Core::RefCounted
{
public:
    //! Creates a ring for `device`. `slots` must be >= 1; the engine default is 3, which is what lets the CPU run two
    //! frames ahead of the GPU while vsync paces the loop. Returns nullptr on failure (no device).
    [[nodiscard]] static Core::Ref<FrameRing> Create(GraphicsDevice& device, UInt32 slots = kDefaultSlots);

    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    //! Acquires the next slot. When `waitForFree` is true a busy slot is waited on (blocking) — the behaviour an
    //! application wants, since it would otherwise have nothing to do. With false this returns Waiting instead of
    //! blocking, which is what tests and a "skip this frame" loop policy need.
    //!
    //! Returns Full once GetSlotCount() frames have been acquired without ending. Over-submission is rejected rather
    //! than absorbed by silently reusing a slot whose frame is still in flight — that reuse is precisely the
    //! overwrite-the-GPU hazard this class exists to prevent.
    [[nodiscard]] FrameQueueStatus BeginFrame(bool waitForFree = true) noexcept;

    //! Ends the frame in `slot` and arms its completion query, which is what makes a later BeginFrame wait for this
    //! frame's GPU work before handing the slot out again.
    //!
    //! Call it after the frame's command lists have been submitted: completion is recorded against everything queued so
    //! far, so calling it earlier reports the frame finished sooner than it did. Returns false when `slot` was not
    //! acquired or is out of range (the ring is left untouched); true once the slot is tracked.
    bool EndFrame(UInt32 slot) noexcept;

    //! True once the GPU work submitted for `slot` has finished, or when nothing is outstanding for it (a fresh slot,
    //! or one whose frame ended without arming anything). Reporting true in that second case matters: NVRHI's
    //! pollEventQuery answers false for a query that was never armed, so a blind poll would call an idle slot busy.
    [[nodiscard]] bool IsSlotComplete(UInt32 slot) noexcept;

    //! Blocks until `slot`'s GPU work finishes, with a timeout. Returns false on timeout — reported rather than
    //! absorbed, because proceeding past it means writing memory the GPU may still be reading.
    bool WaitForSlot(UInt32 slot, UInt64 timeoutNanoseconds) noexcept;

    [[nodiscard]] UInt32 GetSlotCount() const noexcept;

    //! Slots held by the CPU right now: begun and not yet ended. Reaching GetSlotCount() means the next BeginFrame
    //! returns Full. A slot that has ended but whose GPU work is still running is *not* counted — it is free for the
    //! CPU and blocked only for reuse, which is a different question.
    [[nodiscard]] UInt32 GetOutstandingCount() const noexcept;

    //! The slot most recently handed out, or kNoSlot before the first BeginFrame. Convenience for a frame loop that
    //! threads the index through its renderers.
    [[nodiscard]] UInt32 GetCurrentSlot() const noexcept;

    static constexpr UInt32 kDefaultSlots = 3;
    static constexpr UInt32 kNoSlot = 0xFFFFFFFFu;

private:
    FrameRing() = default;

    // Nested pimpl, matching Swapchain: the NVRHI event-query handles stay in Src/FrameRing.cpp so this header never
    // pulls in RhiAccess.h.
    struct Impl;
    Impl* m_Impl = nullptr;
};

}  // namespace Hollowpoint::Graphics
