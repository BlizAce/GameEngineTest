#include "TestSupport.h"

#include <numeric>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/ECS/Detail/SparseSet.h"

using Hollowpoint::Core::Index;
using Hollowpoint::Core::InvalidIndex;
using Hollowpoint::Core::Size;
using Hollowpoint::ECS::Detail::SparseSet;

namespace
{

struct TrackedValue
{
    int Value = 0;

    // Counts live objects, so a test can prove the container does not leak or double-destroy elements.
    static inline int LiveCount = 0;

    TrackedValue()
    {
        ++LiveCount;
    }
    explicit TrackedValue(int value) : Value(value)
    {
        ++LiveCount;
    }
    TrackedValue(const TrackedValue& other) : Value(other.Value)
    {
        ++LiveCount;
    }
    TrackedValue(TrackedValue&& other) noexcept : Value(other.Value)
    {
        ++LiveCount;
    }
    TrackedValue& operator=(const TrackedValue&) = default;
    TrackedValue& operator=(TrackedValue&&) noexcept = default;
    ~TrackedValue()
    {
        --LiveCount;
    }
};

}  // namespace

TEST_SUITE("ECS/SparseSet")
{
    TEST_CASE("an empty set finds nothing and reports itself empty")
    {
        SparseSet<int> set;

        CHECK(set.IsEmpty());
        CHECK(set.GetCount() == 0u);
        CHECK(set.Find(0) == nullptr);
        CHECK_FALSE(set.Contains(0));
        CHECK_FALSE(set.Remove(0));
    }

    TEST_CASE("insert then find round-trips values")
    {
        SparseSet<int> set;

        int* inserted = set.Insert(5);
        REQUIRE(inserted != nullptr);
        *inserted = 42;

        CHECK(set.Contains(5));
        REQUIRE(set.Find(5) != nullptr);
        CHECK(*set.Find(5) == 42);
        CHECK(set.Get(5) == 42);
        CHECK(set.GetCount() == 1u);
    }

    TEST_CASE("inserting an existing slot returns the current value instead of overwriting")
    {
        // Systems call Insert-with-default on every frame in some patterns; silently resetting a component
        // that already exists would zero accumulated state (health, timers) mid-game.
        SparseSet<int> set;

        *set.Insert(3) = 100;
        int* again = set.Insert(3);

        CHECK(*again == 100);
        CHECK(set.GetCount() == 1u);
    }

    TEST_CASE("sparse indices may be sparse and non-contiguous")
    {
        SparseSet<int> set;

        set.Insert(0, 10);
        set.Insert(9000, 20);
        set.Insert(4, 30);

        CHECK(set.GetCount() == 3u);
        CHECK(*set.Find(0) == 10);
        CHECK(*set.Find(9000) == 20);
        CHECK(*set.Find(4) == 30);

        // Gaps in between stay absent, including slots passed during growth.
        CHECK(set.Find(1) == nullptr);
        CHECK(set.Find(8999) == nullptr);
    }

    TEST_CASE("dense iteration visits every entity exactly once")
    {
        SparseSet<int> set;
        for (Index slot = 0; slot < 50; ++slot)
        {
            set.Insert(slot, static_cast<int>(slot));
        }

        std::vector<int> visited(50, 0);
        for (Size index = 0; index < set.GetCount(); ++index)
        {
            const Index slot = set.GetSlotAt(index);
            REQUIRE(slot < 50u);
            // The value must belong to the entity named by the dense position — a mismatch here means the
            // sparse and dense arrays drifted apart.
            CHECK(set.GetValueAt(index) == static_cast<int>(slot));
            ++visited[slot];
        }

        CHECK(std::accumulate(visited.begin(), visited.end(), 0) == 50);
        for (int count : visited)
        {
            CHECK(count == 1);
        }
    }

    TEST_CASE("removal keeps every other element findable and correctly indexed")
    {
        SparseSet<int> set;
        for (Index slot = 0; slot < 10; ++slot)
        {
            set.Insert(slot, static_cast<int>(slot) * 10);
        }

        CHECK(set.Remove(4));
        CHECK(set.GetCount() == 9u);
        CHECK_FALSE(set.Contains(4));
        CHECK(set.Find(4) == nullptr);

        for (Index slot = 0; slot < 10; ++slot)
        {
            if (slot == 4)
            {
                continue;
            }
            REQUIRE(set.Find(slot) != nullptr);
            CHECK(*set.Find(slot) == static_cast<int>(slot) * 10);
        }

        // Removing again is a no-op rather than corrupting the set.
        CHECK_FALSE(set.Remove(4));
        CHECK(set.GetCount() == 9u);
    }

    TEST_CASE("removal updates the moved element's sparse entry")
    {
        // The swap-back moves the last value into the hole. If its reverse mapping is not updated, the moved
        // entity becomes unreachable while a stale index still points at the hole — this test targets exactly
        // that bookkeeping.
        SparseSet<int> set;
        set.Insert(1, 100);
        set.Insert(2, 200);
        set.Insert(3, 300);

        const Index movedSlot = set.GetSlotAt(2);
        CHECK(movedSlot == 3u);

        CHECK(set.Remove(1));  // Removes the first element; slot 3 moves into dense position 0.

        REQUIRE(set.Find(3) != nullptr);
        CHECK(*set.Find(3) == 300);
        CHECK(set.GetDenseIndex(3) == 0);
        CHECK(set.GetSlotAt(0) == 3u);
    }

    TEST_CASE("removing the last dense element does not move anything")
    {
        SparseSet<int> set;
        set.Insert(7, 70);
        set.Insert(8, 80);

        CHECK(set.Remove(8));
        CHECK(set.GetCount() == 1u);
        REQUIRE(set.Find(7) != nullptr);
        CHECK(*set.Find(7) == 70);
    }

    TEST_CASE("re-inserting after removal reuses the dense space")
    {
        SparseSet<int> set;
        for (Index slot = 0; slot < 20; ++slot)
        {
            set.Insert(slot, static_cast<int>(slot));
        }
        for (Index slot = 0; slot < 10; ++slot)
        {
            CHECK(set.Remove(slot));
        }

        CHECK(set.GetCount() == 10u);

        // Values must still be reachable after heavy churn, and new inserts land in the compacted range.
        for (Index slot = 10; slot < 20; ++slot)
        {
            REQUIRE(set.Find(slot) != nullptr);
            CHECK(*set.Find(slot) == static_cast<int>(slot));
        }

        set.Insert(100, 999);
        CHECK(set.GetCount() == 11u);
        CHECK(*set.Find(100) == 999);
    }

    TEST_CASE("clear empties the set and leaves it reusable")
    {
        SparseSet<int> set;
        for (Index slot = 0; slot < 30; ++slot)
        {
            set.Insert(slot, static_cast<int>(slot));
        }

        set.Clear();

        CHECK(set.IsEmpty());
        CHECK(set.GetCount() == 0u);
        for (Index slot = 0; slot < 30; ++slot)
        {
            CHECK_FALSE(set.Contains(slot));
        }

        // Every reverse mapping must have been cleared, or the next insert would corrupt the new layout.
        set.Insert(5, 55);
        CHECK(set.GetCount() == 1u);
        CHECK(*set.Find(5) == 55);
        CHECK(set.GetSlotAt(0) == 5u);
    }

    TEST_CASE("elements are constructed and destroyed exactly once")
    {
        // A container that copies where it should move, or forgets to destroy on remove/clear, corrupts ref-
        // counted components (textures, meshes) held by value.
        TrackedValue::LiveCount = 0;

        {
            SparseSet<TrackedValue> set;
            set.Insert(1, TrackedValue(1));
            set.Insert(2, TrackedValue(2));
            set.Insert(3, TrackedValue(3));

            // Insert-by-value takes a copy; the source dies, so live count tracks set membership only.
            CHECK(TrackedValue::LiveCount == 3);

            CHECK(set.Remove(2));
            CHECK(TrackedValue::LiveCount == 2);

            set.Clear();
            CHECK(TrackedValue::LiveCount == 0);
        }
        CHECK(TrackedValue::LiveCount == 0);
    }

    // The engine's assertion contract (Core/Assert.h) lets a failed invariant *continue*: the editor turns one
    // into a reported error, and tests divert them. So the code immediately after an assert must still be safe.
    // These cases pin that down — before it was fixed, Get dereferenced null and bound accessors read past the
    // end of the array, which is exactly what turns a recoverable editor error into a hard crash.

    TEST_CASE("Get on an absent slot asserts and returns a safe stand-in")
    {
        SparseSet<int> set;
        set.Insert(1, 7);

        int value = -1;
        {
            Hollowpoint::Test::ScopedAssertCapture capture;
            value = set.Get(12);  // Absent: asserts, must not dereference null.
            CHECK(capture.GetCaptureCount() == 1);
        }

        // Default-constructed rather than garbage — true only because this is the first failed lookup of an
        // int on this thread; see the next test case for what happens after something has been written through
        // the stand-in.
        CHECK(value == 0);  // Default-constructed rather than garbage.

        // Writing through the stand-in must be harmless and must not corrupt the set.
        {
            Hollowpoint::Test::ScopedAssertCapture capture;
            set.Get(13) = 999;
        }
        CHECK(set.GetCount() == 1u);
        CHECK(set.Get(1) == 7);
    }

    TEST_CASE("the stand-in is shared per value type, so a failed lookup can return a stale value")
    {
        // Documented consequence of the design rather than a bug to fix: Get cannot fabricate a fresh default
        // on every call without allocating, so one thread_local instance per component type backs all failed
        // lookups. A caller that continued past the assert may therefore read what an earlier failed lookup
        // wrote. That is acceptable — continuing is already an error path — but it has to be known, because it
        // means "the value was 1234" is never evidence that a component existed.
        SparseSet<int> localSet;

        {
            Hollowpoint::Test::ScopedAssertCapture capture;
            localSet.Get(7) = 1234;
            CHECK(localSet.Get(8) == 1234);  // Same stand-in, not a real component.
            CHECK(localSet.IsEmpty());       // ...and nothing was ever inserted.
        }
    }

    TEST_CASE("the stand-in is per-thread, so two threads cannot race on it")
    {
        SparseSet<int> set;

        // Each thread's failed lookup must get its own storage. Running the writes concurrently and then
        // checking both observed their own value would be racy by construction; what matters here — and what
        // ASan/TSan builds would catch if it were a shared global — is that concurrent failed lookups do not
        // corrupt each other or crash.
        std::vector<std::thread> threads;
        for (int threadIndex = 0; threadIndex < 4; ++threadIndex)
        {
            threads.emplace_back([&set, threadIndex]() {
                Hollowpoint::Test::ScopedAssertCapture capture;
                for (int iteration = 0; iteration < 200; ++iteration)
                {
                    int& reference = set.Get(static_cast<Index>(threadIndex));
                    reference = threadIndex * 1000 + iteration;
                }
            });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        CHECK(set.IsEmpty());  // Nothing was inserted by any of the failed lookups.
    }

    TEST_CASE("dense accessors bound-check and degrade safely")
    {
        SparseSet<int> set;
        set.Insert(0, 1);

        Hollowpoint::Test::ScopedAssertCapture capture;

        // Values are not asserted here: the stand-in is shared per type (see above), so only "stays in bounds,
        // does not crash, does not corrupt" is a meaningful claim at this point in the run.
        static_cast<void>(set.GetValueAt(5));
        CHECK(set.GetSlotAt(5) == InvalidIndex);

        // The const path behaves identically.
        const SparseSet<int>& constSet = set;
        static_cast<void>(constSet.GetValueAt(99));
        static_cast<void>(constSet.Get(3));

        CHECK(capture.GetCaptureCount() == 4);

        // Still intact afterwards: a bounds violation must not damage the container.
        CHECK(set.GetCount() == 1u);
        CHECK(set.Get(0) == 1);
    }

    TEST_CASE("ReserveSlots pre-sizes without inserting anything")
    {
        SparseSet<int> set;
        set.ReserveSlots(1024);

        CHECK(set.IsEmpty());
        // A slot inside the reserved range is still absent...
        CHECK_FALSE(set.Contains(500));
        // ...and lookups there must not assert or read uninitialised memory.
        CHECK(set.Find(500) == nullptr);
    }

    TEST_CASE("data pointer exposes contiguous storage for vectorised systems")
    {
        SparseSet<int> set;
        for (Index slot = 0; slot < 16; ++slot)
        {
            set.Insert(slot, static_cast<int>(slot));
        }

        const int* data = set.GetData();
        REQUIRE(data != nullptr);
        for (Size index = 0; index < set.GetCount(); ++index)
        {
            // GetValueAt and the raw pointer must agree; if they did not, a system using the fast path would
            // read different values than one using the accessor.
            CHECK(data[index] == set.GetValueAt(index));
        }
    }

    TEST_CASE("survives heavy interleaved churn without losing elements")
    {
        // The invariant after arbitrary insert/remove sequences: for every slot, Contains iff Find matches the
        // value last written, and dense count equals the number of live slots.
        SparseSet<int> set;
        std::vector<int> expected(256, -1);

        unsigned int state = 0x12345678u;
        auto nextRandom = [&state]() {
            state = state * 1664525u + 1013904223u;
            return (state >> 16) & 0xFF;
        };

        for (int step = 0; step < 20000; ++step)
        {
            const Index slot = nextRandom();
            if ((nextRandom() & 1u) != 0u)
            {
                set.Insert(slot, static_cast<int>(slot));
                expected[slot] = static_cast<int>(slot);
            }
            else
            {
                set.Remove(slot);
                expected[slot] = -1;
            }

            if (step % 500 == 0)
            {
                Size liveCount = 0;
                for (int value : expected)
                {
                    if (value >= 0)
                    {
                        ++liveCount;
                    }
                }
                INFO("step " << step);
                CHECK(set.GetCount() == liveCount);

                for (Index probe = 0; probe < 256; ++probe)
                {
                    const int* found = set.Find(probe);
                    if (expected[probe] < 0)
                    {
                        CHECK(found == nullptr);
                    }
                    else
                    {
                        REQUIRE(found != nullptr);
                        CHECK(*found == expected[probe]);
                    }
                }
            }
        }
    }
}
