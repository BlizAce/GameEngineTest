#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Hollowpoint/Core/Memory.h"

namespace Hollowpoint::Core
{

namespace
{

// ---------------------------------------------------------------------------
// Built-in backend: malloc-style allocation plus the statistics bookkeeping.
//
// Portability note: we deliberately avoid aligned_alloc()/std::aligned_alloc because the requirement
// that the size be a multiple of the alignment differs between libc implementations (and MSVC's
// answer is a different function entirely). Instead we over-allocate and stash the original pointer
// in the slot immediately before the aligned address — portable, O(1), and correct for any alignment.
//
//   layout:  [ saved base ptr ][ padding ][ payload ... ]
//                                     ^ returned pointer (aligned)
// ---------------------------------------------------------------------------

constexpr std::size_t PointerSlot = sizeof(void*);

void* BuiltinAllocate(std::size_t size, std::size_t alignment, MemoryCategory category, void* /*userData*/)
{
    if (size == 0)
    {
        return nullptr;
    }
    if (alignment < alignof(std::max_align_t))
    {
        alignment = alignof(std::max_align_t);
    }

    const std::size_t totalSize = PointerSlot + size + alignment;
    void* base = ::operator new(totalSize, std::nothrow);
    if (base == nullptr)
    {
        return nullptr;
    }

    const std::uintptr_t payloadStart = reinterpret_cast<std::uintptr_t>(base) + PointerSlot;
    const std::uintptr_t alignedAddress =
        (payloadStart + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);

    *reinterpret_cast<void**>(alignedAddress - PointerSlot) = base;

    Detail::AllocationTracker::RecordAllocate(size, category);
    return reinterpret_cast<void*>(alignedAddress);
}

void BuiltinDeallocate(void* pointer, std::size_t size, MemoryCategory category)
{
    if (pointer == nullptr)
    {
        return;
    }
    void* base = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(pointer) - PointerSlot);
    Detail::AllocationTracker::RecordFree(size, category);
    ::operator delete(base);
}

void* BuiltinReallocate(void* pointer, std::size_t oldSize, std::size_t newSize, std::size_t alignment,
                        MemoryCategory category, void* userData)
{
    if (pointer == nullptr)
    {
        return BuiltinAllocate(newSize, alignment, category, userData);
    }
    if (newSize == 0)
    {
        BuiltinDeallocate(pointer, oldSize, category);
        return nullptr;
    }

    void* newPointer = BuiltinAllocate(newSize, alignment, category, userData);
    if (newPointer == nullptr)
    {
        return nullptr;
    }

    const std::size_t copySize = oldSize < newSize ? oldSize : newSize;
    std::memcpy(newPointer, pointer, copySize);
    BuiltinDeallocate(pointer, oldSize, category);
    return newPointer;
}

std::atomic<const AllocatorBackend*> g_Backend{nullptr};

const AllocatorBackend* GetBackend()
{
    return g_Backend.load(std::memory_order_acquire);
}

}  // namespace

void SetAllocatorBackend(const AllocatorBackend* backend)
{
    g_Backend.store(backend, std::memory_order_release);
}

void* Allocate(std::size_t size, std::size_t alignment, MemoryCategory category)
{
    if (size == 0)
    {
        return nullptr;
    }

    const AllocatorBackend* backend = GetBackend();
    if (backend != nullptr && backend->Allocate != nullptr)
    {
        return backend->Allocate(size, alignment, category, backend->userData);
    }
    return BuiltinAllocate(size, alignment, category, nullptr);
}

void* Reallocate(void* pointer, std::size_t oldSize, std::size_t newSize, std::size_t alignment,
                 MemoryCategory category)
{
    const AllocatorBackend* backend = GetBackend();
    if (backend != nullptr && backend->Reallocate != nullptr)
    {
        return backend->Reallocate(pointer, oldSize, newSize, alignment, category, backend->userData);
    }
    return BuiltinReallocate(pointer, oldSize, newSize, alignment, category, nullptr);
}

void Deallocate(void* pointer, std::size_t size, MemoryCategory category)
{
    if (pointer == nullptr)
    {
        return;
    }

    const AllocatorBackend* backend = GetBackend();
    if (backend != nullptr && backend->Deallocate != nullptr)
    {
        backend->Deallocate(pointer, size, category, backend->userData);
        return;
    }
    BuiltinDeallocate(pointer, size, category);
}

void* AllocateZeroed(std::size_t count, std::size_t size, std::size_t alignment, MemoryCategory category)
{
    if (count == 0 || size == 0)
    {
        return nullptr;
    }
    void* memory = Allocate(count * size, alignment, category);
    if (memory != nullptr)
    {
        std::memset(memory, 0, count * size);
    }
    return memory;
}

}  // namespace Hollowpoint::Core
