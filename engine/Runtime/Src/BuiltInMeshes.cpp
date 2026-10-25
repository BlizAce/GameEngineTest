#include <utility>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Renderer/Geometry.h"
#include "Hollowpoint/Runtime/BuiltInMeshes.h"

namespace Hollowpoint::Runtime
{

namespace
{

//! The name separator inside a built-in path. Kept as a constant because Register() validates against it and Lookup()
//! must agree with it exactly — two spellings would let a name be registered that could never be resolved.
constexpr std::string_view kSeparator = ":";

}  // namespace

BuiltInMeshes::BuiltInMeshes()
{
    // The default set is registered here rather than at first use so a registry built for a test contains what a
    // runtime's would: an empty registry that "works" in isolation proves nothing about the paths a scene actually
    // references.
    Register("cube", Renderer::MakeCube(1.0f));
    Register("plane", Renderer::MakePlane(5.0f));

    // A second cube at a different scale, existing purely so a test can assert two distinct paths resolve to distinct
    // geometry. If both returned the same pointer, a resolver ignoring its argument would pass every other case here.
    Register("cube-large", Renderer::MakeCube(2.0f));
}

bool BuiltInMeshes::Register(std::string_view name, const Renderer::GeometryData& geometry)
{
    if (name.empty())
    {
        HP_LOG_WARNING("RUNTIME", "built-in mesh registered with an empty name; ignored");
        return false;
    }

    // A name carrying the separator again ("a:b") would produce a path whose suffix is ambiguous when parsed back, so
    // it is rejected at registration rather than producing a lookup that can never match.
    if (name.find(kSeparator) != std::string_view::npos)
    {
        HP_LOG_WARNING("RUNTIME", "built-in mesh name '{}' contains '{}'; names must be a single segment", name,
                       kSeparator);
        return false;
    }

    if (geometry.IsEmpty())
    {
        // Registered anyway: an empty mesh is Game's documented "drop this drawable" signal rather than a corrupt
        // registry, and refusing here would make a geometry-generation bug look like a missing asset.
        HP_LOG_WARNING("RUNTIME", "built-in mesh '{}' has no vertices", name);
    }

    const std::string key = std::string(kBuiltInScheme) + std::string(name);
    m_Meshes[key] = geometry;
    return true;
}

const Renderer::GeometryData* BuiltInMeshes::Lookup(std::string_view path) const noexcept
{
    if (!IsBuiltInPath(path))
    {
        // Not an error: a non-`engine:` path belongs to the asset system, which will exist. The collector logs a null
        // resolution by name, which is where a typo surfaces.
        return nullptr;
    }

    const auto found = m_Meshes.find(std::string(path));
    if (found == m_Meshes.end())
    {
        return nullptr;
    }
    return &found->second;
}

bool BuiltInMeshes::Contains(std::string_view path) const noexcept
{
    return Lookup(path) != nullptr;
}

Size BuiltInMeshes::GetCount() const noexcept
{
    return m_Meshes.size();
}

BuiltInMeshes& BuiltInMeshes::GetSingleton()
{
    // Function-local static: constructed on first use, destroyed at exit, and thread-safe to initialise under C++11. A
    // namespace-scope object would order-initialise before the log system in some link orders and could not report its
    // own registration warnings.
    static BuiltInMeshes instance;
    return instance;
}

const Renderer::GeometryData* BuiltInMeshes::Resolve(std::string_view path) noexcept
{
    return GetSingleton().Lookup(path);
}

}  // namespace Hollowpoint::Runtime
