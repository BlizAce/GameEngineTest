#include <cstdint>
#include <cstring>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Memory.h"
#include "Hollowpoint/Core/MemoryStats.h"

using namespace Hollowpoint::Core;

namespace
{

// Counts calls to a custom backend so we can prove the engine actually routes through it.
struct BackendProbe
{
    int AllocateCalls = 0;
    int ReallocateCalls = 0;
    int DeallocateCalls = 0;
    void* LastPointer = nullptr;
    std::size_t LastSize = 0;

    static void* Allocate(std::size_t size, std::size_t alignment, MemoryCategory, void* userData)
    {
        HP_UNUSED(alignment);
        BackendProbe* probe = static_cast<BackendProbe*>(userData);
        ++probe->AllocateCalls;
        probe->LastSize = size;
        void* pointer = ::operator new(size);
        probe->LastPointer = pointer;
        return pointer;
    }

    static void* Reallocate(void*, std::size_t, std::size_t, MemoryCategory, void*)
    {
        return nullptr;
    }

    static void Deallocate(void* pointer, std::size_t /*size*/, MemoryCategory, void* userData)
    {
        BackendProbe* probe = static_cast<BackendProbe*>(userData);
        ++probe->DeallocateCalls;
        ::operator delete(pointer);
    }
};

struct TrackedObject
{
    static int s_Constructions;
    static int s_Destructions;

    explicit TrackedObject(int value = 0) : Value(value)
    {
        ++s_Constructions;
    }
    ~TrackedObject()
    {
        ++s_Destructions;
    }

    int Value = 0;
};

int TrackedObject::s_Constructions = 0;
int TrackedObject::s_Destructions = 0;

}  // namespace

TEST_SUITE("Core/Memory")
{
    TEST_CASE("Allocate honours alignment for every power-of-two we care about")
    {
        for (std::size_t alignment :
             {std::size_t(8), std::size_t(16), std::size_t(32), std::size_t(64), std::size_t(256)})
        {
            void* pointer = Allocate(100, alignment);
            REQUIRE(pointer != nullptr);
            CHECK(reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0);

            // Writing across the whole block must stay inside the allocation (checked by ASan builds).
            std::memset(pointer, 0xAB, 100);
            Deallocate(pointer, 100);
        }
    }

    TEST_CASE("Allocate returns nullptr for a zero-size request")
    {
        CHECK(Allocate(0) == nullptr);
        Deallocate(nullptr, 0);  // must be a safe no-op
    }

    TEST_CASE("AllocateZeroed clears the whole block")
    {
        constexpr std::size_t Count = 64;
        UInt8* block = static_cast<UInt8*>(AllocateZeroed(Count, sizeof(UInt8)));
        REQUIRE(block != nullptr);
        for (std::size_t index = 0; index < Count; ++index)
        {
            REQUIRE(block[index] == 0);
        }
        Deallocate(block, Count);
    }

    TEST_CASE("Reallocate preserves the original contents")
    {
        char* text = static_cast<char*>(Allocate(8, alignof(char)));
        REQUIRE(text != nullptr);
        std::memcpy(text, "Hollow", 6);

        char* grown = static_cast<char*>(Reallocate(text, 8, 32));
        REQUIRE(grown != nullptr);
        CHECK(std::memcmp(grown, "Hollow", 6) == 0);

        // Growing then shrinking keeps the prefix intact.
        char* shrunk = static_cast<char*>(Reallocate(grown, 32, 4));
        REQUIRE(shrunk != nullptr);
        CHECK(std::memcmp(shrunk, "Holl", 4) == 0);
        Deallocate(shrunk, 4);

        // Reallocating null behaves like Allocate; reallocating to zero frees.
        void* fromNull = Reallocate(nullptr, 0, 16);
        CHECK(fromNull != nullptr);
        CHECK(Reallocate(fromNull, 16, 0) == nullptr);
    }

    TEST_CASE("New and Delete run constructors and destructors exactly once")
    {
        TrackedObject::s_Constructions = 0;
        TrackedObject::s_Destructions = 0;

        TrackedObject* object = New<TrackedObject>(42);
        REQUIRE(object != nullptr);
        CHECK(object->Value == 42);
        CHECK(TrackedObject::s_Constructions == 1);

        Delete(object);
        CHECK(TrackedObject::s_Destructions == 1);

        // Deleting null is a no-op.
        Delete(static_cast<TrackedObject*>(nullptr));
        CHECK(TrackedObject::s_Destructions == 1);
    }

    TEST_CASE("NewArray and DeleteArray handle non-trivial element types")
    {
        TrackedObject::s_Constructions = 0;
        TrackedObject::s_Destructions = 0;

        constexpr std::size_t Count = 8;
        TrackedObject* array = NewArray<TrackedObject>(Count);
        REQUIRE(array != nullptr);
        CHECK(TrackedObject::s_Constructions == static_cast<int>(Count));

        DeleteArray(array, Count);
        CHECK(TrackedObject::s_Destructions == static_cast<int>(Count));
    }

    TEST_CASE("the STL allocator routes containers through the engine and satisfies the concept")
    {
        Allocator<Int32> intAllocator;
        Int32* raw = intAllocator.allocate(4);
        REQUIRE(raw != nullptr);
        raw[0] = 7;
        CHECK(raw[0] == 7);
        intAllocator.deallocate(raw, 4);

        std::vector<Int32, Allocator<Int32>> values;
        for (Int32 index = 0; index < 1000; ++index)
        {
            values.push_back(index);
        }
        CHECK(values.size() == 1000);
        CHECK(values.front() == 0);
        CHECK(values.back() == 999);

        std::string stdString("Hollowpoint");
        CHECK(stdString.size() == 11);
    }

    TEST_CASE("a custom backend receives every allocation")
    {
        BackendProbe probe;
        AllocatorBackend backend;
        backend.Allocate = &BackendProbe::Allocate;
        backend.Deallocate = &BackendProbe::Deallocate;
        backend.userData = &probe;

        SetAllocatorBackend(&backend);

        void* pointer = Allocate(128);
        CHECK(probe.AllocateCalls == 1);
        CHECK(probe.LastSize == 128);
        Deallocate(pointer, 128);
        CHECK(probe.DeallocateCalls == 1);

        SetAllocatorBackend(nullptr);
        CHECK(probe.AllocateCalls == 1);

        void* builtin = Allocate(64);
        REQUIRE(builtin != nullptr);
        std::memset(builtin, 0, 64);
        Deallocate(builtin, 64);
    }

    TEST_CASE("allocation statistics track live bytes when enabled")
    {
        if (!AllocationTrackingEnabled())
        {
            // Shipping builds compile the bookkeeping out; the API must still answer sensibly.
            CHECK(GetAllocationStats().liveBytes == 0);
            return;
        }

        const AllocationStats before = GetAllocationStats();
        void* pointer = Allocate(4096, alignof(std::max_align_t), MemoryCategory::Asset);
        REQUIRE(pointer != nullptr);

        const AllocationStats during = GetAllocationStats();
        CHECK(during.liveBytes == before.liveBytes + 4096);
        CHECK(during.totalAllocations == before.totalAllocations + 1);
        CHECK(GetLiveBytesInCategory(MemoryCategory::Asset) >= 4096);

        Deallocate(pointer, 4096, MemoryCategory::Asset);
        const AllocationStats after = GetAllocationStats();
        CHECK(after.liveBytes == before.liveBytes);
        CHECK(after.totalFrees == before.totalFrees + 1);
        CHECK(after.peakLiveBytes >= during.liveBytes);
    }

    TEST_CASE("MemoryCategoryName covers every category")
    {
        CHECK(std::string_view(MemoryCategoryName(MemoryCategory::General)) == "General");
        CHECK(std::string_view(MemoryCategoryName(MemoryCategory::Graphics)) == "Graphics");
        CHECK(std::string_view(MemoryCategoryName(static_cast<MemoryCategory>(200))) == "Unknown");
    }
}
