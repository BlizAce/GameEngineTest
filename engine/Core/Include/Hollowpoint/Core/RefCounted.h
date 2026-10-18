#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Memory.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Core
{

// ---------------------------------------------------------------------------
// Intrusive reference counting.
//
// RefCounted is the base for engine-owned objects that are shared but not managed by the asset
// system (renderer resources, command lists, script VMs...). It is intrusive on purpose: one
// allocation per object, no control block, and the reference count sits in the same cache line as
// the object header. The count is 32-bit and saturating — hitting it is a bug we want asserted, not
// an overflow that silently leaks.
//
// Use the Ref<T> smart pointer to hold references; never call AddRef/Release by hand outside of it.
// ---------------------------------------------------------------------------

class HP_CORE_API RefCounted
{
public:
    RefCounted() = default;
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;
    RefCounted(RefCounted&&) noexcept {}
    RefCounted& operator=(RefCounted&&) noexcept
    {
        return *this;
    }

    // Reference count is deliberately not copied on move: a moved-to object starts unowned.
    virtual ~RefCounted() = default;

    void AddRef() const noexcept
    {
        HP_ASSERT(m_ReferenceCount.load(std::memory_order_relaxed) != 0 && "AddRef on a destroyed object");
        m_ReferenceCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Decrements the count and deletes the object when it reaches zero.
    void Release() const noexcept
    {
        const Int32 previous = m_ReferenceCount.fetch_sub(1, std::memory_order_acq_rel);
        HP_ASSERT(previous > 0 && "Release() on an object with no owners left");
        if (previous == 1)
        {
            delete const_cast<RefCounted*>(this);
        }
    }

    [[nodiscard]] Int32 GetReferenceCount() const noexcept
    {
        return m_ReferenceCount.load(std::memory_order_relaxed);
    }

protected:
    mutable std::atomic<Int32> m_ReferenceCount{1};
};

// Smart pointer for RefCounted-derived types. Semantics mirror a raw owning pointer: copying is a
// cheap atomic increment, moving is free of atomics.
template <typename T>
class Ref
{
public:
    using ElementType = T;

    Ref() noexcept = default;
    Ref(std::nullptr_t) noexcept {}

    // Takes ownership of a freshly created object (count already 1), matching CreateObject().
    explicit Ref(T* pointer) noexcept : m_Pointer(pointer) {}

    Ref(const Ref& other) noexcept : m_Pointer(other.m_Pointer)
    {
        if (m_Pointer != nullptr)
        {
            m_Pointer->AddRef();
        }
    }

    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    Ref(const Ref<U>& other) noexcept : m_Pointer(other.Get())
    {
        if (m_Pointer != nullptr)
        {
            m_Pointer->AddRef();
        }
    }

    Ref(Ref&& other) noexcept : m_Pointer(other.m_Pointer)
    {
        other.m_Pointer = nullptr;
    }

    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    Ref(Ref<U>&& other) noexcept : m_Pointer(other.Detach())
    {}

    ~Ref()
    {
        if (m_Pointer != nullptr)
        {
            m_Pointer->Release();
        }
    }

    Ref& operator=(const Ref& other) noexcept
    {
        if (this != &other)
        {
            if (other.m_Pointer != nullptr)
            {
                other.m_Pointer->AddRef();
            }
            if (m_Pointer != nullptr)
            {
                m_Pointer->Release();
            }
            m_Pointer = other.m_Pointer;
        }
        return *this;
    }

    Ref& operator=(Ref&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Pointer != nullptr)
            {
                m_Pointer->Release();
            }
            m_Pointer = other.m_Pointer;
            other.m_Pointer = nullptr;
        }
        return *this;
    }

    Ref& operator=(std::nullptr_t) noexcept
    {
        Reset();
        return *this;
    }

    // Replaces the held pointer, taking ownership of `pointer` (whose count must be 1).
    void Reset(T* pointer = nullptr) noexcept
    {
        if (m_Pointer != nullptr)
        {
            m_Pointer->Release();
        }
        m_Pointer = pointer;
    }

    // Gives up ownership without decrementing the count. Caller becomes responsible.
    [[nodiscard]] T* Detach() noexcept
    {
        T* result = m_Pointer;
        m_Pointer = nullptr;
        return result;
    }

    [[nodiscard]] T* Get() const noexcept
    {
        return m_Pointer;
    }
    [[nodiscard]] T& operator*() const noexcept
    {
        HP_ASSERT(m_Pointer != nullptr);
        return *m_Pointer;
    }
    [[nodiscard]] T* operator->() const noexcept
    {
        HP_ASSERT(m_Pointer != nullptr);
        return m_Pointer;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return m_Pointer != nullptr;
    }

    void Swap(Ref& other) noexcept
    {
        std::swap(m_Pointer, other.m_Pointer);
    }

    template <typename U>
    [[nodiscard]] Ref<U> CastTo() const noexcept
    {
        U* casted = dynamic_cast<U*>(m_Pointer);
        if (casted == nullptr)
        {
            return Ref<U>{};
        }
        casted->AddRef();
        return Ref<U>(casted);
    }

    template <typename U>
    [[nodiscard]] Ref<U> StaticCastTo() const noexcept
    {
        U* casted = static_cast<U*>(m_Pointer);
        if (casted != nullptr)
        {
            casted->AddRef();
        }
        return Ref<U>(casted);
    }

private:
    template <typename>
    friend class Ref;
    T* m_Pointer = nullptr;
};

template <typename A, typename B>
bool operator==(const Ref<A>& a, const Ref<B>& b) noexcept
{
    return a.Get() == b.Get();
}
template <typename A, typename B>
bool operator!=(const Ref<A>& a, const Ref<B>& b) noexcept
{
    return a.Get() != b.Get();
}
template <typename T>
bool operator==(const Ref<T>& ref, std::nullptr_t) noexcept
{
    return ref.Get() == nullptr;
}
template <typename T>
bool operator==(std::nullptr_t, const Ref<T>& ref) noexcept
{
    return ref.Get() == nullptr;
}

// Creates a reference-counted object. The returned Ref owns the initial reference (count 1).
template <typename T, typename... Args>
[[nodiscard]] inline Ref<T> CreateObject(Args&&... args)
{
    static_assert(std::is_base_of_v<RefCounted, T>, "CreateObject requires a RefCounted-derived type");
    return Ref<T>(new T(std::forward<Args>(args)...));
}

}  // namespace Hollowpoint::Core
