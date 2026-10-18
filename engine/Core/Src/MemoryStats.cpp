#include <array>
#include <atomic>
#include <cstddef>

#include "Hollowpoint/Core/MemoryStats.h"

namespace Hollowpoint::Core
{

namespace
{

#if defined(HP_TRACK_ALLOCATIONS) && HP_TRACK_ALLOCATIONS
struct TrackerCounters
{
    std::atomic<UInt64> LiveBytes{0};
    std::atomic<UInt64> LiveAllocations{0};
    std::atomic<UInt64> PeakLiveBytes{0};
    std::atomic<UInt64> TotalAllocations{0};
    std::atomic<UInt64> TotalFrees{0};
    std::atomic<UInt64> LargestLiveAllocation{0};
    std::array<std::atomic<UInt64>, static_cast<std::size_t>(MemoryCategory::Count)> CategoryBytes{};
};

TrackerCounters& GetCounters()
{
    static TrackerCounters counters;
    return counters;
}

#endif  // HP_TRACK_ALLOCATIONS

}  // namespace

const char* MemoryCategoryName(MemoryCategory category)
{
    switch (category)
    {
        case MemoryCategory::General:
            return "General";
        case MemoryCategory::Container:
            return "Container";
        case MemoryCategory::String:
            return "String";
        case MemoryCategory::Asset:
            return "Asset";
        case MemoryCategory::Graphics:
            return "Graphics";
        case MemoryCategory::Physics:
            return "Physics";
        case MemoryCategory::Scripting:
            return "Scripting";
        default:
            return "Unknown";
    }
}

namespace Detail
{

void AllocationTracker::RecordAllocate(std::size_t size, MemoryCategory category)
{
#if defined(HP_TRACK_ALLOCATIONS) && HP_TRACK_ALLOCATIONS
    TrackerCounters& counters = GetCounters();
    counters.LiveBytes.fetch_add(size, std::memory_order_relaxed);
    counters.LiveAllocations.fetch_add(1, std::memory_order_relaxed);
    counters.TotalAllocations.fetch_add(1, std::memory_order_relaxed);

    const UInt64 live = counters.LiveBytes.load(std::memory_order_relaxed);
    UInt64 peak = counters.PeakLiveBytes.load(std::memory_order_relaxed);
    while (live > peak && !counters.PeakLiveBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed))
    {
        // peak reloaded by compare_exchange_weak
    }

    UInt64 largest = counters.LargestLiveAllocation.load(std::memory_order_relaxed);
    while (size > largest && !counters.LargestLiveAllocation.compare_exchange_weak(largest, static_cast<UInt64>(size),
                                                                                   std::memory_order_relaxed))
    {
        // largest reloaded by compare_exchange_weak
    }

    const std::size_t categoryIndex = static_cast<std::size_t>(category);
    if (categoryIndex < static_cast<std::size_t>(MemoryCategory::Count))
    {
        counters.CategoryBytes[categoryIndex].fetch_add(size, std::memory_order_relaxed);
    }
#else
    HP_UNUSED(size);
    HP_UNUSED(category);
#endif
}

void AllocationTracker::RecordFree(std::size_t size, MemoryCategory category)
{
#if defined(HP_TRACK_ALLOCATIONS) && HP_TRACK_ALLOCATIONS
    TrackerCounters& counters = GetCounters();
    counters.LiveBytes.fetch_sub(size, std::memory_order_relaxed);
    counters.LiveAllocations.fetch_sub(1, std::memory_order_relaxed);
    counters.TotalFrees.fetch_add(1, std::memory_order_relaxed);

    const std::size_t categoryIndex = static_cast<std::size_t>(category);
    if (categoryIndex < static_cast<std::size_t>(MemoryCategory::Count))
    {
        counters.CategoryBytes[categoryIndex].fetch_sub(size, std::memory_order_relaxed);
    }
#else
    HP_UNUSED(size);
    HP_UNUSED(category);
#endif
}

AllocationStats AllocationTracker::GetStats()
{
#if defined(HP_TRACK_ALLOCATIONS) && HP_TRACK_ALLOCATIONS
    const TrackerCounters& counters = GetCounters();
    AllocationStats stats;
    stats.liveBytes = counters.LiveBytes.load(std::memory_order_relaxed);
    stats.liveAllocations = counters.LiveAllocations.load(std::memory_order_relaxed);
    stats.peakLiveBytes = counters.PeakLiveBytes.load(std::memory_order_relaxed);
    stats.totalAllocations = counters.TotalAllocations.load(std::memory_order_relaxed);
    stats.totalFrees = counters.TotalFrees.load(std::memory_order_relaxed);
    stats.largestLiveAllocation = counters.LargestLiveAllocation.load(std::memory_order_relaxed);
    return stats;
#else
    return AllocationStats{};
#endif
}

std::size_t AllocationTracker::GetLiveBytesInCategory(MemoryCategory category)
{
#if defined(HP_TRACK_ALLOCATIONS) && HP_TRACK_ALLOCATIONS
    const std::size_t index = static_cast<std::size_t>(category);
    if (index >= static_cast<std::size_t>(MemoryCategory::Count))
    {
        return 0;
    }
    return static_cast<std::size_t>(GetCounters().CategoryBytes[index].load(std::memory_order_relaxed));
#else
    HP_UNUSED(category);
    return 0;
#endif
}

void AllocationTracker::ResetCounters()
{
#if defined(HP_TRACK_ALLOCATIONS) && HP_TRACK_ALLOCATIONS
    TrackerCounters& counters = GetCounters();
    counters.PeakLiveBytes.store(counters.LiveBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
    counters.TotalAllocations.store(0, std::memory_order_relaxed);
    counters.TotalFrees.store(0, std::memory_order_relaxed);
    counters.LargestLiveAllocation.store(0, std::memory_order_relaxed);
#endif
}

}  // namespace Detail

AllocationStats GetAllocationStats()
{
    return Detail::AllocationTracker::GetStats();
}

std::size_t GetLiveBytesInCategory(MemoryCategory category)
{
    return Detail::AllocationTracker::GetLiveBytesInCategory(category);
}

void ResetAllocationCounters()
{
    Detail::AllocationTracker::ResetCounters();
}

}  // namespace Hollowpoint::Core
