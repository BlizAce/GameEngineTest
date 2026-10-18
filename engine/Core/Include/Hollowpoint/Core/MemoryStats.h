#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <utility>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Tracking allocation statistics.
//
// Every engine allocation goes through Allocate()/Deallocate() (see Memory.h and the STL adapter),
// which updates these counters. The counters are atomics so that a worker thread can allocate while
// the main thread renders a memory HUD. When HP_TRACK_ALLOCATIONS is off, the functions compile to
// plain malloc/free calls with zero bookkeeping — that is the shipping configuration.
// ---------------------------------------------------------------------------

// Snapshot of the allocator counters (all values in bytes unless noted).
struct AllocationStats
{
    UInt64 liveBytes = 0;              //!< Bytes currently allocated and not yet freed.
    UInt64 liveAllocations = 0;        //!< Number of live allocations.
    UInt64 peakLiveBytes = 0;          //!< High-water mark of liveBytes since process start.
    UInt64 totalAllocations = 0;       //!< Allocation calls since process start (never decreases).
    UInt64 totalFrees = 0;             //!< Free calls since process start.
    UInt64 largestLiveAllocation = 0;  //!< Size of the single largest live allocation observed.
};

// Byte counters per memory category, used for leak reporting. Only maintained when tracking is on.
enum class MemoryCategory : UInt8
{
    General = 0,
    Container,
    String,
    Asset,
    Graphics,
    Physics,
    Scripting,
    Count
};

// Human readable category name ("Graphics"), or "Unknown" for an invalid value.
HP_CORE_API const char* MemoryCategoryName(MemoryCategory category);

// True when this build records allocation statistics (compile-time constant).
[[nodiscard]] HP_CORE_API constexpr bool AllocationTrackingEnabled()
{
#if defined(HP_TRACK_ALLOCATIONS) && HP_TRACK_ALLOCATIONS
    return true;
#else
    return false;
#endif
}

namespace Detail
{

class HP_CORE_API AllocationTracker
{
public:
    static void RecordAllocate(std::size_t size, MemoryCategory category);
    static void RecordFree(std::size_t size, MemoryCategory category);

    static AllocationStats GetStats();
    static std::size_t GetLiveBytesInCategory(MemoryCategory category);
    static void ResetCounters();
};

}  // namespace Detail

// Returns a snapshot of the allocation counters. Zeroed when tracking is compiled out.
[[nodiscard]] HP_CORE_API AllocationStats GetAllocationStats();

// Live bytes attributed to one category (0 when tracking is compiled out).
[[nodiscard]] HP_CORE_API std::size_t GetLiveBytesInCategory(MemoryCategory category);

// Resets the cumulative counters (peak and totals). Live byte counts are preserved so that a leak
// check started after a reset still sees everything allocated before it. Used by tests.
HP_CORE_API void ResetAllocationCounters();

}  // namespace Hollowpoint::Core
