#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Hollowpoint/Core/Assert.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/Hierarchy.h"
#include "Hollowpoint/MathReflection/MathReflection.h"
#include "Hollowpoint/Reflection/Registry.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Serialization/JsonParser.h"
#include "Hollowpoint/Serialization/JsonWriter.h"

namespace Hollowpoint::Scene
{

using ECS::Hierarchy;
using Reflection::Registry;
using Serialization::JsonArray;
using Serialization::JsonObject;

// The one scene-format version this build understands. A minor bump means "older files still load" (missing fields
// keep their defaults, which the bridge guarantees); a major bump means the structure changed enough that loading an
// old file would produce something plausible-but-wrong, so it is refused instead.
constexpr Core::UInt32 kSceneFormatVersion = 1;

namespace
{

[[nodiscard]] std::optional<Core::UInt32> ReadVersion(const JsonValue& document)
{
    const JsonValue* version = document.FindMember("formatVersion");
    if (version == nullptr)
    {
        return std::nullopt;
    }
    if (!version->IsNumber())
    {
        return std::nullopt;
    }
    return static_cast<Core::UInt32>(version->AsInt());
}

}  // namespace

// ---------------------------------------------------------------------------
// ComponentFactory.
// ---------------------------------------------------------------------------

ComponentFactory& ComponentFactory::Get()
{
    // Function-local static: registration order against the Reflection singleton must not depend on static
    // initialisation order across translation units, and this is the one idiom that guarantees it.
    static ComponentFactory instance;
    return instance;
}

void ComponentFactory::RegisterFactory(std::string_view typeName, std::function<void*(World&, Entity)> create,
                                       std::string_view reflectionTypeName)
{
    Entry entry;
    entry.Create = std::move(create);
    entry.ReflectionTypeName = std::string(reflectionTypeName.empty() ? typeName : reflectionTypeName);

    AddEntry(typeName, std::move(entry));
}

void ComponentFactory::AddEntry(std::string_view typeName, Entry entry)
{
    const std::string name(typeName);

    if (m_Factories.contains(name))
    {
        // Registering twice is a bug in the registration table rather than a runtime condition. Keeping the first is
        // harmless and deterministic; replacing it would make which factory wins depend on link order.
        HP_LOG_WARNING("Scene", "Component type '%s' registered twice; keeping the first factory", name.c_str());
        return;
    }

    m_Factories.emplace(name, std::move(entry));
}

bool ComponentFactory::IsRegistered(std::string_view typeName) const
{
    return m_Factories.contains(std::string(typeName));
}

std::vector<std::string> ComponentFactory::GetRegisteredNames() const
{
    std::vector<std::string> names;
    names.reserve(m_Factories.size());
    for (const auto& [name, entry] : m_Factories)
    {
        HP_UNUSED(entry);
        names.push_back(name);
    }

    // Sorted so an editor list and any tool that diffs "what can this build create" is stable across runs — the same
    // reason scene files are written in declaration order.
    std::sort(names.begin(), names.end());
    return names;
}

void* ComponentFactory::CreateOrGet(const World& world, Entity entity, std::string_view typeName,
                                    const TypeDescriptor*& outType) const
{
    outType = nullptr;

    const auto found = m_Factories.find(std::string(typeName));
    if (found == m_Factories.end())
    {
        return nullptr;
    }

    // The reflected type is resolved at load time rather than registration time so that registration order cannot
    // matter, matching how Reflection resolves struct fields lazily.
    const TypeDescriptor* type = Registry::Get().FindType(found->second.ReflectionTypeName);
    if (type == nullptr)
    {
        // A factory without a descriptor means the component registered a name with one system and not the other —
        // reported by name because that is what the author can grep for.
        HP_LOG_ERROR("Scene", "Component '%s' has a factory but no reflected type; cannot load it",
                     std::string(typeName).c_str());
        return nullptr;
    }

    // The stored callback needs a mutable world; the parameter is const because CreateOrGet does not otherwise modify
    // it and callers hold it as const during iteration.
    World& mutableWorld = const_cast<World&>(world);
    void* address = found->second.Create(mutableWorld, entity);
    if (address == nullptr)
    {
        return nullptr;
    }

    outType = type;
    return address;
}

// ---------------------------------------------------------------------------
// SceneLoadReport.
// ---------------------------------------------------------------------------

void SceneLoadReport::ReportToLog(const char* sourceName) const
{
    const char* name = sourceName != nullptr ? sourceName : "scene";

    for (const Issue& issue : Errors)
    {
        HP_LOG_ERROR("Scene", "%s: entity[%u]: %s", name, issue.EntityIndex, issue.Message.c_str());
    }
    for (const Issue& issue : Warnings)
    {
        HP_LOG_WARNING("Scene", "%s: entity[%u]: %s", name, issue.EntityIndex, issue.Message.c_str());
    }

    if (!Errors.empty() || !Warnings.empty())
    {
        HP_LOG_WARNING("Scene", "%s: loaded with %zu error(s) and %zu warning(s)", name, Errors.size(),
                       Warnings.size());
    }
}

// ---------------------------------------------------------------------------
// Save.
// ---------------------------------------------------------------------------

JsonValue WriteScene(const World& world, const SceneSaveOptions& options)
{
    // Ensuring registration here rather than relying on a static initialiser: this file lives in a static library, and
    // a translation unit whose symbols nobody references is simply not linked in — so its registrations would never
    // run, and scenes would save without transforms. An explicit idempotent call at each entry point cannot be dropped
    // by the linker and costs one atomic load in steady state.
    RegisterSceneTypes();

    JsonObject document;
    document.Set("formatVersion", JsonValue(static_cast<Core::Int64>(kSceneFormatVersion)));

    if (!options.SceneName.empty())
    {
        document.Set("name", JsonValue(options.SceneName));
    }

    // Assign a dense file-local id per live entity, in slot order. Slot order is deterministic and matches how the
    // world iterates, so saving twice produces identical bytes unless something actually changed — which is what keeps
    // level files diffable in version control.
    std::vector<Entity> entities;
    world.ForEachEntity([&entities](const Entity& entity) { entities.push_back(entity); });

    std::unordered_map<Core::Index, Core::Int64> fileIds;
    fileIds.reserve(entities.size());
    for (Size index = 0; index < entities.size(); ++index)
    {
        fileIds.emplace(entities[index].Slot, static_cast<Core::Int64>(index));
    }

    JsonArray entityArray;
    entityArray.reserve(entities.size());

    ConversionDiagnostics conversionErrors;

    for (Size index = 0; index < entities.size(); ++index)
    {
        const Entity& entity = entities[index];

        JsonObject entry;
        entry.Set("id", JsonValue(static_cast<Core::Int64>(index)));

        const Guid guid = world.GetGuid(entity);
        if (!guid.IsNil())
        {
            entry.Set("guid", JsonValue(guid.ToString()));
        }

        // Parent by file-local id. Roots omit the key entirely rather than writing null: most entities in a real
        // scene are nested one or two deep, and an explicit "parent": null on every root is noise that makes the
        // common case harder to read.
        const Entity parent = world.GetParent(entity);
        if (!parent.IsNull())
        {
            const auto parentFile = fileIds.find(parent.Slot);
            if (parentFile != fileIds.end())
            {
                entry.Set("parent", JsonValue(parentFile->second));
            }
            else
            {
                // The parent exists in the world but was not assigned an id, which cannot happen while ids come from
                // the same iteration. Reported because it means the hierarchy points at something this pass cannot see.
                HP_LOG_ERROR("Scene", "Entity slot {} has a parent that is not in the saved set; saving it as a root",
                             entity.Slot);
            }
        }

        JsonObject components;
        world.ForEachComponentStorage([&](const ECS::IComponentStorage& storage) {
            const void* address = storage.FindAddress(entity.Slot);
            if (address == nullptr)
            {
                return;
            }

            // The ECS identifies storages by C++ type info, while scene files use the short registered name — so
            // resolve through the registry's type-info index rather than FindType. Passing storage.GetTypeName() to
            // FindType looks right and matches nothing: it would silently skip every component in every scene, which
            // is precisely the kind of bug that survives review because saves still "succeed".
            const TypeDescriptor* type = Registry::Get().FindTypeByTypeInfoName(storage.GetTypeName());
            if (type == nullptr)
            {
                // A component with no descriptor cannot be described, so it is skipped with a message naming it.
                // Common during development when someone adds a struct and forgets HP_REGISTER_TYPE; silently dropping
                // it would mean a component that vanishes from saved levels.
                HP_LOG_WARNING("Scene", "Component '%s' has no reflected type; skipping it while saving",
                               storage.GetTypeName());
                return;
            }

            JsonValue encoded = Serialization::TypeToJson(address, *type, conversionErrors, options.Conversion);
            if (!encoded.IsNull())
            {
                // Keyed by the registered display name so files stay readable and the loader can look the name up in
                // the factory (which is keyed by the same string).
                components.Set(type->GetName(), std::move(encoded));
            }
        });

        if (!components.IsEmpty())
        {
            entry.Set("components", JsonValue(std::move(components)));
        }

        entityArray.push_back(JsonValue(std::move(entry)));
    }

    document.Set("entities", JsonValue(std::move(entityArray)));
    return JsonValue(std::move(document));
}

// ---------------------------------------------------------------------------
// Load.
// ---------------------------------------------------------------------------

namespace
{

//! One parsed entity, kept separate from the ECS until every entity exists (see the two-phase note in the header).
struct ParsedEntity
{
    Guid Identity;
    std::optional<Core::Int64> ParentId;
    std::vector<std::pair<std::string, JsonValue>> Components;
};

SceneLoadReport LoadDocument(World& world, const JsonValue& document, const SceneLoadOptions& options,
                             const char* sourceName)
{
    // Same reasoning as WriteScene: without this a scene would load reporting every LocalTransform as an unknown
    // component type.
    RegisterSceneTypes();

    SceneLoadReport report;

    if (!document.IsObject())
    {
        report.Loaded = false;
        report.Errors.push_back({0, "scene root is not a JSON object"});
        return report;
    }

    const std::optional<Core::UInt32> version = ReadVersion(document);
    if (!version.has_value())
    {
        // No version field: refuse rather than guess. Guessing means a future v2 file could be loaded as v1 and
        // produce a scene that looks fine but has the wrong structure.
        report.Loaded = false;
        report.Errors.push_back({0, "scene is missing a numeric 'formatVersion'"});
        return report;
    }

    if (*version > kSceneFormatVersion)
    {
        // Newer than this build. Refusing is correct: the bridge tolerates unknown fields, so loading a newer file
        // would succeed while quietly discarding whatever the new structure relies on — data loss reported as success.
        report.Loaded = false;
        report.Errors.push_back({0, "scene format version " + std::to_string(*version) +
                                        " is newer than this build understands (max " +
                                        std::to_string(kSceneFormatVersion) + ")"});
        return report;
    }

    const JsonValue* entityArray = document.FindMember("entities");
    if (entityArray == nullptr || !entityArray->IsArray())
    {
        report.Loaded = false;
        report.Errors.push_back({0, "scene has no 'entities' array"});
        return report;
    }

    const JsonArray& entitiesJson = entityArray->AsArray();

    // ---- Parse everything first. Nothing touches the world until the whole document is understood, so a file that
    // fails structurally leaves the current scene intact rather than half-replaced.
    std::vector<ParsedEntity> parsed;
    parsed.reserve(entitiesJson.size());

    for (Size index = 0; index < entitiesJson.size(); ++index)
    {
        const JsonValue& entry = entitiesJson[index];
        if (!entry.IsObject())
        {
            report.Errors.push_back({static_cast<Core::UInt32>(index), "entity entry is not an object"});
            parsed.push_back({});
            continue;
        }

        ParsedEntity entityData;

        if (const JsonValue* guidJson = entry.FindMember("guid"); guidJson != nullptr && guidJson->IsString())
        {
            // An unparseable identity is a warning, not fatal: the entity still loads with a fresh Guid. Losing a
            // whole level to one corrupted UUID character is worse than that entity being unreferencable.
            const std::optional<Guid> parsedGuid = Guid::Parse(guidJson->AsString());
            if (parsedGuid.has_value())
            {
                entityData.Identity = *parsedGuid;
            }
            else
            {
                report.Warnings.push_back({static_cast<Core::UInt32>(index),
                                           "invalid guid '" + guidJson->AsString() + "'; a new one was assigned"});
            }
        }

        if (const JsonValue* parentJson = entry.FindMember("parent"); parentJson != nullptr && !parentJson->IsNull())
        {
            if (parentJson->IsNumber())
            {
                entityData.ParentId = parentJson->AsInt();
            }
            else
            {
                report.Warnings.push_back(
                    {static_cast<Core::UInt32>(index), "parent must be an entity id; the entity will be a root"});
            }
        }

        if (const JsonValue* componentsJson = entry.FindMember("components"); componentsJson != nullptr)
        {
            if (!componentsJson->IsObject())
            {
                report.Errors.push_back({static_cast<Core::UInt32>(index), "'components' must be an object; ignored"});
            }
            else
            {
                // Insertion order (the file's own order) is preserved, so a component listed first is created first —
                // which matters when one component's setup expects another to exist.
                const JsonObject& object = componentsJson->AsObject();
                for (Size memberIndex = 0; memberIndex < object.GetCount(); ++memberIndex)
                {
                    entityData.Components.emplace_back(object.GetKey(memberIndex), *object.GetValueAt(memberIndex));
                }
            }
        }

        parsed.push_back(std::move(entityData));
    }

    if (report.HasErrors())
    {
        // Structural problems mean the document cannot be trusted as a whole. Leaving the world untouched is the
        // difference between "this level will not load" and "this level half loaded and something is missing".
        report.Loaded = false;
        return report;
    }

    // ---- Apply.
    if (options.ClearWorldFirst)
    {
        world.Clear();
    }

    std::vector<Entity> created;
    created.reserve(parsed.size());

    // Phase 1: create every entity, so any parent id can be resolved regardless of file order.
    for (Size index = 0; index < parsed.size(); ++index)
    {
        const ParsedEntity& entityData = parsed[index];

        Entity entity;
        if (options.AssignFreshIdentities)
        {
            // Prefab instantiation: the document's identities belong to the asset, not to this instance. Two copies
            // sharing one Guid would make every Guid-based reference resolve ambiguously.
            entity = world.CreateEntity();
        }
        else if (!entityData.Identity.IsNil())
        {
            // A duplicate identity in one file is reported and the entity gets a fresh Guid rather than failing the
            // load: the scene is still usable, and the alternative is refusing to open a level someone must fix.
            if (world.IsAlive(world.GetEntityByGuid(entityData.Identity)))
            {
                report.Warnings.push_back(
                    {static_cast<Core::UInt32>(index), "duplicate guid in scene; assigned a fresh identity"});
                entity = world.CreateEntity();
            }
            else
            {
                entity = world.CreateEntityWithGuid(entityData.Identity);
            }
        }
        else
        {
            entity = world.CreateEntity();
        }

        created.push_back(entity);
    }

    // Phase 2: components. Resolving them before parents keeps a component's setup from observing a half-built tree.
    for (Size index = 0; index < parsed.size(); ++index)
    {
        const ParsedEntity& entityData = parsed[index];
        const Entity entity = created[index];

        for (const auto& [typeName, componentJson] : entityData.Components)
        {
            const TypeDescriptor* type = nullptr;
            void* address = ComponentFactory::Get().CreateOrGet(world, entity, typeName, type);

            if (address == nullptr || type == nullptr)
            {
                // An unknown component name is a warning rather than an error. This is the normal signature of a scene
                // saved by a build that had a component this one does not (removed feature, disabled module), and
                // refusing to load would block opening levels for no reason. Named exactly, since "MeshRender" vs
                // "MeshRenderer" is otherwise invisible.
                report.Warnings.push_back(
                    {static_cast<Core::UInt32>(index), "unknown component type '" + typeName + "'; skipped"});
                continue;
            }

            ConversionDiagnostics conversionErrors;
            if (!Serialization::JsonToType(componentJson, address, *type, conversionErrors, options.Conversion))
            {
                for (const std::string& message : conversionErrors.Messages)
                {
                    report.Warnings.push_back({static_cast<Core::UInt32>(index), typeName + ": " + message});
                }
            }
        }
    }

    // Phase 3: parents. Ids are file-local, so a bad id can only ever be out of range or self-referential — both
    // reported and left as a root rather than aborting the load.
    for (Size index = 0; index < parsed.size(); ++index)
    {
        const ParsedEntity& entityData = parsed[index];
        if (!entityData.ParentId.has_value())
        {
            continue;
        }

        const Core::Int64 parentId = *entityData.ParentId;
        if (parentId < 0 || static_cast<Size>(parentId) >= created.size())
        {
            report.Warnings.push_back(
                {static_cast<Core::UInt32>(index),
                 "parent id " + std::to_string(parentId) + " is not in this file; the entity will be a root"});
            continue;
        }

        if (parentId == static_cast<Core::Int64>(index))
        {
            report.Warnings.push_back(
                {static_cast<Core::UInt32>(index), "entity cannot be its own parent; left as a root"});
            continue;
        }

        const Hierarchy::ReparentResult result = world.SetParent(created[index], created[static_cast<Size>(parentId)]);
        if (result != Hierarchy::ReparentResult::Success && result != Hierarchy::ReparentResult::Unchanged)
        {
            // A cycle written into the file. SetParent refuses it, which is exactly right — loading it would hang the
            // transform pass — but the author needs to know the file was bad rather than that their scene "looks odd".
            report.Warnings.push_back(
                {static_cast<Core::UInt32>(index),
                 std::string("could not reparent (") + Hierarchy::ReparentResultName(result) + "); left as a root"});
        }
    }

    // Roots are collected *before* the created list is moved out — reading a moved-from vector is not merely legal but
    // silently empty, which would make every prefab instance look root-less. Order matters here; do not "tidy" it.
    for (Size index = 0; index < parsed.size(); ++index)
    {
        if (!parsed[index].ParentId.has_value())
        {
            report.RootEntities.push_back(created[index]);
        }
    }

    // Only a fully successful load reports what it created: after a refused load the world is untouched, and handing
    // back handles to entities that were never made would be worse than handing back nothing.
    report.CreatedEntities = std::move(created);

    report.Loaded = true;

    if (sourceName != nullptr && (report.Errors.size() + report.Warnings.size()) > 0)
    {
        HP_LOG_INFO("Scene", "Loaded '%s' with %zu warning(s)", sourceName, report.Warnings.size());
    }

    return report;
}

}  // namespace

SceneLoadReport LoadScene(World& world, const JsonValue& document, const SceneLoadOptions& options)
{
    return LoadDocument(world, document, options, nullptr);
}

SceneLoadReport LoadSceneFromText(World& world, std::string_view text, const SceneLoadOptions& options)
{
    const Serialization::JsonDocument parsed = Serialization::ParseJson(text);

    if (!parsed.Success)
    {
        // A syntax error is reported through the same channel as a content error, so callers have exactly one place
        // to check instead of catching an exception here and reading a report there.
        SceneLoadReport report;
        report.Loaded = false;
        report.Errors.push_back({0, "JSON syntax error at line " + std::to_string(parsed.Error.Line) + ", column " +
                                        std::to_string(parsed.Error.Column) + ": " + parsed.Error.Message});
        return report;
    }

    return LoadDocument(world, parsed.Root, options, nullptr);
}

// ---------------------------------------------------------------------------
// Registration of the engine's own serialisable components.
// ---------------------------------------------------------------------------

namespace
{

std::atomic<bool> g_SceneTypesRegistered{false};
std::mutex g_SceneRegistrationMutex;

}  // namespace

void RegisterSceneTypes()
{
    if (g_SceneTypesRegistered.load(std::memory_order_acquire))
    {
        return;
    }

    const std::lock_guard<std::mutex> lock(g_SceneRegistrationMutex);
    if (g_SceneTypesRegistered.load(std::memory_order_relaxed))
    {
        return;
    }

    // Math types first: LocalTransform's fields are Vec3/Quat, and the bridge resolves a struct field's descriptor
    // lazily at encode time — so ordering is not strictly required here, but registering a base before anything that
    // refers to it keeps every path through this function obviously correct.
    Math::RegisterMathTypes();

    Reflection::Registry& registry = Reflection::Registry::Get();

    // Transforms are described flat (position/rotation/scale) because that is what the inspector draws and what a
    // hand-edited scene reads as. The component stores three members rather than one Math::Transform precisely so
    // member pointers can reach them — see LocalTransform.h.
    if (registry.FindType("LocalTransform") == nullptr)
    {
        registry.RegisterType<LocalTransform>("LocalTransform")
            .Field("position", &LocalTransform::Position)
            .Field("rotation", &LocalTransform::Rotation)
            .Field("scale", &LocalTransform::Scale);
    }

    ComponentFactory::Get().Register<LocalTransform>("LocalTransform");

    g_SceneTypesRegistered.store(true, std::memory_order_release);
}

bool AreSceneTypesRegistered()
{
    return g_SceneTypesRegistered.load(std::memory_order_acquire);
}

}  // namespace Hollowpoint::Scene
