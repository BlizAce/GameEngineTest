#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/MemoryStats.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Allocation entry points.
//
// All engine memory flows through Allocate/Reallocate/Deallocate so that (a) statistics can be
// collected in development builds and (b) a custom allocator (linear/frame allocator, pool) can be
// substituted in one place. Applications must call InitializeMemorySystem() once at startup if they
// want the tracked allocator installed; otherwise these functions forward to malloc/free.
// ---------------------------------------------------------------------------

// Function pointers for a pluggable allocator backend. `userData` is passed through untouched and
// may be null. Alignment must be honoured; alignment 0 or 1 means "fundamentally aligned".
struct AllocatorBackend
{
    void* (*Allocate)(std::size_t size, std::size_t alignment, MemoryCategory category, void* userData) = nullptr;
    void* (*Reallocate)(void* pointer, std::size_t oldSize, std::size_t newSize, std::size_t alignment,
                        MemoryCategory category, void* userData) = nullptr;
    void (*Deallocate)(void* pointer, std::size_t size, MemoryCategory category, void* userData) = nullptr;
    void* userData = nullptr;
};

// Installs a backend allocator. Passing nullptr restores the built-in malloc-backed implementation.
// Must not be called while other threads are allocating (it is used at startup/shutdown only).
HP_CORE_API void SetAllocatorBackend(const AllocatorBackend* backend);

// Allocates `size` bytes with the given alignment. Returns nullptr when size is 0 or memory runs out.
[[nodiscard]] HP_CORE_API void* Allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t),
                                         MemoryCategory category = MemoryCategory::General);

// Resizes an allocation produced by Allocate(). `oldSize` must match the previous size.
[[nodiscard]] HP_CORE_API void* Reallocate(void* pointer, std::size_t oldSize, std::size_t newSize,
                                           std::size_t alignment = alignof(std::max_align_t),
                                           MemoryCategory category = MemoryCategory::General);

// Frees an allocation. `size` must match the size passed to Allocate(). Null pointer is a no-op.
HP_CORE_API void Deallocate(void* pointer, std::size_t size, MemoryCategory category = MemoryCategory::General);

// Allocates and zero-initializes `count * size` bytes.
[[nodiscard]] HP_CORE_API void* AllocateZeroed(std::size_t count, std::size_t size,
                                               std::size_t alignment = alignof(std::max_align_t),
                                               MemoryCategory category = MemoryCategory::General);

// ---------------------------------------------------------------------------
// Typed helpers
// ---------------------------------------------------------------------------

//! Allocates uninitialised storage for `count` objects of type T. No constructors run — pair it with
//! NewArray() (which does construct) or placement-new, and release with DeleteArray().
template <typename T>
[[nodiscard]] inline T* AllocateArray(std::size_t count, MemoryCategory category = MemoryCategory::General)
{
    static_assert(!std::is_function_v<T>, "AllocateArray cannot allocate functions");
    return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T), category));
}

// Constructs one object. Returns nullptr when the allocation failed.
template <typename T, typename... Args>
[[nodiscard]] inline T* New(MemoryCategory category, Args&&... args)
{
    void* memory = Allocate(sizeof(T), alignof(T), category);
    if (memory == nullptr)
    {
        return nullptr;
    }
    return ::new (memory) T(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
[[nodiscard]] inline T* New(Args&&... args)
{
    return New<T>(MemoryCategory::General, std::forward<Args>(args)...);
}

// Destroys an object created with New() and releases its memory. Null pointer is a no-op.
template <typename T>
inline void Delete(T* object, MemoryCategory category = MemoryCategory::General)
{
    if (object == nullptr)
    {
        return;
    }
    if constexpr (!std::is_trivially_destructible_v<T>)
    {
        object->~T();
    }
    Deallocate(object, sizeof(T), category);
}

//! Destroys `count` elements and releases the block. `count` must match NewArray()'s argument.
template <typename T>
inline void DeleteArray(T* array, std::size_t count, MemoryCategory category = MemoryCategory::General)
{
    if (array == nullptr)
    {
        return;
    }
    if constexpr (!std::is_trivially_destructible_v<T>)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            array[index].~T();
        }
    }
    Deallocate(array, sizeof(T) * count, category);
}

//! Allocates and default-constructs `count` objects. Release with DeleteArray() passing the same
//! count: the engine allocator does not store element counts, so a mismatch leaks or corrupts.
template <typename T>
[[nodiscard]] inline T* NewArray(std::size_t count, MemoryCategory category = MemoryCategory::General)
{
    T* array = AllocateArray<T>(count, category);
    if (array == nullptr)
    {
        return nullptr;
    }
    for (std::size_t index = 0; index < count; ++index)
    {
        ::new (static_cast<void*>(&array[index])) T();
    }
    return array;
}

// ---------------------------------------------------------------------------
// STL integration
// ---------------------------------------------------------------------------

// Standard-conforming allocator that routes the STL through the engine allocation functions.
// Use it for containers whose memory should show up in the memory HUD:
//   std::vector<Int32, Core::Allocator<Int32>> indices;
// (HPContainers.h provides aliases — prefer those.)
template <typename T, MemoryCategory Category = MemoryCategory::General>
class Allocator
{
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;

    template <typename U>
    struct rebind
    {
        using other = Allocator<U, Category>;
    };

    constexpr Allocator() noexcept = default;
    template <typename U>
    constexpr Allocator(const Allocator<U, Category>&) noexcept
    {}

    [[nodiscard]] T* allocate(std::size_t count)
    {
        return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T), Category));
    }

    void deallocate(T* memory, std::size_t count) noexcept
    {
        Deallocate(memory, sizeof(T) * count, Category);
    }

    template <typename U>
    constexpr bool operator==(const Allocator<U, Category>&) const noexcept
    {
        return true;
    }
    template <typename U>
    constexpr bool operator!=(const Allocator<U, Category>&) const noexcept
    {
        return false;
    }
};

}  // namespace Hollowpoint::Core
