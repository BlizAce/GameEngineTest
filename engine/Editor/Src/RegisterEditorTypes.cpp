#include <mutex>

#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Editor/EntityName.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Scene/SceneIO.h"

// ---------------------------------------------------------------------------
// Registration of editor-only scene types.
//
// The pattern is the one Game/RegisterGameTypes.cpp established, including its two failure-avoidance details:
// registration is guarded by querying the registry (not a once-flag, because ComponentFactory::Clear exists and a
// latch that survives a clear lies), and both halves — TypeDescriptor *and* factory entry — must be present for the
// type to count as registered, since a descriptor without a factory parses but cannot instantiate. The mutex is the
// same guard Game uses: registration mutates two process-global registries and the editor calls this from its own
// startup while tests may call it from another binary's thread at teardown-adjacent times; cheap and never contended.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Reflection::Registry;
using Scene::ComponentFactory;

namespace
{
std::mutex g_RegistrationMutex;
}  // namespace

void RegisterEditorTypes()
{
    const std::lock_guard<std::mutex> lock(g_RegistrationMutex);

    // Scene types (LocalTransform and the math structs) first: an EntityName entity almost always carries a
    // transform too, and idempotent means calling this unconditionally is cheaper than duplicating the guard.
    Scene::RegisterSceneTypes();

    Registry& registry = Registry::Get();
    if (registry.FindType("EntityName") == nullptr)
    {
        registry.RegisterType<EntityName>("EntityName").Field("name", &EntityName::Name);
    }

    ComponentFactory& factory = ComponentFactory::Get();
    if (!factory.IsRegistered("EntityName"))
    {
        factory.Register<EntityName>("EntityName");
    }
}

bool AreEditorTypesRegistered()
{
    Registry& registry = Registry::Get();
    return registry.FindType("EntityName") != nullptr && ComponentFactory::Get().IsRegistered("EntityName");
}

}  // namespace Hollowpoint::Editor
