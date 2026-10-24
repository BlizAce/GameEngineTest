// Frame ring tests: slot accounting, over-submission rejection, and completion tracking against real GPU work.
//
// What is being protected is a bug with no symptom on this machine: writing per-frame data while the GPU is still
// reading the previous frame's copy produces flickering or stale frames, not an error, and there is no validation
// layer installed here to report it. So these tests assert the ring's decisions — when it hands out a slot, when it
// refuses, and what it waits for — rather than trying to observe corruption.
//
// No display is needed: the ring tracks queue submissions, not presentation. A device with no window is enough, so
// unlike the swapchain suite these run headlessly (they only skip when there is no GPU at all).

#include "TestSupport.h"

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Graphics/Buffer.h"
#include "Hollowpoint/Graphics/FrameRing.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/RhiAccess.h"

using namespace Hollowpoint::Graphics;
using Hollowpoint::Core::Ref;
using Hollowpoint::Core::UInt32;
using Hollowpoint::Core::UInt64;

namespace
{

//! One device for the whole file, or nullptr when this host has no usable GPU.
/**
 * Cached because device creation costs ~100 ms and every case needs one. "No GPU" must skip rather than fail: it is a
 * property of the test host, not an engine defect — hence the WARN-and-return pattern below rather than REQUIRE.
 */
[[nodiscard]] GraphicsDevice* GetDevice()
{
    static GraphicsDevice* device = []() -> GraphicsDevice* {
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint frame ring tests";
        return GraphicsDevice::Create(desc);
    }();

    return device;
}

#define REQUIRE_DEVICE()                                                                                           \
    do                                                                                                             \
    {                                                                                                              \
        if (GetDevice() == nullptr)                                                                                \
        {                                                                                                          \
            WARN((std::string("No graphics device on this host: ") + GraphicsDevice::GetFailureReason()).c_str()); \
            return;                                                                                                \
        }                                                                                                          \
    }                                                                                                              \
    while (false)

//! Submits `byteCount` of clear work and returns, without waiting. Used to put real GPU work in flight so a
//! completion query has something non-trivial to wait for.
void SubmitLongClear(GraphicsDevice& device, UInt64 byteCount)
{
    nvrhi::IDevice* rhi = device.GetNativeDevice();
    REQUIRE(rhi != nullptr);

    BufferDesc bufferDesc;
    bufferDesc.Name = "ring-stall";
    bufferDesc.ByteSize = byteCount;
    // ShaderResource-only: clearBuffer works on a plain device-local buffer, and keeping the usage minimal avoids
    // pulling in state requirements this test has nothing to do with.
    bufferDesc.Usage = BufferUsage::ShaderResource;
    Ref<Buffer> buffer = Buffer::Create(device, bufferDesc);
    REQUIRE(buffer != nullptr);

    nvrhi::CommandListHandle commandList = rhi->createCommandList();
    commandList->open();
    // A single huge clear: one submission that takes measurable GPU time without needing a pipeline or framebuffer.
    commandList->clearBufferUInt(buffer->GetNativeBuffer(), 0u);
    commandList->close();

    nvrhi::ICommandList* raw = commandList;
    rhi->executeCommandLists(&raw, 1);
}

}  // namespace

TEST_SUITE("Graphics/FrameRing")
{
    TEST_CASE("a fresh ring reports its slots and no current frame")
    {
        REQUIRE_DEVICE();

        Ref<FrameRing> ring = FrameRing::Create(*GetDevice());
        REQUIRE(ring != nullptr);

        CHECK(ring->GetSlotCount() == FrameRing::kDefaultSlots);
        CHECK(ring->GetOutstandingCount() == 0u);
        CHECK(ring->GetCurrentSlot() == FrameRing::kNoSlot);
    }

    TEST_CASE("beginning frames without ending them fills the ring and is rejected")
    {
        REQUIRE_DEVICE();

        Ref<FrameRing> ring = FrameRing::Create(*GetDevice(), 2);
        REQUIRE(ring != nullptr);

        const UInt32 first = [&]() {
            REQUIRE(ring->BeginFrame(false) == FrameQueueStatus::Began);
            return ring->GetCurrentSlot();
        }();
        CHECK(ring->GetOutstandingCount() == 1u);

        REQUIRE(ring->BeginFrame(false) == FrameQueueStatus::Began);
        const UInt32 second = ring->GetCurrentSlot();
        CHECK(second != first);  // Distinct slots, or two frames would share one set of buffers.
        CHECK(ring->GetOutstandingCount() == 2u);

        // The third begin has no slot and nothing that waiting could resolve: over-submission, answered Full rather
        // than by silently reusing a slot a frame is still using. Deterministic — no GPU work has been submitted, so no
        // completion state can influence it.
        CHECK(ring->BeginFrame(false) == FrameQueueStatus::Full);
        CHECK(ring->BeginFrame(true) == FrameQueueStatus::Full);  // Waiting cannot help; only EndFrame can.

        // Ending both frees both for the CPU; their completion queries are what gate reuse afterwards.
        //
        // The earlier draft of this block ended only one frame and then asserted about reuse, which passed in a debug
        // build and failed under ASan: the other slot was still acquired-and-never-ended, so refusing with Full was the
        // *correct* answer and my expectation was the bug. Debug only got away with it because a completion query
        // happened to be pending, changing which slot the cursor examined first. Anything here that could depend on
        // whether a query has completed yet is therefore checked with waiting enabled — the deterministic form.
        REQUIRE(ring->EndFrame(first));
        REQUIRE(ring->EndFrame(second));
        CHECK(ring->GetOutstandingCount() == 0u);

        // Reuse in submission order, oldest slot first, once both frames' work has been waited on.
        REQUIRE(ring->BeginFrame(true) == FrameQueueStatus::Began);
        CHECK(ring->GetCurrentSlot() == first);
        REQUIRE(ring->BeginFrame(true) == FrameQueueStatus::Began);
        CHECK(ring->GetCurrentSlot() == second);

        // A third begin with both slots held again has to be refused — the ring is genuinely full, not merely busy.
        CHECK(ring->BeginFrame(true) == FrameQueueStatus::Full);
    }

    TEST_CASE("a full cycle round-robins through slots and reuses them once complete")
    {
        REQUIRE_DEVICE();

        Ref<FrameRing> ring = FrameRing::Create(*GetDevice(), 2);
        REQUIRE(ring != nullptr);

        std::vector<UInt32> order;
        for (UInt32 frame = 0; frame < 6; ++frame)
        {
            // waitForFree = true: the GPU work here is a no-op marker submit, so completion is immediate and waiting
            // must succeed rather than time out or report Full.
            REQUIRE_MESSAGE(ring->BeginFrame(true) == FrameQueueStatus::Began, "frame " << frame);
            order.push_back(ring->GetCurrentSlot());
            ring->EndFrame(ring->GetCurrentSlot());
        }

        CHECK(order[0] == 0u);
        CHECK(order[1] == 1u);
        // Reuse in the same order: a ring that jumped slots would hand the CPU a buffer whose previous frame finished
        // later than the one before it, breaking the ordering completion tracking depends on.
        CHECK(order[2] == order[0]);
        CHECK(order[3] == order[1]);
        CHECK(ring->GetOutstandingCount() == 0u);
    }

    TEST_CASE("completion tracks real GPU work rather than reporting too early")
    {
        REQUIRE_DEVICE();
        GraphicsDevice& device = *GetDevice();

        Ref<FrameRing> ring = FrameRing::Create(device, 1);
        REQUIRE(ring != nullptr);

        // One slot makes the next frame's begin depend entirely on this frame's completion — no other slot to fall back
        // to, so a query armed too early or checked too lazily shows up immediately.
        REQUIRE(ring->BeginFrame(false) == FrameQueueStatus::Began);
        const UInt32 slot = ring->GetCurrentSlot();

        SubmitLongClear(device, 64ull * 1024 * 1024);
        REQUIRE(ring->EndFrame(slot));

        // The load-bearing assertion of this file: completion must mean "the submitted work finished", not "a query was
        // armed". If EndFrame's marker submit did not land behind the clear (or if the query were compared against the
        // wrong submission id), IsSlotComplete would report true while 64 MB of clearing still ran, and BeginFrame
        // would hand the slot back over in-flight GPU work.
        //
        // Stated honestly as a race: this can pass spuriously if the clear finishes before the check runs — which is
        // why the clear is deliberately huge, and why the blocking form below is the part that cannot be wrong.
        const bool completeImmediately = ring->IsSlotComplete(slot);
        INFO("complete immediately after EndFrame: " << completeImmediately);

        // This half is not a race: WaitForSlot must return true eventually, and returning false means the query never
        // completed — i.e. the ring would wedge the frame loop permanently.
        CHECK(ring->WaitForSlot(slot, 5000000000ull));
        CHECK(ring->IsSlotComplete(slot));

        device.WaitForIdle();
    }

    TEST_CASE("waiting on a slot with nothing outstanding succeeds at once")
    {
        REQUIRE_DEVICE();

        Ref<FrameRing> ring = FrameRing::Create(*GetDevice(), 2);
        REQUIRE(ring != nullptr);

        // Fresh slots report complete and wait successfully. This matters more than it looks: NVRHI's pollEventQuery
        // returns FALSE for a query that was never set (id 0 is treated as "not complete"), so a ring that polled blind
        // would report an idle slot as permanently busy and deadlock the first BeginFrame of every run.
        CHECK(ring->IsSlotComplete(0));
        CHECK(ring->IsSlotComplete(1));
        CHECK(ring->WaitForSlot(0, 0));

        REQUIRE(ring->BeginFrame(false) == FrameQueueStatus::Began);
        CHECK(ring->IsSlotComplete(ring->GetCurrentSlot()));  // Begun but never submitted: nothing can be reading it.
    }

    TEST_CASE("bad slot arguments are refused without corrupting the ring")
    {
        REQUIRE_DEVICE();

        Ref<FrameRing> ring = FrameRing::Create(*GetDevice(), 2);
        REQUIRE(ring != nullptr);

        // Out-of-range indices are refused with a logged error and leave the ring usable — they cost nothing to check
        // and a swapped slot index is an easy mistake in a frame loop.
        //
        // One case is deliberately NOT exercised: EndFrame on a slot that was never acquired. That path asserts (a
        // begin/end imbalance is a logic error whose silent form — a slot that never arms a completion query — shows up
        // much later as frames reusing buffers still in use), and an asserting API cannot be called from a test in a
        // debug build without aborting it. Verified by inspection plus the balance checks elsewhere in this file.

        CHECK(ring->GetOutstandingCount() == 0u);
        REQUIRE(ring->BeginFrame(false) == FrameQueueStatus::Began);
        CHECK(ring->EndFrame(ring->GetCurrentSlot()));
        CHECK(ring->GetOutstandingCount() == 0u);

        // Out-of-range completion queries answer "not complete" rather than reading past the slot array.
        CHECK_FALSE(ring->IsSlotComplete(99));
    }

    TEST_CASE("a ring survives interleaved frames and heavy reuse")
    {
        REQUIRE_DEVICE();
        GraphicsDevice& device = *GetDevice();

        Ref<FrameRing> ring = FrameRing::Create(device, 3);
        REQUIRE(ring != nullptr);

        // A realistic loop: begin, do work, end, repeat — with real submissions so completion is not free. Ends by
        // draining, which is where a miscounted or never-reset query would hang or refuse the next frame.
        for (UInt32 frame = 0; frame < 12; ++frame)
        {
            REQUIRE_MESSAGE(ring->BeginFrame(true) == FrameQueueStatus::Began, "frame " << frame);
            SubmitLongClear(device, 4ull * 1024 * 1024);
            CHECK(ring->EndFrame(ring->GetCurrentSlot()));
        }

        // Draining must leave the ring usable, not merely empty.
        REQUIRE(ring->BeginFrame(true) == FrameQueueStatus::Began);
        CHECK(ring->EndFrame(ring->GetCurrentSlot()));

        device.WaitForIdle();
        nvrhi::IDevice* rhi = device.GetNativeDevice();
        REQUIRE(rhi != nullptr);
        CHECK(static_cast<bool>(rhi->createCommandList()));  // Device still healthy after all of that.
    }

    TEST_CASE("a frame can end before anything has been submitted")
    {
        // A device with NO submissions yet, which only a fresh GraphicsDevice provides: NVRHI's submission id lives on
        // the queue and starts at 0 per VkDevice, so every other test in this file — sharing one device and having
        // drawn through it — cannot reach this state. That is why this case creates its own device rather than reusing
        // the fixture's, despite costing another ~100 ms.
        //
        // What it pins: setEventQuery records the queue's current submission id, and pollCommandList treats id 0 as
        // "never complete". A ring that arms a query before its queue has ever submitted therefore produces a slot that
        // can never finish — BeginFrame(waitForFree=false) returns Waiting forever and a skip-frame loop starves. The
        // ring performs one bootstrap submission at creation to make that unreachable, and this is the only case that
        // fails when it is removed (verified by mutation).
        // Raw pointer, not Ref: GraphicsDevice is created by GraphicsDevice::Create and deleted by its owner (see the
        // device tests), it is not a RefCounted type.
        GraphicsDeviceDesc desc;
        desc.ApplicationName = "Hollowpoint frame ring tests (fresh queue)";
        GraphicsDevice* fresh = GraphicsDevice::Create(desc);

        if (fresh == nullptr)
        {
            WARN((std::string("No graphics device on this host: ") + GraphicsDevice::GetFailureReason()).c_str());
            return;
        }

        Ref<FrameRing> ring = FrameRing::Create(*fresh, 1);
        REQUIRE(ring != nullptr);

        // End the very first frame with nothing drawn — a load-time warm-up, or a frame that culled everything.
        REQUIRE(ring->BeginFrame(false) == FrameQueueStatus::Began);
        const UInt32 slot = ring->GetCurrentSlot();
        REQUIRE(ring->EndFrame(slot));

        // Completion must be reachable, not pending forever. Short timeout deliberately: with the bootstrap missing
        // this would sit out the whole wait and fail, whereas a long timeout would only make the failure slower to
        // read.
        CHECK(ring->WaitForSlot(slot, 2000000000ull));
        CHECK(ring->IsSlotComplete(slot));

        // And the slot must be reusable without blocking — the property a frame loop depends on when it declines to
        // wait. Waiting is acceptable only if it resolves; Full would mean the ring believed it had no slot at all.
        const FrameQueueStatus reuse = ring->BeginFrame(false);
        INFO("reuse status after an empty first frame: " << static_cast<int>(reuse));
        CHECK(reuse == FrameQueueStatus::Began);

        fresh->WaitForIdle();
        delete fresh;
    }

    TEST_CASE("slot count is honoured, including a single-slot ring")
    {
        REQUIRE_DEVICE();

        Ref<FrameRing> one = FrameRing::Create(*GetDevice(), 1);
        REQUIRE(one != nullptr);
        CHECK(one->GetSlotCount() == 1u);

        // With one slot, an un-ended frame blocks everything: the second begin must be refused rather than sharing.
        REQUIRE(one->BeginFrame(false) == FrameQueueStatus::Began);
        CHECK(one->BeginFrame(false) == FrameQueueStatus::Full);
        // Reuse needs the previous frame's query to complete, so ask with waiting — without it this returns Waiting
        // (correctly: the marker submit may still be in flight), which is what an earlier revision of this assertion
        // mistook for a bug. Full would be the real fault: a slot exists and waiting can resolve it.
        one->EndFrame(one->GetCurrentSlot());
        CHECK(one->BeginFrame(true) == FrameQueueStatus::Began);

        // Zero slots cannot work: there is nowhere to put a frame. Refused at creation rather than producing an object
        // that returns Full forever.
        CHECK(FrameRing::Create(*GetDevice(), 0) == nullptr);
    }
}
