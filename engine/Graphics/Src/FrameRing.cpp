#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/FrameRing.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/RhiAccess.h"

// ---------------------------------------------------------------------------
// Semantics of NVRHI's event queries, read from src/vulkan/vulkan-queries.cpp and vulkan-queue.cpp rather than
// assumed — each one shapes the code below:
//
//  * setEventQuery(query, queue) records `m_Queues[queue]->getLastSubmittedID()`: the id of work ALREADY submitted at
//    the moment of the call. It is a snapshot, not "signal when the next thing finishes". So it must be called after
//    the frame's command lists are submitted — which is why the engine's contract on EndFrame says so explicitly.
//  * A query may only be set while its recorded id is 0 (`assert(query->commandListID == 0)`). In release builds that
//    assert is gone and a second set silently re-targets the query to a newer submission, which would report an older
//    frame as complete. Hence the explicit Submitted guard in EndFrame: it cannot rely on the assert.
//  * resetEventQuery sets the id back to 0, and pollCommandList(0) returns FALSE — an unset query reads as "not
//    complete", not "nothing pending". Any caller polling a slot that was never submitted would therefore wait forever,
//    which is why IsSlotComplete/WaitForSlot answer from the Submitted flag instead of polling blindly.
//  * Completion itself is a timeline semaphore NVRHI maintains per queue (waitSemaphores on the recorded id), so no
//    raw fence handling is needed here — and waitEventQuery has no timeout parameter, which is why WaitForSlot polls
//    with its own deadline instead of calling it.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

namespace
{

//! Gap between completion checks in WaitForSlot. Only reached when the query stays pending, so this bounds how often we
//! ask — it does not bound the frame, and the caller's timeout decides that.
constexpr UInt64 kPollIntervalNanoseconds = 100000ull;  // 0.1 ms

}  // namespace

struct FrameRing::Impl
{
    struct Slot
    {
        //! Completion of this slot's submitted work, or unset (id 0) when nothing is outstanding from it.
        nvrhi::EventQueryHandle Query;

        bool Acquired = false;         //!< Handed to the CPU by BeginFrame and not yet returned by EndFrame.
        bool HasPendingQuery = false;  //!< A query has been set and not yet reset: must not be set again.
    };

    nvrhi::IDevice* Rhi = nullptr;  //!< Owned by the GraphicsDevice, which must outlive this ring.

    std::vector<Slot> Slots;
    UInt32 NextIndex = 0;  //!< Round-robin cursor; the next slot to try.
    UInt32 Current = FrameRing::kNoSlot;

    //! Clears a slot for reuse: resets its query (required before it can be set again) and marks it free.
    void Release(UInt32 index)
    {
        Slot& slot = Slots[index];
        if (slot.HasPendingQuery)
        {
            Rhi->resetEventQuery(slot.Query);
            slot.HasPendingQuery = false;
        }
        slot.Acquired = false;
    }
};

Core::Ref<FrameRing> FrameRing::Create(GraphicsDevice& device, UInt32 slots)
{
    if (slots < 1u)
    {
        HP_LOG_ERROR("GRAPHICS", "FrameRing: slot count must be >= 1 (got {})", slots);
        return nullptr;
    }

    nvrhi::IDevice* rhi = device.GetNativeDevice();
    if (rhi == nullptr)
    {
        HP_LOG_ERROR("GRAPHICS", "FrameRing: device has no RHI object, cannot create event queries");
        return nullptr;
    }

    Core::Ref<FrameRing> ring(new FrameRing());
    ring->m_Impl = new Impl();
    ring->m_Impl->Rhi = rhi;
    ring->m_Impl->Slots.resize(slots);


    // Bootstrap the graphics queue's submission id to at least 1.
    //
    // setEventQuery records `getLastSubmittedID()`, and on a queue that has never submitted anything that id is 0 —
    // while pollCommandList(0) returns false *permanently* (its `commandListID == 0` early-out). A query armed with id
    // 0 therefore wedges its slot for the life of the ring: BeginFrame would wait on a condition that can never become
    // true. That is not hypothetical — ending a frame before anything has been drawn is what a load-time warm-up, a
    // fully-culled frame, or any of this file's tests do.
    //
    // Doing it once here rather than in EndFrame is the difference between one empty submission per ring and one per
    // frame: after this, the id is never 0 again, so an idle EndFrame legitimately resolves against older work instead
    // of needing a fresh submission every time. Queue::submit increments the id and signals NVRHI's tracking timeline
    // semaphore even when handed zero command buffers (verified in vulkan-queue.cpp), so this produces a real,
    // completable submission rather than a no-op.
    {
        nvrhi::CommandListHandle bootstrap = rhi->createCommandList();
        if (!bootstrap)
        {
            HP_LOG_ERROR("GRAPHICS", "FrameRing: could not create the bootstrap command list");
            delete ring->m_Impl;
            ring->m_Impl = nullptr;
            return nullptr;
        }

        bootstrap->open();
        bootstrap->close();
        nvrhi::ICommandList* raw = bootstrap;
        if (rhi->executeCommandLists(&raw, 1) == 0)
        {
            // Id 0 back from a completed submit would mean the queue does not track submissions the way the event-query
            // path requires. Refusing to build a ring here beats building one whose slots never complete.
            HP_LOG_ERROR("GRAPHICS", "FrameRing: the graphics queue reported submission id 0 after submitting");
            delete ring->m_Impl;
            ring->m_Impl = nullptr;
            return nullptr;
        }
    }

    // Queries up front rather than lazily: creating one inside BeginFrame would be a per-frame driver allocation, and
    // a failure here is actionable while the same failure mid-frame is not.
    for (Impl::Slot& slot : ring->m_Impl->Slots)
    {
        slot.Query = rhi->createEventQuery();
        if (!slot.Query)
        {
            HP_LOG_ERROR("GRAPHICS", "FrameRing: could not create an event query");
            delete ring->m_Impl;
            ring->m_Impl = nullptr;
            return nullptr;
        }
    }

    return ring;
}

FrameRing::~FrameRing()
{
    delete m_Impl;
}

FrameQueueStatus FrameRing::BeginFrame(bool waitForFree) noexcept
{
    if (m_Impl == nullptr || m_Impl->Slots.empty())
    {
        return FrameQueueStatus::Full;
    }

    Impl& impl = *m_Impl;
    const UInt32 slotCount = static_cast<UInt32>(impl.Slots.size());

    // Round-robin, oldest first. Two independent conditions can block a slot, and they need different answers:
    //
    //  * it still has an uncompleted query from the frame that last used it — the GPU may be reading the buffers this
    //    frame is about to write, which is the whole hazard this class exists to prevent. Waiting fixes it, so this is
    //    where waitForFree is honoured (and where a caller that declined to wait gets Waiting).
    //  * the caller has begun a frame on it and never ended it — no amount of waiting helps, only their EndFrame does,
    //    so this is over-submission and gets Full.
    //
    // Checking the first condition here rather than trusting Acquired is load-bearing: after EndFrame a slot is
    // CPU-free but still busy on the GPU, and handing it straight back would let frame N+3 overwrite what frame N is
    // reading — silently, with no validation layer on this machine to report it.
    for (UInt32 attempt = 0; attempt < slotCount; ++attempt)
    {
        const UInt32 index = impl.NextIndex;
        Impl::Slot& slot = impl.Slots[index];

        if (slot.HasPendingQuery)
        {
            if (impl.Rhi->pollEventQuery(slot.Query))
            {
                impl.Release(index);
            }
            else if (!waitForFree)
            {
                return FrameQueueStatus::Waiting;
            }
            else
            {
                // No timeout is available through NVRHI's waitEventQuery (it waits indefinitely), so a wedged GPU
                // blocks here by design. That is the right trade for an application — it has nothing else to do until
                // the previous frame finishes — and callers that must not hang pass waitForFree = false or use
                // WaitForSlot, which does have a deadline.
                impl.Rhi->waitEventQuery(slot.Query);
                impl.Release(index);
            }
        }

        if (slot.Acquired)
        {
            // Still held by the caller from a frame that was never ended.
            return FrameQueueStatus::Full;
        }

        slot.Acquired = true;
        impl.Current = index;
        impl.NextIndex = (index + 1u) % slotCount;
        return FrameQueueStatus::Began;
    }

    return FrameQueueStatus::Full;
}

bool FrameRing::EndFrame(UInt32 slot) noexcept
{
    if (m_Impl == nullptr || slot >= m_Impl->Slots.size())
    {
        HP_LOG_ERROR("GRAPHICS", "FrameRing::EndFrame: slot {} out of range", slot);
        return false;
    }

    Impl::Slot& target = m_Impl->Slots[slot];
    if (!target.Acquired)
    {
        // Ending a slot that was never begun would arm a query for work that was never submitted, making the slot look
        // busy and then release at a time unrelated to any frame. Assert in debug; refuse in release rather than
        // corrupting the ring's accounting.
        assert(!"FrameRing::EndFrame called for a slot that is not acquired");
        HP_LOG_ERROR("GRAPHICS", "FrameRing::EndFrame: slot {} is not acquired", slot);
        return false;
    }

    // Invariant: an acquired slot has no pending query. HasPendingQuery is set only here (which clears Acquired) and
    // cleared by Release before a slot can be acquired again — so the check NVRHI makes (`assert(commandListID == 0)`
    // before setting, an assert that vanishes in release where a second set would silently re-target the query to newer
    // work) cannot be violated on any path reaching this point. Asserting it keeps the reasoning local if BeginFrame is
    // ever changed.
    assert(!target.HasPendingQuery && "acquired slot must not have a pending query");

    target.Acquired = false;

    // Arm the query against everything submitted so far. A frame that submitted nothing gets a query pointing at an
    // older submission (already complete), which is exactly right: nothing new is outstanding, so the slot may be
    // reused immediately. That only works because Create performed one bootstrap submission — see EnsureQueueStarted.
    m_Impl->Rhi->setEventQuery(target.Query, nvrhi::CommandQueue::Graphics);
    target.HasPendingQuery = true;
    return true;
}

bool FrameRing::IsSlotComplete(UInt32 slot) noexcept
{
    if (m_Impl == nullptr || slot >= m_Impl->Slots.size())
    {
        return false;  // No such slot: claiming completeness would be a lie.
    }

    const Impl::Slot& target = m_Impl->Slots[slot];
    if (!target.HasPendingQuery)
    {
        // Nothing outstanding for this slot — fresh, or used and already released. Answering true is correct and
        // load-bearing: an unset NVRHI query polls FALSE (pollCommandList treats id 0 as "not complete"), so polling
        // instead would report an idle slot as permanently busy.
        return true;
    }

    return m_Impl->Rhi->pollEventQuery(target.Query);
}

bool FrameRing::WaitForSlot(UInt32 slot, UInt64 timeoutNanoseconds) noexcept
{
    if (m_Impl == nullptr || slot >= m_Impl->Slots.size())
    {
        return false;
    }

    if (!m_Impl->Slots[slot].HasPendingQuery)
    {
        return true;  // Nothing to wait for (and polling an unset query would never succeed).
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeoutNanoseconds);

    while (!m_Impl->Rhi->pollEventQuery(m_Impl->Slots[slot].Query))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            // Reported rather than absorbed: the caller is about to write memory the GPU may still be reading. Whether
            // that is survivable is the caller's decision, not this function's.
            HP_LOG_ERROR("GRAPHICS", "FrameRing: slot {} did not complete within its timeout", slot);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::nanoseconds(kPollIntervalNanoseconds));
    }

    return true;
}

UInt32 FrameRing::GetSlotCount() const noexcept
{
    return m_Impl != nullptr ? static_cast<UInt32>(m_Impl->Slots.size()) : 0u;
}

UInt32 FrameRing::GetOutstandingCount() const noexcept
{
    if (m_Impl == nullptr)
    {
        return 0u;
    }

    // Counted from the slots rather than maintained alongside them: a separate counter that drifts is worse than none,
    // because callers use it to decide whether they may submit another frame.
    UInt32 count = 0;
    for (const Impl::Slot& slot : m_Impl->Slots)
    {
        if (slot.Acquired)
        {
            ++count;
        }
    }
    return count;
}

UInt32 FrameRing::GetCurrentSlot() const noexcept
{
    return m_Impl != nullptr ? m_Impl->Current : kNoSlot;
}

}  // namespace Hollowpoint::Graphics
