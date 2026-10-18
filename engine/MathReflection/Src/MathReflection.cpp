#include <atomic>
#include <mutex>

#include "Hollowpoint/Math/Color.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/MathReflection/MathReflection.h"
#include "Hollowpoint/Reflection/Registry.h"

namespace Hollowpoint::Math
{

// ---------------------------------------------------------------------------
// Registration.
//
// Done imperatively rather than with HP_REGISTER_TYPE static initialisers, so that linking this library does not
// silently populate the global registry as a side effect of an unrelated translation unit being pulled in: the call is
// explicit, idempotent, and testable (AreMathTypesRegistered lets a tool assert what it depends on before relying on
// descriptors).
//
// The names are stable strings rather than compiler-derived type names for two reasons. Compiler output for template
// types differs across compilers, so "Hollowpoint::Math::Vec3<...>"-shaped names would make scene files
// compiler-specific; and a user component called `Vec3` could otherwise collide with an engine type in the registry,
// producing a load error that points nowhere near the cause. The dotted "Math." prefix costs nothing to read and ends
// both problems.
// ---------------------------------------------------------------------------

namespace
{

std::atomic<bool> g_Registered{false};
std::mutex g_RegistrationMutex;

}  // namespace

const char* GetVec2TypeName() noexcept
{
    return "Math.Vec2";
}

const char* GetVec3TypeName() noexcept
{
    return "Math.Vec3";
}

const char* GetVec4TypeName() noexcept
{
    return "Math.Vec4";
}

const char* GetQuatTypeName() noexcept
{
    return "Math.Quat";
}

const char* GetColorTypeName() noexcept
{
    return "Math.Color";
}

void RegisterMathTypes()
{
    if (g_Registered.load(std::memory_order_acquire))
    {
        return;
    }

    // Double-checked on a mutex rather than relying on the atomic alone: registration mutates shared registry state,
    // and two threads racing here would otherwise interleave descriptor writes. The fast path above is what keeps the
    // steady-state cost at one acquire load.
    const std::lock_guard<std::mutex> lock(g_RegistrationMutex);

    if (g_Registered.load(std::memory_order_relaxed))
    {
        return;
    }

    Reflection::Registry& registry = Reflection::Registry::Get();

    // Each type is registered only when absent. Registering twice would add a second descriptor under one name, and
    // FindType would then resolve to whichever the registry happens to reach first — a failure mode with no good error
    // message. The guard makes repeated calls (and repeated module init in tests) exactly a no-op.
    if (registry.FindType(GetVec2TypeName()) == nullptr)
    {
        registry.RegisterType<Vec2>(GetVec2TypeName()).Field("x", &Vec2::X).Field("y", &Vec2::Y);
    }

    if (registry.FindType(GetVec3TypeName()) == nullptr)
    {
        registry.RegisterType<Vec3>(GetVec3TypeName()).Field("x", &Vec3::X).Field("y", &Vec3::Y).Field("z", &Vec3::Z);
    }

    if (registry.FindType(GetVec4TypeName()) == nullptr)
    {
        registry.RegisterType<Vec4>(GetVec4TypeName())
            .Field("x", &Vec4::X)
            .Field("y", &Vec4::Y)
            .Field("z", &Vec4::Z)
            .Field("w", &Vec4::W);
    }

    // A quaternion is exposed as four components rather than an axis+angle or Euler triple. Encoding rotation in a
    // file as Euler angles bakes in a convention (order, handedness) every other tool must agree with; four numbers
    // carry no such ambiguity, and the editor composes whatever UI it prefers on top.
    if (registry.FindType(GetQuatTypeName()) == nullptr)
    {
        registry.RegisterType<Quat>(GetQuatTypeName())
            .Field("x", &Quat::X)
            .Field("y", &Quat::Y)
            .Field("z", &Quat::Z)
            .Field("w", &Quat::W);
    }

    // Colour is stored as RGBA floats rather than a packed 32-bit integer. A packed value looks smaller in a file but
    // bakes in a colour-space and channel-order assumption, and scene files are hand-edited — "0.8, 0.9, 1.0" reads as
    // a colour, an integer does not.
    if (registry.FindType(GetColorTypeName()) == nullptr)
    {
        registry.RegisterType<Color>(GetColorTypeName())
            .Field("r", &Color::R)
            .Field("g", &Color::G)
            .Field("b", &Color::B)
            .Field("a", &Color::A);
    }

    g_Registered.store(true, std::memory_order_release);
}

bool AreMathTypesRegistered()
{
    return g_Registered.load(std::memory_order_acquire);
}

}  // namespace Hollowpoint::Math
