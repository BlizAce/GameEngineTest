#include "TestSupport.h"

#include <doctest/doctest.h>

#include "Hollowpoint/Core/RefCounted.h"

using namespace Hollowpoint::Core;
using Hollowpoint::Test::ScopedAssertCapture;

namespace
{

class Widget : public RefCounted
{
public:
    explicit Widget(int id = 0) : m_Id(id)
    {
        ++s_LiveWidgets;
    }
    ~Widget() override
    {
        --s_LiveWidgets;
    }

    [[nodiscard]] int GetId() const
    {
        return m_Id;
    }
    static int GetLiveCount()
    {
        return s_LiveWidgets;
    }

private:
    int m_Id = 0;
    inline static int s_LiveWidgets = 0;
};

class DerivedWidget : public Widget
{
public:
    explicit DerivedWidget(int id, int extra) : Widget(id), m_Extra(extra) {}
    [[nodiscard]] int GetExtra() const
    {
        return m_Extra;
    }

private:
    int m_Extra = 0;
};

}  // namespace

TEST_SUITE("Core/RefCounted")
{
    TEST_CASE("CreateObject starts with a reference count of one")
    {
        Ref<Widget> widget = CreateObject<Widget>(7);
        REQUIRE(widget != nullptr);
        CHECK(widget->GetId() == 7);
        CHECK(widget->GetReferenceCount() == 1);
    }

    TEST_CASE("copying a Ref increments the count, destruction decrements it")
    {
        {
            Ref<Widget> first = CreateObject<Widget>(1);
            {
                Ref<Widget> second = first;
                CHECK(first.Get() == second.Get());
                CHECK(first->GetReferenceCount() == 2);

                Ref<Widget> third;
                third = second;
                CHECK(third->GetReferenceCount() == 3);
            }
            CHECK(first->GetReferenceCount() == 1);
        }
        CHECK(Widget::GetLiveCount() == 0);
    }

    TEST_CASE("moving a Ref does not touch the count")
    {
        Ref<Widget> source = CreateObject<Widget>(2);
        const Int32 countBefore = source->GetReferenceCount();

        Ref<Widget> moved(std::move(source));
        CHECK(source == nullptr);
        CHECK(moved->GetReferenceCount() == countBefore);

        Ref<Widget> assigned;
        assigned = std::move(moved);
        CHECK(moved == nullptr);
        CHECK(assigned->GetReferenceCount() == countBefore);
    }

    TEST_CASE("self-assignment keeps the object alive")
    {
        Ref<Widget> widget = CreateObject<Widget>(3);

        // Deliberate self-assignment: a naive operator= that releases before adding the new reference
        // would destroy the object and then read freed memory. The assignment goes through a pointer so
        // that -Wself-assign-overloaded (Clang) does not reject the very case under test.
        Ref<Widget>* const self = &widget;
        *self = *self;

        CHECK(widget != nullptr);
        CHECK(widget->GetReferenceCount() == 1);
    }

    TEST_CASE("Reset and Detach transfer ownership")
    {
        Ref<Widget> widget = CreateObject<Widget>(4);
        Widget* raw = widget.Detach();
        REQUIRE(raw != nullptr);
        CHECK(widget == nullptr);
        CHECK(raw->GetReferenceCount() == 1);

        // The caller now owns the reference and must release it.
        raw->Release();
        CHECK(Widget::GetLiveCount() == 0);
    }

    TEST_CASE("upcasting and downcasting preserve const-correct ownership")
    {
        Ref<DerivedWidget> derived = CreateObject<DerivedWidget>(5, 6);

        Ref<Widget> base = derived;  // implicit up-conversion
        CHECK(base->GetReferenceCount() == 2);

        Ref<DerivedWidget> downcast = base.CastTo<DerivedWidget>();
        REQUIRE(downcast != nullptr);
        CHECK(downcast->GetExtra() == 6);
        CHECK(base->GetReferenceCount() == 3);

        // Casting to an unrelated type yields an empty Ref rather than a bogus pointer.
        class Unrelated : public RefCounted
        {};
        CHECK(base.CastTo<Unrelated>() == nullptr);
    }

    TEST_CASE("the object is destroyed exactly when the last owner releases")
    {
        CHECK(Widget::GetLiveCount() == 0);
        {
            Ref<Widget> a = CreateObject<Widget>();
            Ref<Widget> b = a;
            CHECK(Widget::GetLiveCount() == 1);
        }
        CHECK(Widget::GetLiveCount() == 0);
    }

    TEST_CASE("an unbalanced release is reported instead of corrupting the heap")
    {
        // The count is manipulated through a test-only subclass so that this test never frees memory
        // twice (which would be undefined behaviour even when the assert handler continues).
        class CountedWidget : public Widget
        {
        public:
            void SetReferenceCountForTest(Int32 count)
            {
                m_ReferenceCount.store(count, std::memory_order_relaxed);
            }
        };

        Ref<CountedWidget> widget = CreateObject<CountedWidget>();
        widget->SetReferenceCountForTest(0);  // simulate the "already dead" state

        ScopedAssertCapture capture;
        widget->AddRef();  // must be reported: adding a reference to a dead object is a bug
        CHECK(capture.GetCaptureCount() == 1);

        widget->SetReferenceCountForTest(1);  // restore balance so the Ref can release cleanly
    }
}
