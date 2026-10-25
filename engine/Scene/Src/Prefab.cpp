#include <string>
#include <utility>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/ECS/World.h"
#include "Hollowpoint/Scene/LocalTransform.h"
#include "Hollowpoint/Scene/Prefab.h"
#include "Hollowpoint/Scene/SceneIO.h"
#include "Hollowpoint/Scene/TransformHierarchy.h"
#include "Hollowpoint/Serialization/JsonParser.h"

namespace Hollowpoint::Scene
{

namespace
{

//! Counts roots the same way the loader does: an entity is a root when it carries no "parent" key. Duplicated here
//! rather than reusing LoadScene's bookkeeping because the count is needed at *load* time (for asset metadata) while
//! the loader only reports it after a successful instantiation.
void CountEntitiesAndRoots(const JsonValue& document, Size& outEntities, Size& outRoots)
{
    outEntities = 0;
    outRoots = 0;

    const JsonValue* entityArray = document.FindMember("entities");
    if (entityArray == nullptr || !entityArray->IsArray())
    {
        return;
    }

    for (const JsonValue& entry : entityArray->AsArray())
    {
        ++outEntities;

        const bool hasParent =
            entry.IsObject() && entry.FindMember("parent") != nullptr && !entry.FindMember("parent")->IsNull();
        if (!hasParent)
        {
            ++outRoots;
        }
    }
}

//! Writes `transform` into the entity's LocalTransform, creating the component when the document did not include one.
//!
//! Creating it is necessary rather than optional: an entity with no LocalTransform inherits its parent's transform
//! exactly (see TransformHierarchy), so applying a spawn offset to such an entity has nowhere to go and the caller's
//! "spawn at position X" would silently do nothing — leaving the instance stacked at the parent's origin.
void ApplyRootTransform(World& world, Entity entity, const Transform& offset)
{
    LocalTransform local;
    if (const LocalTransform* existing = world.TryGetComponent<LocalTransform>(entity); existing != nullptr)
    {
        // authored.Concatenated(offset) puts the spawn offset *outside* the authored value, so the resulting world
        // transform is parentWorld * offset * authored. The order matters: offset.Concatenated(authored) would apply
        // the prefab's own transform to the spawn position, so spawning a mesh authored with a half-height offset and
        // then moving it would move it twice (or rotate the translation by the author's rotation).
        //
        // The authored value is kept rather than replaced: a prefab whose root is raised by half its height must stay
        // raised, or every instance would sit half-buried.
        local = LocalTransform::FromTransform(existing->ToTransform().Concatenated(offset));
    }
    else
    {
        local = LocalTransform::FromTransform(offset);
    }

    world.AddComponent<LocalTransform>(entity).SetFrom(local.ToTransform());
}

}  // namespace

// ---------------------------------------------------------------------------
// Prefab.
// ---------------------------------------------------------------------------

Prefab Prefab::FromSceneText(std::string_view text, std::string& outError)
{
    const Serialization::JsonDocument parsed = Serialization::ParseJson(text);

    if (!parsed.Success)
    {
        // Position first: a prefab asset can be thousands of lines and "invalid JSON" is not something an author can
        // act on.
        outError = "JSON syntax error at line " + std::to_string(parsed.Error.Line) + ", column " +
                   std::to_string(parsed.Error.Column) + ": " + parsed.Error.Message;
        return Prefab{};
    }

    return FromSceneDocument(parsed.Root, outError);
}

Prefab Prefab::FromSceneDocument(const JsonValue& document, std::string& outError)
{
    if (!document.IsObject())
    {
        outError = "prefab root is not a JSON object";
        return Prefab{};
    }

    // The version and structure checks live in the loader rather than being duplicated here, so a prefab cannot be
    // considered valid by one code path and refused by another. Validating here instead would mean two definitions of
    // "is this a scene", which is how format bugs become invisible.
    Prefab prefab;
    prefab.m_Document = document;  // Copied: the prefab owns its template independently of the caller's document.
    prefab.m_HasDocument = true;

    if (const JsonValue* name = prefab.m_Document.FindMember("name"); name != nullptr && name->IsString())
    {
        prefab.m_Name = name->AsString();
    }

    CountEntitiesAndRoots(prefab.m_Document, prefab.m_EntityCount, prefab.m_RootCount);

    outError.clear();
    return prefab;
}

// ---------------------------------------------------------------------------
// Instantiation.
// ---------------------------------------------------------------------------

InstantiateResult InstantiatePrefab(World& world, const Prefab& prefab, const InstantiateOptions& options)
{
    InstantiateResult result;

    if (!prefab.IsLoaded())
    {
        // A missing or corrupt asset at runtime is normal (content deleted after a level referenced it), so this is
        // reported rather than asserted. The caller decides whether that is fatal for the level.
        HP_LOG_WARNING("Scene", "Cannot instantiate prefab '{}': it is not loaded", prefab.GetName());
        return result;
    }

    if (!options.Parent.IsNull() && !world.IsAlive(options.Parent))
    {
        // Spawning into a dead handle would silently produce entities parented to whatever slot that index now holds —
        // the classic "spawned onto a recycled entity" bug where an object appears attached to unrelated geometry.
        HP_LOG_WARNING("Scene", "Cannot instantiate prefab '{}': the requested parent entity is not alive",
                       prefab.GetName());
        return result;
    }

    SceneLoadOptions loadOptions;
    // Always append: instantiating must never clear the world it is spawning into, whatever the caller's defaults were.
    loadOptions.ClearWorldFirst = false;
    loadOptions.AssignFreshIdentities = options.AssignFreshIdentities;
    loadOptions.Conversion = Serialization::ConversionOptions{};

    const SceneLoadReport report = LoadScene(world, prefab.GetDocument(), loadOptions);

    if (!report.Loaded)
    {
        for (const SceneLoadReport::Issue& issue : report.Errors)
        {
            result.Warnings.push_back("prefab is not loadable: " + issue.Message);
        }
        return result;
    }

    for (const SceneLoadReport::Issue& issue : report.Warnings)
    {
        result.Warnings.push_back(issue.Message);
    }

    result.Created = true;
    result.Entities = report.CreatedEntities;
    result.Roots = report.RootEntities;

    // Attach the instance's roots under the requested parent. Doing this after the load (rather than patching the
    // document) keeps the prefab untouched and means one shared document can be instantiated under a hundred parents.
    if (!options.Parent.IsNull())
    {
        for (const Entity& root : result.Roots)
        {
            world.SetParent(root, options.Parent);
        }
    }

    if (options.RootTransform.has_value() && !result.Roots.empty())
    {
        // No composition with the parent's transform here, deliberately. The roots were just attached to `Parent`, so
        // their world transform is already parentWorld * local; setting local = offset gives parentWorld * offset *
        // authored, which is exactly "offset relative to the parent". Folding in the parent's own transform a second
        // time would double-count it, and the symptom — spawned objects drifting further away the deeper they are
        // nested — reads as a bug in the transform system rather than here.
        for (const Entity& root : result.Roots)
        {
            ApplyRootTransform(world, root, *options.RootTransform);
        }
    }

    return result;
}

}  // namespace Hollowpoint::Scene
