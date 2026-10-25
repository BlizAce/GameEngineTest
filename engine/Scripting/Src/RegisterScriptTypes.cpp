#include <mutex>
#include <string_view>

#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Scripting/RegisterScriptTypes.h"
#include "Hollowpoint/Scripting/ScriptSystem.h"

namespace Hollowpoint::Scripting
{

using Reflection::Registry;
using Scene::ComponentFactory;

namespace
{

//! The name scene files use for ScriptComponent. Kept as a constant rather than inlined at both call sites: the type
//! registration and the factory registration must agree exactly, and SceneIO's failure mode when they do not is a
//! silently missing component (it skips unknown names), not an error.
constexpr std::string_view kSceneTypeName = "Script";

std::mutex g_RegistrationMutex;

}  // namespace

void RegisterScriptTypes()
{
    const std::lock_guard<std::mutex> lock(g_RegistrationMutex);

    Registry& registry = Registry::Get();
    if (registry.FindType(kSceneTypeName) == nullptr)
    {
        // One field only, and it is a name rather than a script path or an inline chunk of Lua. That is deliberate: the
        // VM binds by global table name (ScriptSystem::BindScript looks the name up in _G), so a scene says *which*
        // behaviour an entity has while the code that implements it stays in a .lua file the host controls — reloadable
        // without touching the scene, and identical between two scenes that share a behaviour.
        registry.RegisterType<ScriptComponent>(kSceneTypeName).Field("scriptName", &ScriptComponent::ScriptName);
    }

    ComponentFactory& factory = ComponentFactory::Get();
    if (!factory.IsRegistered(kSceneTypeName))
    {
        factory.Register<ScriptComponent>(kSceneTypeName);
    }
}

bool AreScriptTypesRegistered()
{
    // Both halves, for the reason documented on Game::TypeIsUsable: a descriptor without a factory entry parses but
    // instantiates nothing, and asking only about the descriptor would report that half-registered state as fine.
    Registry& registry = Registry::Get();
    return registry.FindType(kSceneTypeName) != nullptr && ComponentFactory::Get().IsRegistered(kSceneTypeName);
}

}  // namespace Hollowpoint::Scripting
